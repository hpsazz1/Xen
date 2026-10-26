"""双图资源离线回归；合成图仅位于测试临时目录，不调用屏幕或设备。"""

from pathlib import Path
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.voice_switch.reference_assets import load_reference, png_size, resource_directory
from tools.voice_switch.reference_tool import add_image, begin
from tools.voice_switch.screen_capture import png_rgb


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xen-reference-test-")
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

    def test_mismatched_image_size_or_invalid_geometry_does_not_publish(self):
        identity = begin(self.root, self.plan, True)
        add_image(self.root, identity, "standing", self.image)
        smaller = self.base / "different.png"
        smaller.write_bytes(png_rgb(640, 480, [bytes([0, 0, 0]) * 640] * 480))
        with self.assertRaisesRegex(ValueError, "分辨率"):
            add_image(self.root, identity, "aim", smaller)
        with self.assertRaisesRegex(ValueError, "ROI"):
            add_image(self.root, identity, "aim", self.image, roi_x=600)
        self.assertFalse((self.root / identity / "reference.json").exists())
        self.assertFalse((self.root / identity / "aim.png").exists())

    def test_path_escape_and_truncated_png_are_rejected(self):
        for identity in ("..", "../x", "C:\\x", "x/y", "NUL", "x..y"):
            with self.assertRaises(ValueError):
                resource_directory(self.root, identity)
        self.image.write_bytes(self.image.read_bytes()[:50])
        with self.assertRaises(ValueError):
            png_size(self.image)


if __name__ == "__main__":
    unittest.main()
