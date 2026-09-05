param(
    [Parameter(Mandatory = $true)][string]$PythonExecutable,
    [Parameter(Mandatory = $true)][string]$PowerShellExecutable,
    [Parameter(Mandatory = $true)][string]$ProducerExecutable,
    [Parameter(Mandatory = $true)][string]$SequenceExecutable,
    [Parameter(Mandatory = $true)][string]$GitExecutable,
    [Parameter(Mandatory = $true)][string]$PublisherTestsScript,
    [Parameter(Mandatory = $true)][string]$PublishScript,
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [ValidateSet('all', 'producer', 'plan', 'fidelity', 'sequence', 'publisher')]
    [string]$Case = 'all'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\scripts\path_safety.psm1') -Force
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$owned = New-XenOwnedTestDirectory -BasePath $TestRoot -RepositoryRoot $repository

function Get-TreeState([string]$Root) {
    $state = foreach ($entry in Get-ChildItem -LiteralPath $Root -Force -Recurse |
            Sort-Object FullName) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '既有数据快照不允许穿过 reparse point'
        }
        $relative = [IO.Path]::GetRelativePath($Root, $entry.FullName)
        if ($entry.PSIsContainer) { "directory:$relative" } else {
            "file:$relative=" + [Convert]::ToBase64String(
                [IO.File]::ReadAllBytes($entry.FullName))
        }
    }
    return @($state) -join "`n"
}

function Invoke-Entry($Entry, [string]$Base, [bool]$BreakBusiness = $false) {
    if ($Entry.Language -eq 'python') {
        $arguments = @($pythonGuard, $Entry.Script) + $Entry.Arguments + @(
            '--test-root', $Base, '--powershell-executable', $PowerShellExecutable)
        if ($BreakBusiness) { $arguments[3] = Join-Path $owned.RootPath 'missing-input' }
        $hostPath = $PythonExecutable
    } else {
        $parameters = @{} + $Entry.Parameters
        $parameters.TestRoot = $Base
        if ($BreakBusiness) {
            if ($Entry.Name -eq 'publisher') {
                $parameters.GitExecutable = Join-Path $owned.RootPath 'missing-git.exe'
            } else { $parameters.Executable = $PowerShellExecutable }
        }
        $commandFile = Join-Path $owned.RootPath 'command.json'
        @{ Script = $Entry.Script; Parameters = $parameters; Base = $Base } |
            ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $commandFile -Encoding utf8
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            $powershellGuard, '-CommandFile', $commandFile)
        $hostPath = $PowerShellExecutable
    }
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = (& $hostPath @arguments 2>&1) -join "`n"
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    return @{ ExitCode = $exitCode; Output = $output }
}

try {
    $pythonGuard = Join-Path $owned.RootPath 'python_delete_guard.py'
    [IO.File]::WriteAllText($pythonGuard, @'
import os
import pathlib
import runpy
import sys

script = sys.argv.pop(1)
base = os.path.normcase(os.path.abspath(sys.argv[sys.argv.index("--test-root") + 1]))

def guard(event, args):
    if event == "shutil.rmtree" and os.path.normcase(os.path.abspath(args[0])) == base:
        raise RuntimeError("R04_DELETE_BLOCKED: " + str(args[0]))

sys.addaudithook(guard)
sys.argv[0] = script
sys.path.insert(0, str(pathlib.Path(script).parent))
runpy.run_path(script, run_name="__main__")
'@)
    $powershellGuard = Join-Path $owned.RootPath 'powershell_delete_guard.ps1'
    [IO.File]::WriteAllText($powershellGuard, @'
param([string]$CommandFile)
$ErrorActionPreference = 'Stop'
$command = Get-Content -LiteralPath $CommandFile -Raw | ConvertFrom-Json -AsHashtable
function global:Remove-Item {
    [CmdletBinding()]
    param([string[]]$LiteralPath, [string[]]$Path, [switch]$Recurse, [switch]$Force)
    foreach ($candidate in (@($LiteralPath) + @($Path))) {
        if ($candidate -and [IO.Path]::GetFullPath($candidate) -ieq
                [IO.Path]::GetFullPath($command.Base)) {
            throw "R04_DELETE_BLOCKED: $candidate"
        }
    }
    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
}
$parameters = $command.Parameters
& $command.Script @parameters
'@)
    $entries = @(
        @{ Name = 'producer'; Language = 'python';
            Script = Join-Path $PSScriptRoot 'aim_production_red_producer_integration_tests.py';
            Arguments = @('--producer', $ProducerExecutable, '--evaluator',
                (Join-Path $repository 'scripts\evaluate_aim_production_red.py')) },
        @{ Name = 'plan'; Language = 'python';
            Script = Join-Path $PSScriptRoot 'aim_production_red_plan_builder_tests.py';
            Arguments = @('--builder', (Join-Path $repository 'scripts\build_aim_production_red_dbed788b_plan.py'),
                '--fixture-header', (Join-Path $PSScriptRoot 'aim_x_latest_pixel_holdout_fixture.h')) },
        @{ Name = 'fidelity'; Language = 'python';
            Script = Join-Path $PSScriptRoot 'aim_production_red_fidelity_plan_builder_tests.py';
            Arguments = @('--builder', (Join-Path $repository 'scripts\build_aim_production_red_dbed788b_fidelity_plan.py')) },
        @{ Name = 'sequence'; Language = 'powershell';
            Script = Join-Path $PSScriptRoot 'mouse_effect_probe_sequence_cli_tests.ps1';
            Parameters = @{ Executable = $SequenceExecutable } },
        @{ Name = 'publisher'; Language = 'powershell'; Script = $PublisherTestsScript;
            Parameters = @{ PublishScript = $PublishScript; GitExecutable = $GitExecutable } }
    )
    foreach ($entry in $entries) {
        if ($Case -ne 'all' -and $Case -ne $entry.Name) { continue }
        $base = Join-Path $owned.RootPath ($entry.Name + '-existing-base')
        $nested = Join-Path $base 'previous-run'
        [void][IO.Directory]::CreateDirectory($nested)
        [IO.File]::WriteAllBytes((Join-Path $base 'keep.bin'), [byte[]](0, 1, 2, 255, 42))
        [IO.File]::WriteAllText((Join-Path $nested 'keep.txt'), '既有父目录和历史数据必须原样保留')
        $before = Get-TreeState $base
        $result = Invoke-Entry $entry $base
        if ($result.ExitCode -ne 0 -or (Get-TreeState $base) -cne $before) {
            throw "$($entry.Name) 完整业务失败或改动了既有 base：$($result.Output)"
        }
        # 业务失败发生在取得 child 后，必须同样释放本轮目录。
        $failed = Invoke-Entry $entry $base $true
        if ($failed.ExitCode -eq 0 -or (Get-TreeState $base) -cne $before) {
            throw "$($entry.Name) 业务失败后未保持既有 base 或清理本轮 child"
        }
        $link = Join-Path $owned.RootPath ($entry.Name + '-junction')
        [void](New-Item -ItemType Junction -Path $link -Value $base)
        try {
            foreach ($unsafe in @($link, (Join-Path $link 'previous-run'))) {
                $rejected = Invoke-Entry $entry $unsafe
                if ($rejected.ExitCode -eq 0 -or (Get-TreeState $base) -cne $before) {
                    throw "$($entry.Name) 未拒绝 junction 路径链或改变了目标 bytes"
                }
            }
        } finally {
            if ((Split-Path -Parent ([IO.Path]::GetFullPath($link))) -ine $owned.RootPath -or
                ([IO.File]::GetAttributes($link) -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                throw '拒绝清理不属于本轮的 junction'
            }
            [IO.Directory]::Delete($link, $false)
        }
        Write-Host "$($entry.Name)：完整业务、既有 bytes、本轮清理、junction 叶与祖先拒绝通过。"
    }
} finally {
    Remove-XenOwnedTestDirectory -RootPath $owned.RootPath -BasePath $owned.BasePath `
        -RepositoryRoot $repository -OwnerId $owned.OwnerId
}
