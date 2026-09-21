"""仅用临时假包检查人工入口契约，绝不执行有授权的Launch。"""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/invoke_hud_stop_acceptance.ps1"
POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")


@unittest.skipUnless(POWERSHELL, "需要PowerShell")
class HudAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xen-hud-contract-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "package"
        self.run = self.root / "run"
        self.package.mkdir()
        for name in ("XenLauncher.exe", "runtimes/nvidia/Xen.exe", "Start-Xen.cmd",
                     "tools/source/start_source_context_session.ps1"):
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("NEVER EXECUTE TEST FIXTURE", encoding="utf-8")
        self.manifest = {"schema": 1, "product": "Xen", "runtimes": [
            {"id": "nvidia", "executable": "runtimes/nvidia/Xen.exe", "backends": ["tensorrt"]}]}
        self.write_manifest()
        (self.package / "config.ini").write_text(
            "[detector]\nbackend=tensorrt\n[auto_stop]\nexperimental_hud_model=true\n", encoding="utf-8")

    def write_manifest(self):
        (self.package / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def invoke(self, mode="Prepare", success=True):
        # 不允许测试带物理确认令牌；即使误选Launch也只能走授权拒绝路径。
        result = subprocess.run([POWERSHELL, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
                                 "-Mode", mode, "-PackageRoot", str(self.package), "-RunDirectory", str(self.run)],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_prepare_validate_no_launch(self):
        self.invoke()
        self.invoke("Validate")
        task = json.loads((self.run / "task.json").read_text(encoding="utf-8"))
        self.assertEqual(task["task_id"], "AUTO-STOP-HUD-EXPERIMENT-001")
        self.assertEqual(len(task["identities"]), 7)
        self.assertIn("-AllowPhysicalOutput -Confirm HUD_STOP_EXPERIMENT", task["launch_command"])
        self.assertIn(task["launch_command"], (self.run / "TASK.md").read_text(encoding="utf-8"))
        self.assertFalse((self.run / "launch.json").exists())
        self.assertEqual((self.run / "config.ini").read_bytes(), (self.package / "config.ini").read_bytes())
        self.invoke(success=False)

    def test_launch_without_token_rejected_before_prepare(self):
        self.invoke("Launch", success=False)
        self.assertFalse(self.run.exists())

    def test_worker_changed_rejected(self):
        self.invoke()
        (self.package / "runtimes/nvidia/Xen.exe").write_text("changed")
        self.invoke("Validate", success=False)

    def test_bootstrap_changed_rejected(self):
        self.invoke()
        (self.package / "tools/source/start_source_context_session.ps1").write_text("changed")
        self.invoke("Validate", success=False)

    def test_snapshot_changed_rejected(self):
        self.invoke()
        (self.run / "config.ini").write_text("changed")
        self.invoke("Validate", success=False)

    def test_production_config_rejected(self):
        path = self.package / "config.ini"
        path.write_text(path.read_text().replace("true", "false"))
        self.invoke(success=False)
        self.assertFalse(self.run.exists())

    def test_duplicate_route_rejected(self):
        self.manifest["runtimes"].append(self.manifest["runtimes"][0])
        self.write_manifest()
        self.invoke(success=False)

    def test_unsafe_route_rejected(self):
        self.manifest["runtimes"][0]["executable"] = "../Xen.exe"
        self.write_manifest()
        self.invoke(success=False)

    def test_overlapping_paths_rejected(self):
        self.run = self.package / "run"
        self.invoke(success=False)
        self.run = self.root
        self.invoke(success=False)

    def test_reparse_path_rejected(self):
        if not shutil.which("cmd"):
            self.skipTest("Windows目录junction测试")
        alias = self.root / "alias"
        result = subprocess.run(["cmd", "/c", "mklink", "/J", str(alias), str(self.package)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        try:
            self.package = alias
            self.invoke(success=False)
        finally:
            alias.rmdir()


if __name__ == "__main__":
    unittest.main()
