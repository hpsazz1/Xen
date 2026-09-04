#!/usr/bin/env python3
"""验证 composite-phase plan seed/final freeze 的公开文件合同。"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode("utf-8")).hexdigest()


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(
        value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def load_fixture_module(path: pathlib.Path) -> Any:
    spec = importlib.util.spec_from_file_location("binder_fixture", path)
    if spec is None or spec.loader is None:
        raise AssertionError("无法加载 binder fixture")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True, type=pathlib.Path)
    parser.add_argument("--sequence-executable", required=True,
                        type=pathlib.Path)
    parser.add_argument("--binder", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--seal-executable", type=pathlib.Path)
    options = parser.parse_args()
    generator = options.generator.resolve()
    sequence_executable = options.sequence_executable.resolve()
    binder = options.binder.resolve()
    evaluator = options.evaluator.resolve()
    producer = options.producer.resolve()
    seal_executable = (options.seal_executable.resolve()
                       if options.seal_executable is not None else None)
    expect(all(path.is_file() for path in (
        generator, sequence_executable, binder, evaluator, producer)) and
        seal_executable is not None and seal_executable.is_file(),
        "plan generator 的公开依赖缺失")
    fixture = load_fixture_module(
        pathlib.Path(__file__).with_name(
            "mouse_effect_probe_b_composite_phase_binder_tests.py"))

    with tempfile.TemporaryDirectory(
            prefix="xen-composite-phase-plan-") as temporary:
        root = pathlib.Path(temporary)
        sequence_path = root / "sequence.json"
        generated = subprocess.run([
            str(sequence_executable), "--output", str(sequence_path),
            "--profile", "physical-b-composite-phase-calibration",
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(generated.returncode == 0 and sequence_path.is_file(),
               f"冻结 sequence 生成失败: {generated.stderr}")

        fixture_plan, capture, commands = fixture.build_inputs(
            binder, evaluator)
        policy_path = root / "capture-policy.json"
        write_json(policy_path, fixture_plan["capture_policy"])
        common = [
            sys.executable, str(generator),
            "--sequence", str(sequence_path),
            "--capture-policy", str(policy_path),
            "--binder", str(binder),
            "--evaluator", str(evaluator),
            "--producer", str(producer),
            "--report-verifier", str(seal_executable),
            "--run-uuid", fixture_plan["run_uuid"],
            "--activation-epoch", str(fixture_plan["activation_epoch"]),
            "--scope-id", fixture_plan["scope_id"],
        ]
        seed_path = root / "plan-seed.json"
        seed_result = subprocess.run(
            common + ["--output", str(seed_path)], check=False,
            capture_output=True, text=True, encoding="utf-8")
        expect(seed_result.returncode == 0 and seed_path.is_file(),
               f"plan seed 生成失败: {seed_result.stderr}")
        seed = json.loads(seed_path.read_text(encoding="utf-8"))
        semantic_input = dict(seed)
        claimed_seed = semantic_input.pop("plan_seed_semantic_sha256")
        expect(seed["status"] == "AWAITING_AUXILIARY_PREFLIGHT" and
               seed["frozen_at_utc_unix_ns"] is None and
               seed["scheduler_policy"]["preflight_file_sha256"] is None and
               seed["scheduler_policy"]["issue_lead_applies_to"] ==
                   "NONZERO_PULSE_ONLY" and
               seed["scheduler_policy"][
                   "negative_control_marker_lead_ns"] == 0 and
               claimed_seed == canonical_sha256(semantic_input),
               "Prepare 只能生成未揭示 response 的 plan seed")

        preflight_path = root / "scheduler-preflight.json"
        preflight = {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_b_composite_phase_scheduler_preflight",
            "status": "PASS",
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "run_uuid": fixture_plan["run_uuid"],
            "activation_epoch": fixture_plan["activation_epoch"],
            "sequence_semantic_sha256":
                json.loads(sequence_path.read_text(encoding="utf-8"))[
                    "sequence_sha256"],
        }
        write_json(preflight_path, preflight)
        final_path = root / "plan.json"
        final_result = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "9000000000000",
            "--output", str(final_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        final = json.loads(final_path.read_text(encoding="utf-8"))
        semantic_input = dict(final)
        claimed = semantic_input.pop("plan_semantic_sha256")
        expect(final_result.returncode == 0 and
               final["status"] == "FROZEN_BEFORE_CAPTURE" and
               final["scheduler_policy"]["preflight_file_sha256"] ==
                   fixture.file_sha256(preflight_path) and
               claimed == canonical_sha256(semantic_input) and
               final["sequence_binding"]["sample_count"] == 295 and
               final["sequence_binding"]["window_count"] == 42,
               "同机 preflight 后最终 plan 必须完整封存 sequence/scheduler")

        capture["plan_semantic_sha256"] = claimed
        capture["report_verifier_file_sha256"] = final["seal"][
            "report_verifier_file_sha256"]
        commands["plan_semantic_sha256"] = claimed
        fixture.seal_semantic(capture, "capture_semantic_sha256")
        fixture.seal_semantic(commands, "command_semantic_sha256")
        capture_path = root / "capture.json"
        command_path = root / "commands.json"
        evidence_path = root / "evidence.json"
        write_json(capture_path, capture)
        write_json(command_path, commands)
        bound = subprocess.run([
            sys.executable, str(binder), "--plan", str(final_path),
            "--capture", str(capture_path), "--commands", str(command_path),
            "--output", str(evidence_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(bound.returncode == 0 and evidence_path.is_file(),
               f"generator 最终 plan 必须被冻结 binder 接受: {bound.stderr}")

        if seal_executable is not None:
            expect(seal_executable.is_file(), "composite seal executable 缺失")
            duplicate_preflight_path = root / "duplicate-preflight.json"
            duplicate_plan_path = root / "duplicate-plan.json"
            duplicate = subprocess.run([
                str(seal_executable),
                "--plan-seed", str(seed_path.resolve()),
                "--sequence", str(sequence_path.resolve()),
                "--preflight-output", str(duplicate_preflight_path.resolve()),
                "--plan-output", str(duplicate_plan_path.resolve()),
                "--run-uuid", fixture_plan["run_uuid"],
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
            ], check=False, capture_output=True, text=True, encoding="utf-8")
            expect(duplicate.returncode == 2 and
                   not duplicate_preflight_path.exists() and
                   not duplicate_plan_path.exists(),
                   "composite seal 必须在 preflight 前拒绝重复参数")
            actual_preflight_path = root / "actual-preflight.json"
            actual_plan_path = root / "actual-plan.json"
            sealed = subprocess.run([
                str(seal_executable),
                "--plan-seed", str(seed_path.resolve()),
                "--sequence", str(sequence_path.resolve()),
                "--preflight-output", str(actual_preflight_path.resolve()),
                "--plan-output", str(actual_plan_path.resolve()),
                "--run-uuid", fixture_plan["run_uuid"],
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
            ], check=False, capture_output=True, text=True, encoding="utf-8")
            if sealed.returncode == 0:
                expect(actual_preflight_path.is_file() and
                       actual_plan_path.is_file(),
                       "preflight 通过后必须原子发布两份输出")
                actual_preflight = json.loads(
                    actual_preflight_path.read_text(encoding="utf-8"))
                actual_plan = json.loads(
                    actual_plan_path.read_text(encoding="utf-8"))
                actual_semantic_input = dict(actual_plan)
                actual_semantic = actual_semantic_input.pop(
                    "plan_semantic_sha256")
                expect(actual_preflight["status"] == "PASS" and
                       actual_preflight["physical_dispatch_count"] == 0 and
                       actual_preflight["event_count"] == 42 and
                       actual_preflight["observed_max_wake_lateness_ns"] <=
                           actual_preflight["max_wake_lateness_ns"] and
                       actual_plan["status"] == "FROZEN_BEFORE_CAPTURE" and
                       actual_plan["scheduler_policy"][
                           "preflight_file_sha256"] ==
                           fixture.file_sha256(actual_preflight_path) and
                       actual_semantic == canonical_sha256(
                           actual_semantic_input),
                       "seal 必须绑定真实 preflight 且保持 output-off/semantic")
            else:
                expect(sealed.returncode == 3 and
                       "scheduler preflight" in sealed.stderr and
                       not actual_preflight_path.exists() and
                       not actual_plan_path.exists(),
                       f"当前调度竞争超限时必须在输出前 fail closed: {sealed.stderr}")

        collided = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "9000000000000",
            "--output", str(final_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(collided.returncode == 2,
               "plan generator 不得覆盖既有最终 plan")

        bad_preflight = dict(preflight)
        bad_preflight["physical_dispatch_count"] = 1
        bad_path = root / "bad-preflight.json"
        write_json(bad_path, bad_preflight)
        rejected = subprocess.run(common + [
            "--preflight", str(bad_path),
            "--frozen-at-utc-unix-ns", "9000000000001",
            "--output", str(root / "bad-plan.json"),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(rejected.returncode == 2,
               "非 output-off preflight 必须 fail closed")

        zero_freeze = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "0",
            "--output", str(root / "zero-freeze-plan.json"),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(zero_freeze.returncode == 2,
               "最终 plan 的 freeze UTC 必须为正整数")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
