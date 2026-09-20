param([string]$TestRoot = (Join-Path $PSScriptRoot '../cache/recoil-acceptance-tests'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$owned = New-XenOwnedTestDirectory -BasePath $TestRoot -RepositoryRoot (Split-Path -Parent $PSScriptRoot)
$root = $owned.RootPath
$entry = Join-Path $PSScriptRoot 'invoke_recoil_legacy_acceptance.ps1'
$passed = 0
function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:passed++
}
function Write-Fixture([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Reject-Test([hashtable]$Arguments, [string]$Message) {
    $failed = $false
    try { & $entry @Arguments | Out-Null } catch { $failed = $true }
    Assert-Test $failed $Message
}
try {
    $package = Join-Path $root 'package'
    $run = Join-Path $root 'run'
    Write-Fixture (Join-Path $package 'XenLauncher.exe') 'fake-launcher'
    Write-Fixture (Join-Path $package 'runtimes/nvidia/Xen.exe') 'fake-worker'
    $config = "[detector]`nbackend=tensorrt`n[recoil]`nenabled=false`nmixed_aim=false`nprofile_directory=cache/recoil/profiles`n"
    Write-Fixture (Join-Path $package 'config.ini') $config
    $manifest = @{ schema = 1; product = 'Xen'; runtimes = @(@{id='nvidia'; executable='runtimes/nvidia/Xen.exe'; backends=@('tensorrt')}) }
    Write-Fixture (Join-Path $package 'manifest.json') ($manifest | ConvertTo-Json -Depth 8)
    $active = @{ schema_version=1; active=@{} }
    foreach ($index in 1..17) {
        $active.active["weapon_test$index"] = @{file="test$index.json"; previous=''}
        Write-Fixture (Join-Path $package "cache/recoil/profiles/test$index.json") '{"schema_version":3,"verified":true}'
    }
    Write-Fixture (Join-Path $package 'cache/recoil/profiles/active.json') ($active | ConvertTo-Json -Depth 8)
    Write-Fixture (Join-Path $package 'cache/runtime/old.json') 'untouched-old'
    $arguments = @{ Mode='Prepare'; PackageRoot=$package; RunDirectory=$run }
    $bad = $arguments.Clone(); $bad.AllowPhysicalOutput=$true
    Reject-Test $bad 'Prepare拒绝物理开关'
    & $entry @arguments | Out-Null
    Assert-Test ((Get-Content -LiteralPath (Join-Path $package 'config.ini') -Raw) -ceq $config) 'Prepare不改原配置'
    $task = Get-Content -LiteralPath (Join-Path $run 'task.json') -Raw | ConvertFrom-Json
    Assert-Test (-not $task.automatic_timeout -and -not $task.automatic_fire) '人工窗口不伪装自动超时或自动开火'
    $taskText = Get-Content -LiteralPath (Join-Path $run 'TASK.md') -Raw -Encoding UTF8
    Assert-Test ($taskText.Contains($task.launch_command)) 'TASK回读唯一Launch'
    Assert-Test ([regex]::Matches($taskText, '-Mode Launch').Count -eq 1) '仅一个Launch命令'
    $launch = @{ Mode='Launch'; PackageRoot=$package; RunDirectory=$run }
    Reject-Test $launch '缺少物理授权拒绝'
    $launch.AllowPhysicalOutput=$true
    $launch.PhysicalOutputConfirmation='wrong'
    Reject-Test $launch '错误令牌拒绝'
    $launch.PhysicalOutputConfirmation='XEN_RECOIL_LEGACY_ACCEPT_SENDS_REAL_KMBOX_INPUT'
    Write-Fixture (Join-Path $package 'config.ini') ($config + '; changed')
    Reject-Test $launch '配置漂移拒绝'
    Write-Fixture (Join-Path $package 'config.ini') $config
    function Get-Process { param($Name, $ErrorAction); return @() }
    function Start-Process {
        param($FilePath, $WorkingDirectory, $WindowStyle, [switch]$PassThru)
        if ($WindowStyle -ne 'Normal') { throw 'UI需要用户可见' }
        $process = [pscustomobject]@{ Id=12345; ExitCode=0 }
        $process | Add-Member ScriptMethod WaitForExit { return }
        return $process
    }
    & $entry @launch | Out-Null
    Assert-Test (Test-Path -LiteralPath (Join-Path $run 'launch.json')) '模拟UI退出后保留Launch记录'
    Reject-Test $launch '禁止同Run重复Launch'
    Write-Fixture (Join-Path $package 'cache/runtime/new-run.json') 'new-debug-report'
    Write-Fixture (Join-Path $package 'cache/runtime/new-run-recoil-batches/batch.json') 'new-batch'
    & $entry -Mode Recover -PackageRoot $package -RunDirectory $run
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $run 'reports/old.json'))) 'Recover不复制历史报告'
    $summary = Get-Content -LiteralPath (Join-Path $run 'automatic-summary.json') -Raw | ConvertFrom-Json
    Assert-Test ($summary.collected_files.Count -eq 2) '仅收新增Debug和batch'
    Assert-Test (-not $summary.physical_effect_verified -and $summary.human_observation_required) '回收不宣称真实效果'
    Assert-Test (-not $summary.config_changed_during_ui) '原配置保持'
    Write-Host "PASS: $script:passed 项压枪人工验收入口测试；Start-Process仅自制替身，无设备或应用启动。"
} finally {
    Remove-Item Function:Get-Process -ErrorAction SilentlyContinue
    Remove-Item Function:Start-Process -ErrorAction SilentlyContinue
    Remove-XenOwnedTestDirectory -RootPath $root -BasePath $owned.BasePath -OwnerId $owned.OwnerId -RepositoryRoot (Split-Path -Parent $PSScriptRoot)
}
