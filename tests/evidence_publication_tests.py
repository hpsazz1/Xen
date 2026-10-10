"""证据文件发布边界回归，确定性注入目标竞争，不访问设备。"""
import importlib.util
from concurrent.futures import ThreadPoolExecutor
import threading
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
WRITERS = {
    "evaluate_aim_production_red": "_write_json_atomic",
    "build_aim_production_red_dbed788b_plan": "write_json_atomic",
    "build_aim_production_red_dbed788b_fidelity_plan": "write_json_atomic",
    "freeze_mouse_effect_probe_b_composite_phase_plan": "write_atomic",
    "bind_mouse_effect_probe_b_composite_phase_calibration": "_write_json_atomic",
    "evaluate_mouse_effect_probe_b_composite_phase": "_write_json_atomic",
}
def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
class PublicationTests(unittest.TestCase):
    def test_concurrent_destination_is_never_overwritten(self):
        for name, function in WRITERS.items():
            with self.subTest(writer=name), tempfile.TemporaryDirectory() as text:
                target = Path(text) / "evidence.json"
                writer = getattr(load(name), function)
                replace = Path.replace
                link = os.link
                def rival_replace(source, destination):
                    Path(destination).write_bytes(b"rival-evidence")
                    return replace(source, destination)
                def rival_link(source, destination, *args, **kwargs):
                    Path(destination).write_bytes(b"rival-evidence")
                    return link(source, destination, *args, **kwargs)
                with patch.object(Path,"replace",rival_replace),patch.object(os,"link",rival_link):
                    try: writer(target, {"owner":"ours"})
                    except (FileExistsError, ValueError): pass
                    self.assertEqual(target.read_bytes(),b"rival-evidence")
                self.assertEqual(list(target.parent.iterdir()),[target])
    def test_normal_publish_then_collision_preserves_first(self):
        for name, function in WRITERS.items():
            with self.subTest(writer=name),tempfile.TemporaryDirectory() as text:
                target=Path(text)/"evidence.json"; writer=getattr(load(name),function)
                writer(target,{"owner":"first"})
                with self.assertRaises((FileExistsError,ValueError)): writer(target,{"owner":"second"})
                self.assertEqual(json.loads(target.read_text(encoding="utf-8")),{"owner":"first"})
    def test_same_process_competitors_keep_one_complete_output(self):
        for name, function in WRITERS.items():
            with self.subTest(writer=name), tempfile.TemporaryDirectory() as text:
                target = Path(text) / "evidence.json"
                writer = getattr(load(name), function)
                barrier = threading.Barrier(2)
                link = os.link
                def synchronized_link(source, destination, *args, **kwargs):
                    barrier.wait(timeout=5)
                    return link(source, destination, *args, **kwargs)
                def publish(owner):
                    try:
                        writer(target, {"owner": owner})
                        return True
                    except (FileExistsError, ValueError):
                        return False
                with patch.object(os, "link", synchronized_link), ThreadPoolExecutor(2) as pool:
                    results = list(pool.map(publish, ("first", "second")))
                self.assertEqual(sorted(results), [False, True])
                self.assertIn(json.loads(target.read_text(encoding="utf-8"))["owner"], ("first", "second"))
                self.assertEqual(list(target.parent.iterdir()), [target])

    def test_pair_second_collision_preserves_rival_and_rolls_back_own_first(self):
        module = load("produce_mouse_effect_probe_b_composite_phase_ledgers")
        owned_type = module._PUBLICATION._WindowsPublishedFile
        original_publish = owned_type.publish
        with tempfile.TemporaryDirectory() as text:
            capture = (Path(text) / "capture.json").resolve()
            command = (Path(text) / "command.json").resolve()
            def rival_command(owned, destination):
                if destination == command:
                    command.write_bytes(b"rival-command")
                return original_publish(owned, destination)
            with patch.object(owned_type, "publish", rival_command), self.assertRaises(ValueError):
                module.write_pair_atomic(capture, command, {"capture": True}, {"command": True})
            self.assertFalse(capture.exists())
            self.assertEqual(command.read_bytes(), b"rival-command")
            self.assertEqual(list(command.parent.iterdir()), [command])

    def test_pair_check_then_replace_is_blocked_until_handle_rollback(self):
        module = load("produce_mouse_effect_probe_b_composite_phase_ledgers")
        owned_type = module._PUBLICATION._WindowsPublishedFile
        original_publish = owned_type.publish
        with tempfile.TemporaryDirectory() as text:
            capture = (Path(text) / "capture.json").resolve()
            command = (Path(text) / "command.json").resolve()
            checked = []
            def replace_after_check(owned, destination):
                if destination == command:
                    self.assertTrue(capture.is_file())
                    with self.assertRaises(PermissionError):
                        capture.unlink()
                    with self.assertRaises(PermissionError):
                        capture.write_bytes(b"rival-write-after-check")
                    checked.append(True)
                    command.write_bytes(b"rival-command")
                return original_publish(owned, destination)
            with patch.object(owned_type, "publish", replace_after_check), self.assertRaises(ValueError):
                module.write_pair_atomic(capture, command, {"capture": True}, {"command": True})
            self.assertEqual(checked, [True])
            self.assertFalse(capture.exists())
            self.assertEqual(command.read_bytes(), b"rival-command")
            self.assertEqual(list(command.parent.iterdir()), [command])

    def test_pair_target_rival_before_publish_is_preserved(self):
        module = load("produce_mouse_effect_probe_b_composite_phase_ledgers")
        owned_type = module._PUBLICATION._WindowsPublishedFile
        original_publish = owned_type.publish
        with tempfile.TemporaryDirectory() as text:
            capture = (Path(text) / "capture.json").resolve()
            command = (Path(text) / "command.json").resolve()
            def rival_before_publish(owned, destination):
                if destination == capture:
                    capture.write_bytes(b"rival-before-publish")
                return original_publish(owned, destination)
            with patch.object(owned_type, "publish", rival_before_publish), self.assertRaises(ValueError):
                module.write_pair_atomic(capture, command, {"capture": True}, {"command": True})
            self.assertEqual(capture.read_bytes(), b"rival-before-publish")
            self.assertFalse(command.exists())
            self.assertEqual(list(capture.parent.iterdir()), [capture])

    def test_pair_handle_acquisition_failure_does_not_publish_partial_pair(self):
        module = load("produce_mouse_effect_probe_b_composite_phase_ledgers")
        owned_type = module._PUBLICATION._WindowsPublishedFile
        original_init = owned_type.__init__
        with tempfile.TemporaryDirectory() as text:
            capture = (Path(text) / "capture.json").resolve()
            command = (Path(text) / "command.json").resolve()
            attempts = []
            def fail_second_acquisition(owned, temporary):
                attempts.append(temporary)
                if len(attempts) == 2:
                    raise PermissionError("injected handle acquisition failure")
                original_init(owned, temporary)
            with patch.object(owned_type, "__init__", fail_second_acquisition), self.assertRaises(PermissionError):
                module.write_pair_atomic(capture, command, {"capture": True}, {"command": True})
            self.assertFalse(capture.exists())
            self.assertFalse(command.exists())
            self.assertEqual(list(capture.parent.iterdir()), [])

    def test_pair_normal_outputs_are_same_group(self):
        writer = load("produce_mouse_effect_probe_b_composite_phase_ledgers").write_pair_atomic
        with tempfile.TemporaryDirectory(prefix="证据组😀-") as text:
            capture = (Path(text) / "capture😀.json").resolve()
            command = (Path(text) / "command中文.json").resolve()
            writer(capture, command, {"group": "same"}, {"group": "same"})
            self.assertEqual(json.loads(capture.read_text(encoding="utf-8")), {"group": "same"})
            self.assertEqual(json.loads(command.read_text(encoding="utf-8")), {"group": "same"})
            self.assertEqual(set(capture.parent.iterdir()), {capture, command})

if __name__ == "__main__": unittest.main()
