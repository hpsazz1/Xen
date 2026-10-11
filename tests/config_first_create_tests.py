"""Exercise ordinary config creation with isolated processes and no app runtime."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


PROBE = Path(sys.argv.pop(1)).resolve()


class FirstCreateTests(unittest.TestCase):
    def invoke(self, mode, path):
        process = subprocess.run(
            [str(PROBE), mode, str(path)], capture_output=True, timeout=20,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        return json.loads(process.stdout)

    def assert_loaded(self, result, *, created=False, edited=False):
        self.assertTrue(result["ok"], result)
        self.assertTrue(result["error_empty"], result)
        self.assertEqual(result["created"], created, result)
        self.assertEqual(result["width"], 1234 if edited else 900, result)
        self.assertEqual(result["edited_model"], edited, result)

    def race(self, root, competitor):
        path = root / "config.ini"
        ready, resume = root / "ready", root / "resume"
        process = subprocess.Popen(
            [str(PROBE), "slow-create", str(path), str(ready), str(resume)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:
            deadline = time.monotonic() + 10
            while not ready.exists():
                self.assertIsNone(process.poll(), "creator exited before publication")
                self.assertLess(time.monotonic(), deadline, "creator did not become ready")
                time.sleep(0.005)
            self.assertFalse(path.exists())
            other = competitor(path)
            winner = path.read_bytes()
            resume.write_text("resume", encoding="ascii")
            stdout, stderr = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, stderr)
            delayed = json.loads(stdout)
            self.assertNotEqual(other.get("pid"), delayed["pid"])
            self.assertEqual(path.read_bytes(), winner)
            self.assertEqual(list(root.glob("config.ini.tmp.*")), [])
            print(json.dumps({"case": root.name, "competitor": other,
                              "delayed": delayed, "winner_preserved": True}), flush=True)
            return delayed, path
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)

    def test_creation_and_ordinary_save(self):
        for name in ("ascii", "配置-🧪"):
            with self.subTest(path=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / name
                root.mkdir()
                path = root / "config.ini"
                self.assert_loaded(self.invoke("create", path), created=True)
                initial = path.read_bytes()
                self.assert_loaded(self.invoke("create", path))
                self.assertEqual(path.read_bytes(), initial)
                self.assert_loaded(self.invoke("edit", path), edited=True)
                self.assertNotEqual(path.read_bytes(), initial)
                self.assert_loaded(self.invoke("create", path), edited=True)
                self.assertEqual(list(root.glob("config.ini.tmp.*")), [])

    def test_concurrent_creation_preserves_saved_preferences(self):
        self.check_competitor("edit", edited=True)

    def test_concurrent_creation_reports_only_one_creator(self):
        self.check_competitor("create", edited=False)

    def check_competitor(self, mode, *, edited):
        for name in ("ascii", "配置-🧪"):
            with self.subTest(path=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / name
                root.mkdir()

                def competitor(path):
                    result = self.invoke(mode, path)
                    self.assert_loaded(result, created=True, edited=edited)
                    return result

                delayed, path = self.race(root, competitor)
                self.assert_loaded(delayed, edited=edited)
                self.assert_loaded(self.invoke("create", path), edited=edited)

    def test_concurrent_invalid_file_is_retained_then_explicitly_repaired(self):
        for name in ("ascii", "配置-🧪"):
            with self.subTest(path=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / name
                root.mkdir()
                invalid = b"[ui]\nwidth=invalid\n"

                def competitor(path):
                    path.write_bytes(invalid)
                    return {"synthetic_invalid_file": True}

                delayed, path = self.race(root, competitor)
                self.assertFalse(delayed["ok"], delayed)
                self.assertFalse(delayed["created"], delayed)
                self.assertFalse(delayed["error_empty"], delayed)
                self.assertEqual(path.read_bytes(), invalid)
                rejected = self.invoke("create", path)
                self.assertFalse(rejected["ok"], rejected)
                self.assertEqual(path.read_bytes(), invalid)
                self.assert_loaded(self.invoke("replace", path), edited=True)
                self.assert_loaded(self.invoke("create", path), edited=True)


if __name__ == "__main__":
    unittest.main()
