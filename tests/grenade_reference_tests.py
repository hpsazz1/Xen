"""双图资源离线回归；合成图仅位于测试临时目录，不调用屏幕或设备。"""

from pathlib import Path
import json
import hashlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.grenade_capture.reference_assets import load_reference, manifest_revision, png_size, resource_directory
from tools.grenade_capture.reference_tool import add_image, begin
from tools.grenade_capture.screen_capture import png_rgb


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        fixtures = Path(__file__).resolve().parents[1] / "temp/XEN-GRENADE-CATALOG-F9-20261028-001/backend-tests"
        fixtures.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="reference-", dir=fixtures)
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "references"
        self.plan = self.base / "smoke-mirage-example.json"
        self.original = {"schema_version": 3, "name": "测试烟", "map": "de_mirage", "voice_command": "测试烟",
                         "notes": "人工说明原样保留", "actions": [{"type": "button", "button": "left", "duration_ms": 1000}]}
        self.plan.write_text(json.dumps(self.original), encoding="utf-8")
        self.image = self.base / "synthetic.png"
        self.image.write_bytes(png_rgb(640, 360, [bytes([14, 30, 60]) * 640] * 360))

    def complete(self):
        identity = begin(self.root, self.plan, True, "靠住墙角")
        self.assertFalse(add_image(self.root, identity, "standing", self.image))
        self.assertTrue(add_image(self.root, identity, "aim", self.image))
        return identity

    def test_confirmation_and_both_images_are_required(self):
        with self.assertRaisesRegex(ValueError, "人工"):
            begin(self.root, self.plan, False)
        identity = begin(self.root, self.plan, True)
        with self.assertRaises(ValueError):
            load_reference(self.root, identity)
        with self.assertRaisesRegex(ValueError, "standing"):
            add_image(self.root, identity, "aim", self.image)
        add_image(self.root, identity, "standing", self.image)
        self.assertFalse((self.root / identity / "reference.json").exists())

    def test_finished_resource_keeps_plan_and_original_png_bytes(self):
        identity = self.complete()
        value, directory = load_reference(self.root, identity, self.plan.name)
        self.assertEqual(value["plan"], self.original)
        self.assertEqual(value["standing_note"], "靠住墙角")
        self.assertEqual((value["geometry"]["roi_x"], value["geometry"]["roi_y"]), (160, 20))
        self.assertEqual((value["target_x"], value["target_y"]), (160, 160))
        self.assertEqual((directory / "aim.png").read_bytes(), self.image.read_bytes())
        self.assertEqual((directory / "standing.png").read_bytes(), self.image.read_bytes())
        with self.assertRaises(ValueError):
            load_reference(self.root, identity, "other.json")

    def test_legacy_same_team_metadata_does_not_invalidate_existing_aim(self):
        identity = self.complete()
        manifest = self.root / identity / "reference.json"
        original = json.loads(manifest.read_text(encoding="utf-8"))
        for name, team in (("T1 VIP烟", "T"), ("Mirage CT5 匪口烟", "CT"),
                           ("Mirage T来源12号 连接烟", "T"), ("T1 对照 CT5", "CT")):
            for legacy in (None, ""):
                with self.subTest(name=name, legacy=legacy):
                    value = {**original, "plan": {**original["plan"], "name": name, "team": team},
                             "aim_plan": {**original["plan"], "name": name}}
                    if legacy is not None:
                        value["aim_plan"]["team"] = legacy
                    value["reference_revision"] = manifest_revision(value)
                    manifest.write_text(json.dumps(value), encoding="utf-8")
                    loaded, directory = load_reference(self.root, identity)
                    self.assertEqual(loaded, value)
                    self.assertEqual((directory / "aim.png").read_bytes(), self.image.read_bytes())

    def test_real_team_or_plan_change_still_invalidates_existing_aim(self):
        identity = self.complete()
        manifest = self.root / identity / "reference.json"
        original = json.loads(manifest.read_text(encoding="utf-8"))
        old_plan = {**original["plan"], "name": "T1 VIP烟"}
        for change in ({"team": "CT"}, {"map": "de_nuke"}, {"name": "T2 VIP烟"},
                       {"actions": [{"type": "button", "button": "right", "duration_ms": 1000}]}):
            with self.subTest(change=change):
                value = {**original, "plan": {**old_plan, **change}, "aim_plan": old_plan}
                value["reference_revision"] = manifest_revision(value)
                manifest.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "当前方案不一致"):
                    load_reference(self.root, identity)

    def partial(self, role):
        identity = self.complete()
        directory = self.root / identity
        value = json.loads((directory / "reference.json").read_text(encoding="utf-8"))
        removed = "standing" if role == "aim" else "aim"
        (directory / (removed + ".png")).unlink()
        value.pop(removed + "_image")
        value["image_sha256"].pop(removed + "_image")
        value["capture_complete"] = False
        if role == "standing":
            value.pop("geometry")
            value.pop("confirmed_at_utc")
            value["manual_throw_verified"] = False
        value["reference_revision"] = manifest_revision(value)
        (directory / "reference.json").write_text(json.dumps(value), encoding="utf-8")
        return identity, value

    def test_aim_only_is_valid_without_a_standing_image(self):
        identity, original = self.partial("aim")
        value, directory = load_reference(self.root, identity, self.plan.name)
        self.assertEqual(value, original)
        self.assertFalse((directory / "standing.png").exists())
        self.assertEqual((directory / "aim.png").read_bytes(), self.image.read_bytes())

    def test_standing_only_is_transferable_but_not_a_matching_reference(self):
        identity, original = self.partial("standing")
        value, directory = load_reference(self.root, identity, self.plan.name, allow_partial=True)
        self.assertEqual(value, original)
        self.assertFalse((directory / "aim.png").exists())
        with self.assertRaisesRegex(ValueError, "瞄点尚未采集"):
            load_reference(self.root, identity)

    def test_partial_manifest_cannot_claim_missing_image_or_false_confirmation(self):
        identity, original = self.partial("standing")
        manifest = self.root / identity / "reference.json"
        for update in ({"capture_complete": True}, {"capture_complete": 0},
                       {"manual_throw_verified": True}, {"geometry": {}}, {"aim_image": "aim.png"}):
            value = dict(original, **update)
            value["reference_revision"] = manifest_revision(value)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(update=update), self.assertRaises(ValueError):
                load_reference(self.root, identity, allow_partial=True)

    def test_partial_image_still_checks_its_png_hash(self):
        identity, _ = self.partial("standing")
        image = self.root / identity / "standing.png"
        image.write_bytes(png_rgb(640, 360, [bytes([90, 80, 70]) * 640] * 360))
        with self.assertRaisesRegex(ValueError, "图片已改变"):
            load_reference(self.root, identity, allow_partial=True)

    def test_changed_valid_image_is_rejected_by_manifest_identity(self):
        identity = self.complete()
        target = self.root / identity / "aim.png"
        target.write_bytes(png_rgb(640, 360, [bytes([60, 30, 14]) * 640] * 360))
        with self.assertRaisesRegex(ValueError, "图片已改变"):
            load_reference(self.root, identity)

    def test_changed_manifest_is_rejected(self):
        identity = self.complete()
        target = self.root / identity / "reference.json"
        value = json.loads(target.read_text(encoding="utf-8"))
        value["target_x"] = 150
        target.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "清单身份"):
            load_reference(self.root, identity)

    def test_completed_and_partial_images_are_not_overwritten(self):
        identity = self.complete()
        with self.assertRaises(ValueError):
            begin(self.root, self.plan, True)
        with self.assertRaises(ValueError):
            add_image(self.root, identity, "aim", self.image)

    def test_invalid_aim_geometry_does_not_publish(self):
        identity = begin(self.root, self.plan, True)
        add_image(self.root, identity, "standing", self.image)
        with self.assertRaisesRegex(ValueError, "ROI"):
            add_image(self.root, identity, "aim", self.image, roi_x=600)
        self.assertFalse((self.root / identity / "reference.json").exists())
        self.assertFalse((self.root / identity / "aim.png").exists())

    def test_shared_standing_keeps_own_size_and_marker_without_changing_pixels(self):
        shared = self.base / "spawn-points.png"
        shared.write_bytes(png_rgb(2560, 1440, [bytes([20, 60, 90]) * 2560] * 1440))
        marker = {"x": 0.25, "y": 0.5, "label": "T1"}
        identity = begin(self.root, self.plan, True, "使用出生点总图", standing_marker=marker)
        add_image(self.root, identity, "standing", shared)
        add_image(self.root, identity, "aim", self.image)
        value, directory = load_reference(self.root, identity, self.plan.name)
        self.assertEqual(value["standing_marker"], marker)
        self.assertEqual(png_size(directory / "standing.png"), (2560, 1440))
        self.assertEqual(png_size(directory / "aim.png"), (640, 360))
        self.assertEqual((value["geometry"]["source_width"], value["geometry"]["source_height"]), (640, 360))
        self.assertEqual((directory / "standing.png").read_bytes(), shared.read_bytes())
        self.assertEqual((directory / "aim.png").read_bytes(), self.image.read_bytes())

    def test_aim_mismatch_still_rejected_when_standing_can_differ(self):
        identity = self.complete()
        directory = self.root / identity
        target = directory / "aim.png"
        target.write_bytes(png_rgb(640, 480, [bytes([20, 30, 40]) * 640] * 480))
        manifest = directory / "reference.json"
        value = json.loads(manifest.read_text(encoding="utf-8"))
        value["image_sha256"]["aim_image"] = hashlib.sha256(target.read_bytes()).hexdigest()
        value["reference_revision"] = manifest_revision(value)
        manifest.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "瞄点图尺寸"):
            load_reference(self.root, identity)

    def test_bad_standing_marker_is_rejected_at_creation_and_load(self):
        markers = [None, {}, {"x": True, "y": 0.5, "label": "T1"},
                   {"x": -0.1, "y": 0.5, "label": "T1"},
                   {"x": 0.5, "y": 1.1, "label": "T1"},
                   {"x": float("nan"), "y": 0.5, "label": "T1"},
                   {"x": 0.5, "y": 0.5, "label": "T1\n"}]
        for marker in markers[1:]:
            with self.subTest(marker=marker), self.assertRaisesRegex(ValueError, "站位标记"):
                begin(self.root, self.plan, True, standing_marker=marker)
        identity = self.complete()
        manifest = self.root / identity / "reference.json"
        original = json.loads(manifest.read_text(encoding="utf-8"))
        for marker in markers:
            value = dict(original, standing_marker=marker)
            value["reference_revision"] = manifest_revision(value)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(marker=marker), self.assertRaisesRegex(ValueError, "站位标记"):
                load_reference(self.root, identity)

    def test_path_escape_and_truncated_png_are_rejected(self):
        for identity in ("..", "../x", "C:\\x", "x/y", "NUL", "x..y"):
            with self.assertRaises(ValueError):
                resource_directory(self.root, identity)
        self.image.write_bytes(self.image.read_bytes()[:50])
        with self.assertRaises(ValueError):
            png_size(self.image)


if __name__ == "__main__":
    unittest.main()
