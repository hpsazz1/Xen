"""冻结 Run 的子进程环境契约；不启动应用、不使用设备。"""
from pathlib import Path
import shutil
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/invoke_live_game_acceptance.ps1"
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")

@unittest.skipUnless(POWERSHELL, "需要 PowerShell")
class FrozenDataRootTests(unittest.TestCase):
    def test_child_environment_and_restoration_on_success_and_failure(self):
        command = "$source='" + str(SCRIPT).replace("'", "''") + "';" + r"""
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw $errors[0] }
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Start-DiagnosticProcess'},$false)
. ([scriptblock]::Create($function.Extent.Text))
function Start-Process {
    param($FilePath,$WorkingDirectory,[switch]$PassThru,[switch]$Wait)
    if ($env:XEN_DATA_ROOT -cne 'C:\frozen-run') { throw 'wrong frozen data root' }
    if ($env:XEN_RUNTIME_DIAGNOSTICS -cne '1') { throw 'missing diagnostics' }
    foreach ($name in @('XEN_RELEASE_ROOT','XEN_RUNTIME_ID','XEN_RELEASE_BACKENDS')) {
        if ([Environment]::GetEnvironmentVariable($name,'Process')) { throw 'inherited release context' }
    }
    if ($script:failStart) { throw 'expected simulated failure' }
    [pscustomobject]@{Id=7}
}
$names=@('XEN_DATA_ROOT','XEN_RUNTIME_DIAGNOSTICS','XEN_RELEASE_ROOT','XEN_RUNTIME_ID','XEN_RELEASE_BACKENDS')
$original=@{}
foreach($name in $names) { $original[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
try {
    foreach($previous in @($null,'sentinel')) {
        foreach($script:failStart in @($false,$true)) {
            foreach($name in $names) { [Environment]::SetEnvironmentVariable($name,$previous,'Process') }
            $failed=$false
            try { Start-DiagnosticProcess 'never-execute.exe' 'C:\frozen-run' | Out-Null } catch {
                if ($_.Exception.Message -ne 'expected simulated failure') { throw }
                $failed=$true
            }
            if ($failed -ne $script:failStart) { throw 'unexpected launch result' }
            foreach($name in $names) {
                if ([string][Environment]::GetEnvironmentVariable($name,'Process') -cne [string]$previous) { throw "not restored: $name" }
            }
        }
    }
} finally { foreach($name in $names) { [Environment]::SetEnvironmentVariable($name,$original[$name],'Process') } }
"""
        result = subprocess.run([POWERSHELL, '-NoProfile', '-Command', command], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
