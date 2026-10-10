"""仅用临时假包检查人工入口契约，绝不执行有授权的Launch。"""
import json
from datetime import datetime, timezone, timedelta
import os
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

    def test_diagnostics_environment_inherited_and_restored(self):
        # 只加载生产启动函数，Start-Process是进程替身，不执行Launch或打开设备。
        command = "$source='" + str(SCRIPT).replace("'", "''") + "';" + r'''
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Start-DiagnosticProcess'},$false)
. ([scriptblock]::Create($function.Extent.Text))
function Start-Process {
    param($FilePath,$WorkingDirectory,$WindowStyle,[switch]$PassThru)
    if ($env:XEN_RUNTIME_DIAGNOSTICS -cne '1') { throw '子进程缺少临时记录开关' }
    if ($script:failStart) { throw '模拟启动失败' }
    [pscustomobject]@{Id=7}
}
$original=[Environment]::GetEnvironmentVariable('XEN_RUNTIME_DIAGNOSTICS','Process')
try {
    foreach($previous in @($null,'0')) {
        foreach($script:failStart in @($false,$true)) {
            [Environment]::SetEnvironmentVariable('XEN_RUNTIME_DIAGNOSTICS',$previous,'Process')
            $failed=$false
            try { Start-DiagnosticProcess 'never-execute.exe' 'C:\fixture' | Out-Null } catch { $failed=$true }
            if ($failed -ne $script:failStart) { throw '启动返回状态错误' }
            if ([string][Environment]::GetEnvironmentVariable('XEN_RUNTIME_DIAGNOSTICS','Process') -cne [string]$previous) { throw '调用方环境未恢复' }
        }
    }
} finally { [Environment]::SetEnvironmentVariable('XEN_RUNTIME_DIAGNOSTICS',$original,'Process') }
'''
        original = (self.package / 'config.ini').read_bytes()
        result = subprocess.run([POWERSHELL, '-NoProfile', '-Command', command], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.package / 'config.ini').read_bytes(), original)

    def prepare_recovery(self, legacy=False):
        self.invoke()
        log = self.package / "logs/xen.log"
        log.parent.mkdir()
        log.write_text("old\n", encoding="utf-8")
        old_time = (datetime.now(timezone.utc) - timedelta(minutes=1)).timestamp()
        os.utime(log, (old_time, old_time))
        launch = {"schema": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
                  "before_files": [str(log)], "entrypoint_pid": 1,
                  "ended_utc": datetime.now(timezone.utc).isoformat(), "exit_code": 0}
        if not legacy:
            # Windows FILETIME 和 DateTime ticks 均以 100ns 计，纪元相差 1600 年。
            ticks = log.stat().st_mtime_ns // 100 + 621355968000000000
            launch["before_file_metadata"] = [{"path": str(log), "length": log.stat().st_size,
                                                "last_write_utc_ticks": ticks}]
        (self.run / "launch.json").write_text(json.dumps(launch), encoding="utf-8")
        return log

    def test_recover_appended_existing_log_and_preserve_evidence(self):
        log = self.prepare_recovery()
        original_times = (log.stat().st_atime_ns, log.stat().st_mtime_ns)
        log.write_text("old\ncurrent\n", encoding="utf-8")
        os.utime(log, ns=original_times)
        self.invoke("Recover")
        target = self.run / "reports/logs/xen.log"
        self.assertEqual(target.read_bytes(), log.read_bytes())
        summary = json.loads((self.run / "automatic-summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["collection_basis"], "LENGTH_OR_WRITE_TIME_CHANGED")
        self.assertTrue(summary["reports_may_contain_other_runs"])
        self.assertFalse(summary["physical_effect_verified"])
        self.invoke("Recover")
        log.write_text("old\ncurrent\nlater\n", encoding="utf-8")
        self.invoke("Recover", success=False)
        self.assertEqual(target.read_text(encoding="utf-8"), "old\ncurrent\n")

    def test_recover_rewritten_existing_log(self):
        log = self.prepare_recovery()
        log.write_text("new\n", encoding="utf-8")
        self.invoke("Recover")
        self.assertEqual((self.run / "reports/logs/xen.log").read_bytes(), log.read_bytes())

    def test_recover_unchanged_log_excluded_new_report_included(self):
        self.prepare_recovery()
        report = self.package / "logs/new.json"
        report.write_text("{}", encoding="utf-8")
        self.invoke("Recover")
        self.assertFalse((self.run / "reports/logs/xen.log").exists())
        self.assertTrue((self.run / "reports/logs/new.json").exists())

    def test_recover_legacy_uses_write_time_fallback(self):
        log = self.prepare_recovery(legacy=True)
        self.invoke("Recover")
        self.assertFalse((self.run / "reports/logs/xen.log").exists())
        log.write_text("old\ncurrent\n", encoding="utf-8")
        self.invoke("Recover")
        self.assertEqual((self.run / "reports/logs/xen.log").read_bytes(), log.read_bytes())
        summary = json.loads((self.run / "automatic-summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["collection_basis"], "LEGACY_WRITE_TIME_FALLBACK")

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
