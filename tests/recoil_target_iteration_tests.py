"""固定目标迭代的离线数据合同测试，不使用设备或游戏。"""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys


SPEC = importlib.util.spec_from_file_location("recoil_target_iteration", Path(__file__).resolve().parents[1] /
                                             "scripts" / "recoil_target_iteration.py")
iteration = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(iteration)


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def write_rows(path, values):
    path.write_text("\n".join(json.dumps(value) for value in values) + "\n", encoding="utf-8")


class TargetIterationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.parent = self.root / "parent.json"
        write_json(self.parent, {"schema_version": 2, "id": "parent", "weapon_id": "weapon_ak47", "revision": 1,
                                 "sensitivity": 1.4, "verified": False,
                                 "points": [[0, 0, 0], [20, 0, 100], [40, 0, 200]]})
        self.registry = self.root / "usage.json"
        self.runs = [self.make_run(index, residue) for index, residue in enumerate([9, 10, 10, 11, 80])]

    def make_run(self, index, residual):
        root = self.root / f"run-{index}"
        root.mkdir()
        manifest = {"schema_version": 1, "run_id": f"run-{index}", "parent_profile_sha256": iteration.sha256(self.parent),
                    "environment_fingerprint": "fixed-environment", "target_id": "target", "anchor_id": "anchor",
                    "firing_id": f"fire-{index}", "device_epoch": "device-1", "weapon_id": "weapon_ak47",
                    "split": "fit", "generation": 0, "clock_domain": "steady", "fire_start_us": index * 1000000,
                    "window_end_us": 40000, "max_shots": 5, "observed_shots": 2,
                    "sensitivity": 1.4, "x_strength": 1, "y_strength": 1,
                    "alignment_uncertainty_us": 100, "max_alignment_uncertainty_us": 1000}
        commands = [{"command_id": str(i), "firing_id": manifest["firing_id"], "device_epoch": "device-1",
                     "t_us": i * 20000, "phase": "firing", "status": "backend_completed", "saturated": False,
                     "ff_dx": 0, "ff_dy": 100, "fb_dx": 0, "fb_dy": residual, "dx": 0, "dy": 100 + residual}
                    for i in (1, 2)]
        frames = []
        for i, t in enumerate([0, 20000, 40000]):
            raw = root / f"frame-{i}.raw"
            raw.write_bytes(f"synthetic-{index}-{i}".encode())
            frames.append({"frame_id": str(i), "t_us": t, "target_id": "target", "anchor_id": "anchor",
                           "valid": True, "background_consistent": True, "error_x": 0, "error_y": 1,
                           "raw_path": raw.name, "raw_sha256": iteration.sha256(raw),
                           "source_time_basis": "receive_steady_not_exposure"})
        write_rows(root / "frames.jsonl", frames)
        write_rows(root / "commands.jsonl", commands)
        write_rows(root / "events.jsonl", [{"type": "start", "event_loss_count": 0},
                                            {"type": "end", "event_loss_count": 0, "reason": "completed",
                                             "cleanup_complete": True}])
        write_json(root / "review.json", {"split": "fit", "integrity_pass": True, "identity_pass": True})
        write_json(root / "manifest.json", manifest)
        self.rehash(root)
        return root

    def rehash(self, run):
        manifest = iteration.read_json(run / "manifest.json")
        manifest["files"] = {name: iteration.sha256(run / name) for name in iteration.FILES}
        write_json(run / "manifest.json", manifest)

    def change_manifest(self, run, **changes):
        manifest = iteration.read_json(run / "manifest.json")
        manifest.update(changes)
        write_json(run / "manifest.json", manifest)

    def change_commands(self, mutate):
        commands = iteration.rows(self.runs[0] / "commands.jsonl")
        mutate(commands)
        write_rows(self.runs[0] / "commands.jsonl", commands)
        self.rehash(self.runs[0])

    def fit(self, **kwargs):
        return iteration.fit_runs(self.runs, self.parent, self.registry, **kwargs)

    def test_median_and_mad_add_parent_exactly_once(self):
        report = self.fit(alpha=1)
        last = report["trajectory"][-1]
        self.assertEqual(last["residual_median_counts"], [0, 20])
        self.assertEqual(last["residual_mad_counts"], [0, 2])
        self.assertEqual(last["algebraic_parent_plus_residual_counts"], [0, 220])
        self.assertIsNone(report["candidate"])
        self.assertFalse(report["executable_candidate_eligible"])
        self.assertFalse(report["physical_verified"])

    def test_new_generation_does_not_reapply_old_residual(self):
        parent = iteration.read_json(self.parent)
        parent["points"] = [[0, 0, 0], [20, 0, 110], [40, 0, 220]]
        write_json(self.parent, parent)
        for run in self.runs:
            self.change_manifest(run, generation=1, parent_profile_sha256=iteration.sha256(self.parent))
            commands = iteration.rows(run / "commands.jsonl")
            for command in commands:
                command.update(ff_dy=110, fb_dy=3, dy=113)
            write_rows(run / "commands.jsonl", commands)
            self.rehash(run)
        last = self.fit(alpha=1)["trajectory"][-1]
        self.assertEqual(last["algebraic_parent_plus_residual_counts"], [0, 226])

    def test_no_timing_calibration_never_exports_profile(self):
        report = self.fit()
        self.assertNotIn("points", report)
        self.assertIn("时序", report["reasons"][0])
        self.assertTrue(iteration.inspect_run(self.runs[0])["analysis_eligible"])

    def test_missing_data_rejected(self):
        (self.runs[0] / "frames.jsonl").unlink()
        self.assertFalse(iteration.inspect_run(self.runs[0])["analysis_eligible"])

    def test_malformed_record_rejected_without_traceback(self):
        write_rows(self.runs[0] / "commands.jsonl", [["invalid"]])
        self.rehash(self.runs[0])
        self.assertFalse(iteration.inspect_run(self.runs[0])["analysis_eligible"])

    def test_hash_mismatch_rejected(self):
        (self.runs[0] / "frame-1.raw").write_bytes(b"changed")
        self.assertFalse(iteration.inspect_run(self.runs[0])["analysis_eligible"])

    def test_different_parent_rejected(self):
        self.change_manifest(self.runs[0], parent_profile_sha256="different")
        with self.assertRaisesRegex(iteration.ContractError, "同一父"):
            self.fit()

    def test_holdout_not_fit(self):
        run = self.runs[0]
        self.change_manifest(run, split="holdout")
        review = iteration.read_json(run / "review.json")
        review["split"] = "holdout"
        write_json(run / "review.json", review)
        self.rehash(run)
        with self.assertRaisesRegex(iteration.ContractError, "禁止参与"):
            self.fit()

    def test_registry_prevents_holdout_relabel(self):
        run = self.runs[0]
        original = iteration._load_run(run)
        original["manifest"]["split"] = "holdout"
        iteration.register_usage(self.registry, [original])
        with self.assertRaisesRegex(iteration.ContractError, "用途被改写"):
            self.fit()

    def test_registry_prevents_renamed_duplicate(self):
        run = iteration._load_run(self.runs[0])
        iteration.register_usage(self.registry, [run])
        run["manifest"]["run_id"] = "renamed"
        with self.assertRaisesRegex(iteration.ContractError, "重命名"):
            iteration.register_usage(self.registry, [run])

    def test_unknown_completion_rejected(self):
        self.change_commands(lambda commands: commands[0].update(status="unknown"))
        with self.assertRaisesRegex(iteration.ContractError, "未知完成"):
            self.fit()

    def test_zero_net_ledger_does_not_claim_backend_receipt(self):
        self.change_commands(lambda commands: commands.insert(0, {**commands[0], "command_id": "zero", "t_us": 1000,
                              "ff_dy": 0, "fb_dy": 0, "dy": 0, "status": "no_op", "zero_command": True,
                              "backend_completed_ns": None}))
        self.assertEqual(self.fit()["zero_net_ledger_rows"], 1)

    def test_zero_net_cannot_hide_real_movement(self):
        self.change_commands(lambda commands: commands[0].update(status="no_op", zero_command=True))
        with self.assertRaisesRegex(iteration.ContractError, "不能伪造"):
            self.fit()

    def test_saturation_rejected(self):
        self.change_commands(lambda commands: commands[0].update(saturated=True))
        with self.assertRaisesRegex(iteration.ContractError, "饱和"):
            self.fit()

    def test_conservation_rejected(self):
        self.change_commands(lambda commands: commands[0].update(dy=999))
        with self.assertRaisesRegex(iteration.ContractError, "不守恒"):
            self.fit()

    def test_duplicate_receipt_rejected(self):
        self.change_commands(lambda commands: commands.append(dict(commands[-1])))
        with self.assertRaisesRegex(iteration.ContractError, "重复命令"):
            self.fit()

    def test_missing_feedforward_rejected(self):
        self.change_commands(lambda commands: commands[0].update(ff_dy=90, dy=99))
        with self.assertRaisesRegex(iteration.ContractError, "遗漏的基线"):
            self.fit()

    def test_alignment_uncertainty_rejected(self):
        self.change_manifest(self.runs[0], alignment_uncertainty_us=None)
        with self.assertRaisesRegex(iteration.ContractError, "不确定度"):
            self.fit()

    def test_registered_software_timing_band_allows_bounded_lag(self):
        self.change_manifest(self.runs[0], ff_schedule_tolerance_us=2000)
        self.change_commands(lambda commands: commands[0].update(ff_dy=90, dy=99))
        self.assertIsNone(self.fit()["candidate"])

    def test_software_timing_band_cannot_cover_unbounded_lag(self):
        self.change_manifest(self.runs[0], ff_schedule_tolerance_us=1000)
        self.change_commands(lambda commands: commands[0].update(ff_dy=90, dy=99))
        with self.assertRaisesRegex(iteration.ContractError, "遗漏的基线"):
            self.fit()

    def test_window_excludes_recovery(self):
        self.change_commands(lambda commands: commands.append({**commands[-1], "command_id": "recovery",
                              "phase": "recovery", "t_us": 50000, "ff_dy": 0, "fb_dy": 999, "dy": 999}))
        self.assertEqual(self.fit(alpha=1)["trajectory"][-1]["residual_median_counts"], [0, 20])

    def test_same_run_not_five_independent_runs(self):
        self.runs = [self.runs[0]] * 5
        with self.assertRaisesRegex(iteration.ContractError, "重复 Run"):
            self.fit()

    def test_cli_refuses_existing_output(self):
        output = self.root / "existing.json"
        output.write_text("keep", encoding="utf-8")
        code = iteration.main(["fit", *map(str, self.runs), "--parent", str(self.parent),
                               "--registry", str(self.registry), "--output", str(output)])
        self.assertEqual(code, 2)
        self.assertEqual(output.read_text(), "keep")

    def test_cli_failure_writes_new_machine_readable_report(self):
        output = self.root / "refused.json"
        self.change_commands(lambda commands: commands[0].update(status="unknown"))
        code = iteration.main(["fit", *map(str, self.runs), "--parent", str(self.parent),
                               "--registry", str(self.registry), "--output", str(output)])
        report = iteration.read_json(output)
        self.assertEqual(code, 2)
        self.assertFalse(report["analysis_eligible"])
        self.assertIsNone(report["candidate"])
        self.assertIn("未知完成", report["error"])

    def test_cannot_modify_registry_while_locked(self):
        with iteration.exclusive_lock(self.registry.with_suffix(".json.lock"), "locked"):
            with self.assertRaisesRegex(iteration.ContractError, "正被使用"):
                self.fit()

    def test_process_termination_releases_os_lock(self):
        lock = self.registry.with_suffix(".json.lock")
        child_code = """import importlib.util, pathlib, sys, time
spec = importlib.util.spec_from_file_location('iteration', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with module.exclusive_lock(pathlib.Path(sys.argv[2]), 'locked'):
    print('locked', flush=True)
    time.sleep(30)
"""
        child = subprocess.Popen([sys.executable, "-c", child_code, str(Path(iteration.__file__).resolve()), str(lock)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), "locked")
            with self.assertRaises(iteration.ContractError):
                self.fit()
            child.kill()
            child.wait(timeout=5)
            self.assertTrue(lock.exists())
            self.assertTrue(self.fit()["analysis_eligible"])
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            child.stdout.close()
            child.stderr.close()

    def test_review_checks_data_and_preserves_raw_files(self):
        run = self.runs[0]
        review = iteration.read_json(run / "review.json")
        review.update(integrity_pass=False, identity_pass=False)
        write_json(run / "review.json", review)
        self.rehash(run)
        before = {name: iteration.sha256(run / name) for name in iteration.FILES[:3]}
        self.assertFalse(iteration.inspect_run(run)["analysis_eligible"])
        report = iteration.review_run(run, self.registry, "目标身份未变，效果尚待验证", identity_confirmed=True)
        self.assertTrue(report["analysis_eligible"])
        self.assertFalse(report["physical_verified"])
        self.assertEqual(before, {name: iteration.sha256(run / name) for name in iteration.FILES[:3]})
        self.assertEqual(len(list((run / "review_history").glob("*.json"))), 3)
        # 同一原始记录后续复核允许增加审核版本，不允许改用途。
        iteration.review_run(run, self.registry, "补充人工观察", identity_confirmed=True)
        self.fit()

    def test_review_cannot_override_broken_completion(self):
        self.change_commands(lambda commands: commands[0].update(status="unknown"))
        with self.assertRaisesRegex(iteration.ContractError, "未知完成"):
            iteration.review_run(self.runs[0], self.registry, "人工看起来正常", identity_confirmed=True)
        self.assertFalse((self.runs[0] / "review_history").exists())

    def test_review_requires_explicit_identity(self):
        with self.assertRaisesRegex(iteration.ContractError, "明确确认"):
            iteration.review_run(self.runs[0], self.registry, "观察")

    def test_frozen_run_metadata_cannot_be_changed(self):
        iteration.register_usage(self.registry, [iteration._load_run(self.runs[0])])
        self.change_manifest(self.runs[0], alignment_uncertainty_us=0)
        with self.assertRaisesRegex(iteration.ContractError, "被改写"):
            self.fit()

    def test_capture_schema_normalization(self):
        run = self.runs[0]
        m = iteration.read_json(run / "manifest.json")
        start = 1000000000
        m.update(schema="recoil_target_iteration_v1", baseline_sha256=m["parent_profile_sha256"],
                 template_sha256="anchor", fire=True, software_fire_start_ns=start,
                 window_end_ns=start + 40000000, target_shots=5)
        for key in ("parent_profile_sha256", "target_id", "anchor_id", "fire_start_us", "window_end_us", "max_shots"):
            del m[key]
        write_json(run / "manifest.json", m)
        commands = iteration.rows(run / "commands.jsonl")
        write_rows(run / "commands.jsonl", [{"command_id": index, "firing_id": 1, "device_epoch": "device-1",
                  "phase": "fire", "backend_completed_ns": start + row["t_us"] * 1000, "status": "completed",
                  "planned_ns": start + row["t_us"] * 1000 - 100,
                  "calculated_ns": start + row["t_us"] * 1000 - 100,
                  "call_started_ns": start + row["t_us"] * 1000 - 100,
                  "returned_ns": start + row["t_us"] * 1000 + 100, "queue_inventory": 0,
                  "saturated": False, "ff_completed": [row["ff_dx"], row["ff_dy"]],
                  "fb_completed": [row["fb_dx"], row["fb_dy"]], "total": [row["dx"], row["dy"]]}
                  for index, row in enumerate(commands)])
        frames = iteration.rows(run / "frames.jsonl")
        write_rows(run / "frames.jsonl", [{**frame, "received_ns": start + frame["t_us"] * 1000,
                   "identity_sha256": "anchor", "error_px": [0, 1], "anchor_valid": True,
                   "phase": "active", "source_dropped_frames": 0, "transport_dropped_frames": 0} for frame in frames])
        self.rehash(run)
        self.assertTrue(iteration.inspect_run(run)["analysis_eligible"])
        captured_frames = iteration.rows(run / "frames.jsonl")
        captured_frames[0]["phase"] = "prepare"
        for frame in captured_frames:
            frame["source_dropped_frames"] = 3
        write_rows(run / "frames.jsonl", captured_frames)
        self.rehash(run)
        self.assertTrue(iteration.inspect_run(run)["analysis_eligible"])
        captured_frames[-1]["source_dropped_frames"] = 4
        write_rows(run / "frames.jsonl", captured_frames)
        self.rehash(run)
        self.assertFalse(iteration.inspect_run(run)["analysis_eligible"])


if __name__ == "__main__":
    unittest.main()
