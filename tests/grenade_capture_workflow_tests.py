"""PgUp/PgDn独立制作离线闭环：合成PNG、本地接收目录及回环HTTP，不采屏/按键/连接辅机。"""

from copy import deepcopy
import hashlib
import http.server
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.grenade_capture import capture_workflow, worker
from tools.grenade_capture.capture_workflow import (
    ContextReader, LocalSavePending, capture_once, short_name, standing_for, transfer_capture, validate_context)
from tools.grenade_capture.reference_assets import load_reference, manifest_revision
from tools.grenade_capture.reference_tool import add_image, begin
from tools.grenade_capture.screen_capture import png_rgb


class CaptureTests(unittest.TestCase):
    def setUp(self):
        fixtures = Path(__file__).resolve().parents[1] / "temp/XEN-GRENADE-TEXTURE-SPAWN-20261028-001/backend-tests"
        fixtures.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="run-", dir=fixtures)
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root, self.remote = self.base / "host", self.base / "auxiliary"
        self.standing, self.aim = self.base / "standing.png", self.base / "aim.png"
        self.standing.write_bytes(png_rgb(800, 600, [bytes([60, 120, 40]) * 800] * 600))
        self.aim.write_bytes(png_rgb(640, 360, [bytes([30, 80, 160]) * 640] * 360))
        self.plan = {"schema_version": 3, "name": "Mirage T1 VIP R2", "map": "de_mirage", "voice_command": "",
                     "location": "T1", "notes": "原瞄角与动作保持", "mask": {"wasd": 0, "start_ms": -1, "duration_ms": 0},
                     "actions": [{"type": "button", "button": "left", "start_ms": 0, "duration_ms": 200}]}
        (self.remote / "plans").mkdir(parents=True)
        self.plan_path = self.remote / "plans/mirage-t1-vip-r2.json"
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False, indent=2), encoding="utf-8")
        self.context = {"ready": True, "reason": "", "map": "de_mirage", "team": "T", "generation": "service-1",
                        "remaining_ms": 2500, "plan_id": self.plan_path.name, "plan": self.plan,
                        "plan_sha256": hashlib.sha256(self.plan_path.read_bytes()).hexdigest()}
        self.capture_count = 0

    def fake_capture(self, path):
        self.capture_count += 1
        shutil.copyfile(self.aim, path)

    def complete(self, reader=None):
        return capture_once(reader or (lambda: deepcopy(self.context)), self.root, self.fake_capture, self.standing)

    def capture_standing(self):
        return capture_once(lambda: deepcopy(self.context), self.root,
                            capture=lambda path: shutil.copyfile(self.standing, path), role="standing")

    def test_short_names_preserve_alternative_and_existing_position(self):
        self.assertEqual(short_name("Mirage T1 VIP R2", "de_mirage"), "T1 VIP烟")
        self.assertEqual(short_name("Mirage T1 VIP 备用瞄角 R1", "de_mirage"), "T1 VIP烟 备用瞄角")
        self.assertEqual(short_name("Mirage T2 B小烟 R1", "de_mirage"), "T2 B小烟")
        self.assertEqual(short_name("Ancient T来源5号 中路烟 [S102]", "de_ancient"), "T5 中路烟")
        self.assertEqual(short_name("未知站位 A烟"), "未知站位 A烟")

    def test_crouch_context_requires_explicit_boolean(self):
        for pose in (True, False):
            value = deepcopy(self.context)
            value["plan"]["crouch"] = pose
            self.assertIs(validate_context(value), value)
        for pose in (None, 0, 1, "蹲姿", []):
            value = deepcopy(self.context)
            value["plan"]["crouch"] = pose
            with self.subTest(pose=pose), self.assertRaisesRegex(ValueError, "采集上下文字段无效"):
                validate_context(value)

    def test_explicit_team_context_matches_and_rejects_invalid_or_mismatched_team(self):
        value = deepcopy(self.context)
        value["plan"]["team"] = "T"
        self.assertIs(validate_context(value), value)
        value["plan"]["team"] = ""
        self.assertIs(validate_context(value), value)
        for team in ("CT", "t", "SPECTATOR", None, 1, ["T"]):
            with self.subTest(team=team):
                value = deepcopy(self.context)
                value["plan"]["team"] = team
                with self.assertRaisesRegex(ValueError, "采集上下文字段无效"):
                    validate_context(value)

    def test_spawn_group_prefers_explicit_team_without_grouping_custom_points(self):
        self.assertEqual(capture_workflow.spawn_group(self.plan), "de_mirage-T")
        self.assertEqual(capture_workflow.spawn_group({**self.plan, "team": ""}), "de_mirage-T")
        self.assertEqual(capture_workflow.spawn_group({**self.plan, "team": "CT"}), "de_mirage-CT")
        self.assertIsNone(capture_workflow.spawn_group({**self.plan, "name": "T垃圾桶左 拱门烟", "team": "T"}))
        for team in ("SPECTATOR", None, ["T"]):
            with self.subTest(team=team):
                self.assertIsNone(capture_workflow.spawn_group({**self.plan, "team": team}))

    def test_explicit_ct_does_not_reuse_mirage_t_fallback_picture(self):
        self.plan["team"] = "CT"
        self.context["team"] = "CT"
        self.complete()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertNotIn("standing_image", manifest)
        self.assertNotIn("standing_marker", manifest)
        self.assertFalse((directory / "standing.png").exists())

    def test_spawn_standing_group_uses_explicit_team_before_name(self):
        def add(name, identity, team):
            value = self.add_plan(name, identity)
            value["plan"]["team"] = team
            path = self.remote / "plans" / value["plan_id"]
            path.write_text(json.dumps(value["plan"], ensure_ascii=False), encoding="utf-8")
            return value["plan_id"]
        same_team = add("CT2 B小烟", "mirage-ct2-explicit-t", "T")
        other_team = add("T2 B小烟", "mirage-t2-explicit-ct", "CT")
        custom = add("T垃圾桶左 拱门烟", "mirage-custom-t", "T")
        self.plan["team"] = "T"
        captures = capture_workflow.share_spawn_standing(self.root, self.context, self.standing,
                                                         self.remote / "plans")
        self.assertEqual({item["plan_id"] for item in captures}, {self.plan_path.name, same_team})
        self.assertFalse((self.root / "references" / other_team[:-5]).exists())
        self.assertFalse((self.root / "references" / custom[:-5]).exists())

    def test_non_de_map_custom_point_capture_transfer_round_trip(self):
        self.plan.update(name="T垃圾桶左 拱门烟", map="cs_office", team="T")
        self.context["map"] = "cs_office"
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False), encoding="utf-8")
        self.context["plan_sha256"] = hashlib.sha256(self.plan_path.read_bytes()).hexdigest()
        captured = self.complete()
        self.assertTrue(captured["package"].startswith("packages/cs_office/"))
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        manifest, directory = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual(manifest["plan"], self.plan)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertNotIn("standing_image", manifest)
        self.assertEqual((self.remote / captured["package"]).read_bytes(),
                         (self.root / captured["package"]).read_bytes())
        for map_name in ("cs_office", "ar_pool_day", "workshop_arena_1"):
            with self.subTest(map=map_name):
                self.assertEqual(capture_workflow.spawn_group({**self.plan, "name": "T1 烟", "map": map_name}),
                                 map_name + "-T")

    def test_f10_capture_preserves_originals_and_packages_matching_name(self):
        captured = self.complete()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem, self.plan_path.name)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual(manifest["plan"], self.plan)
        self.assertEqual(manifest["display_name"], "T1 VIP烟")
        self.assertEqual(manifest["confirmation_source"], "host_pgdn")
        self.assertTrue(manifest["manual_throw_verified"])
        self.assertTrue(manifest["capture_complete"])
        self.assertEqual(manifest["standing_image"], "standing.png")
        self.assertEqual(manifest["standing_marker"]["label"], "T1")
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual((manifest["geometry"]["source_width"], manifest["geometry"]["source_height"]), (640, 360))
        archive = self.root / captured["package"]
        self.assertEqual(archive.name, "T1 VIP烟.zip")
        with zipfile.ZipFile(archive) as package:
            self.assertEqual(len(package.namelist()), 5)
            self.assertIsNone(package.testzip())
            self.assertEqual(json.loads(package.read("plans/" + self.plan_path.name)), self.plan)

    def test_f9_first_saves_and_transfers_only_standing_without_claiming_aim(self):
        captured = self.capture_standing()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem, allow_partial=True)
        self.assertFalse(manifest["capture_complete"])
        self.assertFalse(manifest["manual_throw_verified"])
        self.assertNotIn("aim_image", manifest)
        self.assertNotIn("geometry", manifest)
        self.assertNotIn("confirmed_at_utc", manifest)
        self.assertEqual(captured["capture_role"], "standing")
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.assertFalse((directory / "aim.png").exists())
        with self.assertRaisesRegex(ValueError, "瞄点尚未采集"):
            load_reference(self.root / "references", self.plan_path.stem)
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        remote, target = load_reference(self.remote / "references", self.plan_path.stem, allow_partial=True)
        self.assertEqual(remote, manifest)
        self.assertEqual((target / "standing.png").read_bytes(), self.standing.read_bytes())
        with zipfile.ZipFile(self.root / captured["package"]) as package:
            self.assertFalse(any(name.endswith("aim.png") for name in package.namelist()))
            self.assertEqual(len(package.namelist()), 4)

    def add_plan(self, name, identity, map_name="de_mirage"):
        plan = deepcopy(self.plan)
        plan["name"], plan["map"] = name, map_name
        path = self.remote / "plans" / (identity + ".json")
        path.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
        return {**deepcopy(self.context), "plan": plan, "map": map_name, "plan_id": path.name,
                "plan_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def test_spawn_standing_updates_same_map_team_preserving_every_aim(self):
        second = self.add_plan("T2 B小烟", "mirage-t2-catwalk")
        third = self.add_plan("T10 VIP烟", "mirage-t10-vip")
        self.add_plan("CT1 B小烟", "mirage-ct1-catwalk")
        self.add_plan("T1 B小烟", "dust-t1-catwalk", "de_dust2")
        self.add_plan("B小 B烟", "mirage-catwalk-b")
        self.complete(lambda: deepcopy(second))
        original, directory = load_reference(self.root / "references", "mirage-t2-catwalk")
        original_aim = (directory / "aim.png").read_bytes()
        captures = capture_workflow.share_spawn_standing(self.root, self.context, self.standing,
                                                         self.remote / "plans")
        self.assertEqual({item["plan_id"] for item in captures},
                         {self.plan_path.name, second["plan_id"], third["plan_id"]})
        for item in captures:
            identity = item["plan_id"][:-5]
            manifest, directory = load_reference(self.root / "references", identity, allow_partial=True)
            self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
            self.assertNotIn("standing_marker", manifest)
            transfer_capture(self.root, identity, self.remote)
            remote, _ = load_reference(self.remote / "references", identity, allow_partial=True)
            self.assertEqual(remote, manifest)
        updated, directory = load_reference(self.root / "references", "mirage-t2-catwalk")
        self.assertEqual((directory / "aim.png").read_bytes(), original_aim)
        for key in ("aim_plan", "geometry", "confirmed_at_utc", "manual_throw_verified"):
            self.assertEqual(updated[key], original[key])
        self.assertFalse((self.root / "references/mirage-ct1-catwalk").exists())
        self.assertFalse((self.root / "references/dust-t1-catwalk").exists())
        self.assertFalse((self.root / "references/mirage-catwalk-b").exists())
        self.assertFalse(load_reference(self.root / "references", "mirage-t10-vip", allow_partial=True)[0]["manual_throw_verified"])

    def test_new_spawn_f10_reuses_shared_picture_but_other_standing_remains_independent(self):
        capture_workflow.share_spawn_standing(self.root, self.context, self.standing, self.remote / "plans")
        new = self.add_plan("T11 B小烟", "mirage-t11-catwalk")
        self.complete(lambda: deepcopy(new))
        manifest, directory = load_reference(self.root / "references", "mirage-t11-catwalk")
        self.assertTrue(manifest["capture_complete"])
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        other = self.add_plan("B小 B烟", "mirage-catwalk-b")
        self.complete(lambda: deepcopy(other))
        manifest, directory = load_reference(self.root / "references", "mirage-catwalk-b")
        self.assertNotIn("standing_image", manifest)
        self.assertFalse((directory / "standing.png").exists())

    def test_obs_f9_distributes_one_capture_to_same_spawn_group(self):
        second = self.add_plan("T2 B小烟", "mirage-t2-catwalk")
        result, status = self.run_worker("standing")
        self.assertEqual(result, 0)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("sent", True))
        manifest, directory = load_reference(self.remote / "references", second["plan_id"][:-5], allow_partial=True)
        self.assertEqual((directory / "standing.png").read_bytes(), self.aim.read_bytes())
        self.assertFalse(manifest["manual_throw_verified"])

    def test_shared_standing_preserves_auxiliary_newer_aim_and_backups_previous_picture(self):
        second = self.add_plan("T2 B小烟", "mirage-t2-catwalk")
        self.complete(lambda: deepcopy(second))
        transfer_capture(self.root, second["plan_id"][:-5], self.remote)
        remote_context = deepcopy(second)
        remote_context["plan"]["notes"] = "另一份已采瞄角"
        self.aim.write_bytes(png_rgb(640, 360, [bytes([77, 55, 33]) * 640] * 360))
        capture_once(lambda: deepcopy(remote_context), self.remote, self.fake_capture, self.standing)
        latest, remote_directory = load_reference(self.remote / "references", second["plan_id"][:-5])
        latest_aim = (remote_directory / "aim.png").read_bytes()
        captures = capture_workflow.share_spawn_standing(self.root, self.context, self.standing, self.remote / "plans")
        manifest, directory = load_reference(self.root / "references", second["plan_id"][:-5], allow_partial=True)
        self.assertEqual((directory / "aim.png").read_bytes(), latest_aim)
        self.assertEqual(manifest["aim_plan"], latest["aim_plan"])
        self.assertNotEqual(manifest["aim_plan"], manifest["plan"])
        for item in captures:
            transfer_capture(self.root, item["plan_id"][:-5], self.remote)
        self.standing.write_bytes(png_rgb(800, 600, [bytes([12, 13, 14]) * 800] * 600))
        capture_workflow.share_spawn_standing(self.root, self.context, self.standing, self.remote / "plans")
        renewed, directory = load_reference(self.root / "references", second["plan_id"][:-5], allow_partial=True)
        self.assertEqual((directory / "aim.png").read_bytes(), latest_aim)
        self.assertEqual(renewed["aim_plan"], latest["aim_plan"])
        self.assertEqual(len(list((self.root / "standing/backups/de_mirage-T").glob("*.png"))), 1)
        self.assertTrue(list((self.root / "reference-backups" / second["plan_id"][:-5]).glob("*/standing.png")))

    def test_obs_group_transfer_failure_does_not_report_early_sent(self):
        self.add_plan("T2 B小烟", "mirage-t2-catwalk")
        states = []
        original_write = worker.write_json
        def record(path, value):
            states.append(value["state"])
            original_write(path, value)
        original_transfer = transfer_capture
        def interrupted(root, identity, destination, **kwargs):
            if identity == "mirage-t2-catwalk":
                raise OSError("synthetic second transfer failure")
            return original_transfer(root, identity, destination, **kwargs)
        with patch.object(worker, "write_json", side_effect=record), \
             patch.object(worker, "transfer_capture", side_effect=interrupted):
            result, status = self.run_worker("standing")
        self.assertEqual(result, 2)
        self.assertEqual((status["state"], status["terminal"]), ("pending", True))
        self.assertNotIn("sent", states)

    def test_shared_standing_keeps_unsent_local_aim_instead_of_remote_old_image(self):
        second = self.add_plan("T2 B小烟", "mirage-t2-catwalk")
        self.complete(lambda: deepcopy(second))
        transfer_capture(self.root, second["plan_id"][:-5], self.remote)
        self.aim.write_bytes(png_rgb(640, 360, [bytes([99, 88, 77]) * 640] * 360))
        self.complete(lambda: deepcopy(second))
        current, _ = load_reference(self.root / "references", second["plan_id"][:-5])
        capture_workflow.share_spawn_standing(self.root, self.context, self.standing, self.remote / "plans")
        manifest, directory = load_reference(self.root / "references", second["plan_id"][:-5])
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual(manifest["confirmed_at_utc"], current["confirmed_at_utc"])

    def test_f9_then_f10_preserves_custom_standing_without_spawn_marker(self):
        self.capture_standing()
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.complete()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertTrue(manifest["capture_complete"])
        self.assertTrue(manifest["manual_throw_verified"])
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertNotIn("standing_marker", manifest)
        custom, marker = standing_for(self.context, self.root, self.aim)
        self.assertEqual(custom, directory / "standing.png")
        self.assertIsNone(marker)
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        remote, _ = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual(remote, manifest)

    def test_f10_then_f9_preserves_aim_geometry_and_manual_confirmation(self):
        self.complete()
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        original, directory = load_reference(self.root / "references", self.plan_path.stem)
        aim = (directory / "aim.png").read_bytes()
        self.capture_standing()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertTrue(manifest["capture_complete"])
        self.assertEqual((directory / "aim.png").read_bytes(), aim)
        for field in ("geometry", "target_x", "target_y", "manual_throw_verified", "confirmed_at_utc", "confirmation_source"):
            self.assertEqual(manifest[field], original[field])
        self.assertNotEqual(manifest["reference_revision"], original["reference_revision"])
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        remote, target = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual(remote, manifest)
        self.assertEqual((target / "aim.png").read_bytes(), aim)

    def test_repeated_f9_and_f10_only_replace_the_selected_role(self):
        self.capture_standing()
        self.complete()
        first, directory = load_reference(self.root / "references", self.plan_path.stem)
        old_aim = (directory / "aim.png").read_bytes()
        self.standing.write_bytes(png_rgb(640, 480, [bytes([22, 33, 44]) * 640] * 480))
        self.capture_standing()
        second, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertEqual((directory / "aim.png").read_bytes(), old_aim)
        self.assertEqual(second["confirmed_at_utc"], first["confirmed_at_utc"])
        standing = (directory / "standing.png").read_bytes()
        self.aim.write_bytes(png_rgb(640, 360, [bytes([66, 77, 88]) * 640] * 360))
        self.complete()
        third, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertEqual((directory / "standing.png").read_bytes(), standing)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual(third["standing_captured_at_utc"], second["standing_captured_at_utc"])
        self.assertNotEqual(third["reference_revision"], second["reference_revision"])

    def changed_plan_then_standing(self, legacy=False):
        self.complete()
        previous, directory = load_reference(self.root / "references", self.plan_path.stem)
        old_plan = deepcopy(previous["plan"])
        old_aim = (directory / "aim.png").read_bytes()
        if legacy:
            previous.pop("aim_plan")
            previous["reference_revision"] = manifest_revision(previous)
            capture_workflow.write_json(directory / "reference.json", previous)
        self.plan["notes"] = "setpos 1 2 3;setang -42 -170 0"
        self.plan["actions"][0]["duration_ms"] = 300
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False, indent=2), encoding="utf-8")
        self.context["plan_sha256"] = hashlib.sha256(self.plan_path.read_bytes()).hexdigest()
        self.capture_standing()
        changed, directory = load_reference(self.root / "references", self.plan_path.stem, allow_partial=True)
        self.assertEqual(changed["plan"], self.plan)
        self.assertEqual(changed["aim_plan"], old_plan)
        self.assertEqual((directory / "aim.png").read_bytes(), old_aim)
        for field in ("geometry", "target_x", "target_y", "manual_throw_verified", "confirmed_at_utc", "confirmation_source"):
            self.assertEqual(changed[field], previous[field])
        with self.assertRaisesRegex(ValueError, "参考资源与当前方案不一致"):
            load_reference(self.root / "references", self.plan_path.stem)
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        remote, remote_directory = load_reference(self.remote / "references", self.plan_path.stem, allow_partial=True)
        self.assertEqual(remote, changed)
        self.assertEqual((remote_directory / "aim.png").read_bytes(), old_aim)
        with self.assertRaisesRegex(ValueError, "参考资源与当前方案不一致"):
            load_reference(self.remote / "references", self.plan_path.stem)
        self.aim.write_bytes(png_rgb(640, 360, [bytes([41, 81, 161]) * 640] * 360))
        self.complete()
        renewed, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertEqual(renewed["aim_plan"], self.plan)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        remote, _ = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual(remote, renewed)

    def test_f9_keeps_aim_bound_to_its_captured_plan_until_f10(self):
        self.changed_plan_then_standing()

    def test_f9_preserves_legacy_aim_plan_before_updating_current_plan(self):
        self.changed_plan_then_standing(legacy=True)

    def test_f10_keeps_legacy_standing_note_marker_and_original_image(self):
        refs = self.root / "references"
        marker = {"x": .223, "y": .498, "label": "T1"}
        identity = begin(refs, self.plan_path, True, "出生点 T1", marker)
        add_image(refs, identity, "standing", image=self.standing)
        add_image(refs, identity, "aim", image=self.aim)
        self.complete()
        manifest, directory = load_reference(refs, identity)
        self.assertEqual(manifest["standing_note"], "出生点 T1")
        self.assertEqual(manifest["standing_marker"], marker)
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.capture_standing()
        manifest, _ = load_reference(refs, identity)
        self.assertNotIn("standing_marker", manifest)
        self.assertEqual(manifest["standing_source"], "host_f9")

    def test_f9_partial_local_failure_recovers_before_f10_without_losing_standing(self):
        write = capture_workflow.write_json
        def fail_metadata(path, value):
            if Path(path).parent.name == "captures":
                raise OSError("synthetic metadata failure")
            return write(path, value)
        with patch.object(capture_workflow, "write_json", side_effect=fail_metadata):
            with self.assertRaises(LocalSavePending):
                self.capture_standing()
        self.complete()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertTrue(manifest["capture_complete"])
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.assertFalse((self.root / "capture-transactions" / self.plan_path.stem / "pending.json").exists())

    def test_f9_partial_transfer_recovers_interrupted_directory_switch(self):
        self.capture_standing()
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        target = self.remote / "references" / self.plan_path.stem
        backup = self.remote / "reference-backups" / self.plan_path.stem / "interrupted"
        backup.parent.mkdir(parents=True, exist_ok=True)
        target.rename(backup)
        journal = self.remote / "capture-transfers" / self.plan_path.stem / "pending.json"
        journal.write_text(json.dumps({"identity": self.plan_path.stem, "backup": backup.relative_to(self.remote).as_posix()}), encoding="utf-8")
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        manifest, directory = load_reference(self.remote / "references", self.plan_path.stem, allow_partial=True)
        self.assertFalse(manifest["manual_throw_verified"])
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.assertFalse((directory / "aim.png").exists())
        self.assertFalse(journal.exists())

    def test_not_ready_expired_or_wrong_foreground_never_captures(self):
        for change in ({"ready": False, "reason": "请启用道具测试"}, {"remaining_ms": 0},
                       {"_deadline": time.monotonic() - 1}):
            value = deepcopy(self.context)
            value.update(change)
            with self.assertRaises(ValueError):
                self.complete(lambda: value)
        with self.assertRaisesRegex(ValueError, "失焦"):
            capture_once(lambda: deepcopy(self.context), self.root, self.fake_capture, self.standing,
                         foreground=lambda: 2, expected_foreground=1)
        self.assertEqual(self.capture_count, 0)
        self.assertFalse((self.root / "references").exists())

    def test_context_changes_while_capturing_do_not_publish(self):
        for changed in ("generation", "plan_sha256", "team"):
            before, after = deepcopy(self.context), deepcopy(self.context)
            after[changed] = {"generation": "service-2", "plan_sha256": "0" * 64, "team": "CT"}[changed]
            iterator = iter((before, after))
            with self.assertRaisesRegex(ValueError, "状态改变"):
                self.complete(lambda: next(iterator))
        self.assertFalse((self.root / "references").exists())

    def test_missing_other_standing_image_does_not_reuse_spawn_image(self):
        value = deepcopy(self.context)
        value["plan"]["name"] = "T10 B小烟"
        with self.assertRaises(ValueError):
            standing_for(value, self.root, self.standing)
        (self.root / "standing").mkdir(parents=True)
        custom = self.root / "standing" / (self.plan_path.stem + ".png")
        shutil.copyfile(self.standing, custom)
        path, marker = standing_for(value, self.root, self.standing)
        self.assertEqual(path, custom)
        self.assertIsNone(marker)

    def test_recapture_keeps_old_reference_and_package_backup(self):
        first = self.complete()
        old = (self.root / "references" / self.plan_path.stem / "aim.png").read_bytes()
        self.aim.write_bytes(png_rgb(640, 360, [bytes([40, 70, 180]) * 640] * 360))
        second = self.complete()
        self.assertNotEqual(first["capture_id"], second["capture_id"])
        backup = self.root / "reference-backups" / self.plan_path.stem / second["capture_id"]
        self.assertEqual((backup / "aim.png").read_bytes(), old)
        self.assertEqual(len(list((self.root / "package-backups/de_mirage").glob("*.zip"))), 1)

    def test_damaged_old_zip_is_backed_up_and_recapture_continues(self):
        captured = self.complete()
        archive = self.root / captured["package"]
        archive.write_bytes(b"damaged old archive")
        self.complete()
        backups = list((self.root / "package-backups/de_mirage").glob("*.zip"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b"damaged old archive")
        with zipfile.ZipFile(archive) as value:
            self.assertIsNone(value.testzip())

    def test_failed_local_metadata_write_has_recoverable_transaction(self):
        write = capture_workflow.write_json
        def fail_metadata(path, value):
            if Path(path).parent.name == "captures":
                raise OSError("synthetic metadata failure")
            return write(path, value)
        with patch.object(capture_workflow, "write_json", side_effect=fail_metadata):
            with self.assertRaises(LocalSavePending):
                self.complete()
        journal = self.root / "capture-transactions" / self.plan_path.stem / "pending.json"
        self.assertTrue(journal.is_file())
        previous = json.loads(journal.read_text(encoding="utf-8"))["metadata"]
        self.assertTrue((self.root / previous["package"]).is_file())
        captured = self.complete()
        self.assertFalse(journal.exists())
        self.assertNotEqual(captured["capture_id"], previous["capture_id"])
        self.assertTrue((journal.parent / ("completed-" + previous["capture_id"] + ".json")).is_file())

    def test_interrupted_json_temporary_does_not_block_recovery(self):
        captures = self.root / "captures"
        transactions = self.root / "capture-transactions" / self.plan_path.stem
        captures.mkdir(parents=True)
        transactions.mkdir(parents=True)
        old_capture = captures / (self.plan_path.stem + ".json.pending")
        old_journal = transactions / "pending.json.pending"
        old_capture.write_text("interrupted", encoding="utf-8")
        old_journal.write_text("interrupted", encoding="utf-8")
        captured = self.complete()
        self.assertEqual(captured["status"], "pending")
        self.assertTrue(old_capture.is_file())
        self.assertTrue(old_journal.is_file())

    def test_long_same_names_keep_distinct_archive_identity(self):
        self.context["plan"]["name"] = "T1 " + "长名称" * 50
        first = self.complete()
        first_bytes = (self.root / first["package"]).read_bytes()
        self.context["plan_id"] = "another-plan.json"
        second = self.complete()
        self.assertNotEqual(first["package"], second["package"])
        self.assertEqual((self.root / first["package"]).read_bytes(), first_bytes)
        self.assertIn("another-plan", second["package"])

    def test_real_local_transfer_checks_plan_hash_and_changes_only_resources(self):
        captured = self.complete()
        before_plan = self.plan_path.read_bytes()
        result = transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.assertEqual(result["status"], "sent")
        self.assertEqual(self.plan_path.read_bytes(), before_plan)
        manifest, directory = load_reference(self.remote / "references", self.plan_path.stem, self.plan_path.name)
        self.assertEqual(manifest["reference_revision"], captured["reference_revision"])
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual((self.remote / captured["package"]).read_bytes(), (self.root / captured["package"]).read_bytes())

    def test_changed_plan_or_network_failure_preserves_local_pending(self):
        captured = self.complete()
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "辅机方案"):
            transfer_capture(self.root, self.plan_path.stem, self.remote)
        with self.assertRaises(ValueError):
            transfer_capture(self.root, self.plan_path.stem, self.base / "unavailable")
        self.assertTrue((self.root / captured["package"]).exists())
        record = json.loads((self.root / "captures" / (self.plan_path.stem + ".json")).read_text(encoding="utf-8"))
        self.assertEqual(record["status"], "pending")
        self.assertFalse((self.remote / "references").exists())

    def test_interrupted_directory_switch_restores_before_retry(self):
        self.complete()
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        target = self.remote / "references" / self.plan_path.stem
        backup = self.remote / "reference-backups" / self.plan_path.stem / "interrupted"
        backup.parent.mkdir(parents=True, exist_ok=True)
        target.rename(backup)
        journal = self.remote / "capture-transfers" / self.plan_path.stem / "pending.json"
        journal.write_text(json.dumps({"identity": self.plan_path.stem, "backup": backup.relative_to(self.remote).as_posix()}), encoding="utf-8")
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.assertTrue(target.is_dir())
        load_reference(self.remote / "references", self.plan_path.stem)
        self.assertFalse(journal.exists())

    def test_plan_and_metadata_path_escape_are_rejected(self):
        for change in ({"plan_id": "../bad.json"}, {"map": "../bad", "plan": {**self.plan, "map": "../bad"}}):
            value = deepcopy(self.context)
            value.update(change)
            with self.assertRaises(ValueError):
                validate_context(value)
        self.complete()
        record = self.root / "captures" / (self.plan_path.stem + ".json")
        metadata = json.loads(record.read_text(encoding="utf-8"))
        metadata["package"] = "packages/de_mirage/../../../../escape.zip"
        record.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaises(ValueError):
            transfer_capture(self.root, self.plan_path.stem, self.remote)

    def test_context_reader_uses_real_loopback_http(self):
        payload = json.dumps(self.context).encode()
        paths = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                paths.append(self.path)
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *args):
                pass
        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = ContextReader("127.0.0.1", server.server_port)()
            self.assertGreater(result["_deadline"], time.monotonic())
            self.assertEqual(result["plan_sha256"], self.context["plan_sha256"])
            standing = ContextReader("127.0.0.1", server.server_port, role="standing")()
            self.assertEqual(standing["plan_sha256"], self.context["plan_sha256"])
            self.assertEqual(paths, ["/capture-context.json", "/capture-context.json?role=standing"])
        finally:
            server.shutdown()
            worker.join(2)
            server.server_close()
        for host in ("8.8.8.8", "0.0.0.0", "::1", "example.com"):
            with self.assertRaises(ValueError):
                ContextReader(host)

    def test_context_reader_network_errors_explain_auxiliary_service_without_retry(self):
        reader = ContextReader("192.168.3.20")
        for error, message in (
            (urllib.error.URLError(TimeoutError("timed out")), "读取辅机方案超时"),
            (TimeoutError("timed out"), "读取辅机方案超时"),
            (urllib.error.URLError(ConnectionRefusedError("refused")), "无法连接辅机道具服务"),
            (urllib.error.HTTPError(reader.url, 503, "unavailable", {}, None), "辅机道具服务返回HTTP 503"),
        ):
            with self.subTest(error=type(error).__name__), patch.object(reader.opener, "open", side_effect=error) as opened:
                with self.assertRaisesRegex(ValueError, message) as caught:
                    reader()
                self.assertIn("192.168.3.20:5015", str(caught.exception))
                self.assertIn("辅机Xen", str(caught.exception))
                self.assertEqual(opened.call_count, 1)
                self.assertEqual(opened.call_args.kwargs["timeout"], 2)

    def test_context_reader_real_http_header_timeout_remains_bounded_without_retry(self):
        requests, release = [], threading.Event()
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                release.wait(3)
                try:
                    self.send_response(200)
                    self.end_headers()
                except OSError:
                    pass
            def log_message(self, *args):
                pass
        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        started = time.monotonic()
        try:
            with self.assertRaisesRegex(ValueError, "读取辅机方案超时"):
                ContextReader("127.0.0.1", server.server_port)()
            self.assertLess(time.monotonic() - started, 3)
            self.assertEqual(requests, ["/capture-context.json"])
        finally:
            release.set()
            server.shutdown()
            worker.join(2)
            server.server_close()


    # 单作业忙态、超时与退出状态的消费已由 OBS 插件模型负责；此处验证实际 worker 协议。
    def worker_arguments(self):
        self.status_path = self.base / "jobs" / "capture.json"
        return ["--host", "127.0.0.1", "--web-port", "5015", "--root", str(self.root),
                "--destination", str(self.remote), "--shared-standing", str(self.standing),
                "--window", "12", "--process", "34", "--job-status", str(self.status_path)]

    def run_worker(self, role=None):
        def capture(*args, **kwargs):
            return capture_once(*args, capture=self.fake_capture, **kwargs)
        with patch.object(worker, "ContextReader", return_value=lambda: deepcopy(self.context)) as context_reader, \
             patch.object(worker, "capture_once", side_effect=capture), \
             patch.object(worker, "foreground_identity", return_value={"window": 12, "process": 34}):
            args = self.worker_arguments() + (["--role", role] if role else [])
            result = worker.main(args)
            context_reader.assert_called_once_with("127.0.0.1", 5015, role=role or "aim")
        return result, json.loads(self.status_path.read_text(encoding="utf-8"))

    def test_obs_worker_captures_and_transfers_once_then_reports_terminal_success(self):
        plan_bytes = self.plan_path.read_bytes()
        result, status = self.run_worker()
        self.assertEqual(result, 0)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("sent", True))
        self.assertIn("T1 VIP烟", status["message"])
        self.assertEqual(self.plan_path.read_bytes(), plan_bytes)
        _, directory = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())

    def test_obs_worker_f9_routes_standing_and_transfers_without_aim(self):
        result, status = self.run_worker("standing")
        self.assertEqual(result, 0)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("sent", True))
        self.assertIn("站位图", status["message"])
        manifest, directory = load_reference(self.remote / "references", self.plan_path.stem, allow_partial=True)
        self.assertFalse(manifest["manual_throw_verified"])
        self.assertFalse((directory / "aim.png").exists())
        self.assertEqual((directory / "standing.png").read_bytes(), self.aim.read_bytes())

    def test_obs_worker_failed_context_reports_failure_without_capture(self):
        self.context["ready"] = False
        self.context["reason"] = "当前方案不可用"
        result, status = self.run_worker()
        self.assertEqual(result, 1)
        self.assertEqual(self.capture_count, 0)
        self.assertEqual((status["state"], status["terminal"]), ("failed", True))
        self.assertIn("当前方案不可用", status["message"])
        self.assertFalse((self.remote / "references").exists())

    def test_obs_worker_transfer_failure_reports_pending_and_keeps_local_resources(self):
        with patch.object(worker, "transfer_capture", side_effect=OSError("synthetic disconnected share")):
            result, status = self.run_worker()
        self.assertEqual(result, 2)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("pending", True))
        self.assertIn("本地已保存，发送未确认", status["message"])
        _, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertFalse((self.remote / "references").exists())

    def test_obs_worker_interrupted_local_publish_reports_pending_not_false_failure(self):
        write = capture_workflow.write_json
        def fail_metadata(path, value):
            if Path(path).parent.name == "captures":
                raise OSError("synthetic metadata failure")
            return write(path, value)
        with patch.object(capture_workflow, "write_json", side_effect=fail_metadata):
            result, status = self.run_worker()
        self.assertEqual(result, 2)
        self.assertEqual((status["state"], status["terminal"]), ("pending", True))
        journal = self.root / "capture-transactions" / self.plan_path.stem / "pending.json"
        self.assertTrue(journal.is_file())
        self.assertFalse((self.remote / "references").exists())


if __name__ == "__main__":
    unittest.main()
