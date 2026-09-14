[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare', 'Validate', 'Launch')][string]$Mode,
    [Parameter(Mandatory)][string]$RunDirectory,
    [string]$EngineScript,
    [string]$Executable,
    [string]$ConfigPath,
    [string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [switch]$AllowPhysicalOutput,
    [string]$Confirm
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$runLock = $null
function Quote-PS([string]$Value) { "'" + $Value.Replace("'", "''") + "'" }
function Assert-PlainPath([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path) -or $Path.StartsWith('\\') -or $Path.Contains('"')) { throw '路径必须为本机绝对路径。' }
    for ($part = [IO.Path]::GetFullPath($Path); $part; $part = [IO.Path]::GetDirectoryName($part)) {
        if ((Test-Path -LiteralPath $part) -and (([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw '拒绝重解析路径。' }
    }
}
function Get-Digest([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Write-Json([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 6), $utf8) }
function Read-FireSettings([string]$Path) {
    Assert-PlainPath $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -gt 4096) { throw '配置超过4096字节。' }
        $reader = New-Object IO.StreamReader($stream, (New-Object Text.UTF8Encoding($false, $true)), $true)
        try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    # 两字段专用语法先校验再转数值，不能由ConvertFrom-Json吞掉重复键或转换浮点数。
    $pair = '"(?<key>shot_hold_ms|fire_interval_ms)"\s*:\s*(?<value>0|[1-9][0-9]*)'
    $match = [regex]::Match($text, '\A\s*\{\s*' + $pair + '\s*,\s*' + $pair + '\s*\}\s*\z')
    if (-not $match.Success -or $match.Groups['key'].Captures[0].Value -ceq $match.Groups['key'].Captures[1].Value) { throw '仅允许shot_hold_ms和fire_interval_ms两个不同的整数字段。' }
    $settings = @{}
    for ($index = 0; $index -lt 2; $index++) {
        $number = 0
        if (-not [int]::TryParse($match.Groups['value'].Captures[$index].Value, [ref]$number)) { throw '配置整数超限。' }
        $settings[$match.Groups['key'].Captures[$index].Value] = $number
    }
    if ($settings.shot_hold_ms -lt 1 -or $settings.shot_hold_ms -gt 2000 -or
        $settings.fire_interval_ms -lt 1 -or $settings.fire_interval_ms -gt 5000 -or
        $settings.fire_interval_ms -le $settings.shot_hold_ms) { throw '按住须1..2000ms，间隔须1..5000ms且大于按住时长。' }
    return $settings
}
function New-FirePlan($Settings) {
    return [ordered]@{ schema_version=2; capture_enabled=$false; baseline='stationary'; shots=15;
        fire_delay_ms=1; fire_interval_ms=$Settings.fire_interval_ms; move_during_fire_delay=$false;
        move_ms=1; counter_hold_ms=1; counter_delay_ms=0; shot_after_release_ms=0;
        shot_hold_ms=$Settings.shot_hold_ms; late_tolerance_ms=5; direction=2 }
}
function Test-FirePlan([string]$Binary, [string]$Directory, $Plan) {
    $preview = Join-Path $Directory 'preview-plan.json'; Assert-PlainPath $preview
    Write-Json $preview $Plan
    & $Binary --plan $preview --dry-run --require-current-plan
    if ($LASTEXITCODE -ne 0) { throw '正式计划校验失败。' }
}
try {
    $runPath = [IO.Path]::GetFullPath($RunDirectory); Assert-PlainPath $runPath
    $scriptPath = [IO.Path]::GetFullPath($PSCommandPath); Assert-PlainPath $scriptPath
    if ($Mode -eq 'Prepare') {
        if (-not $EngineScript) { $EngineScript = Join-Path $PSScriptRoot 'invoke_auto_stop_counterpulse.ps1' }
        if ($AllowPhysicalOutput -or $Confirm) { throw 'Prepare不接受物理授权参数。' }
        if (-not $Executable -or -not $ConfigPath) { throw 'Prepare需要正式程序和配置路径。' }
        if (Test-Path -LiteralPath $runPath) { throw '目录已存在，请使用新的独立目录。' }
        $paths = @{ engine=[IO.Path]::GetFullPath($EngineScript); executable=[IO.Path]::GetFullPath($Executable); config=[IO.Path]::GetFullPath($ConfigPath); script=$scriptPath }
        $binding = [ordered]@{ schema_version=1; owner='XEN_WEAPON_FIRE_TEST'; status='PREPARED_NOT_LAUNCHED'; run_directory=$runPath; scope=$Scope; credential_directory='' }
        foreach ($name in @('engine', 'executable', 'config', 'script')) {
            Assert-PlainPath $paths[$name]
            $binding[$name] = $paths[$name]; $binding[$name + '_sha256'] = Get-Digest $paths[$name]
        }
        if ($CredentialDirectory) { $binding.credential_directory = [IO.Path]::GetFullPath($CredentialDirectory); Assert-PlainPath $binding.credential_directory }
        $null = [IO.Directory]::CreateDirectory($runPath); Assert-PlainPath $runPath
        Write-Json (Join-Path $runPath 'fire-settings.json') ([ordered]@{ shot_hold_ms=80; fire_interval_ms=800 })
        Test-FirePlan $binding.executable $runPath (New-FirePlan (Read-FireSettings (Join-Path $runPath 'fire-settings.json')))
        $launch = '& ' + (Quote-PS $scriptPath) + ' -Mode Launch -RunDirectory ' + (Quote-PS $runPath) + ' -AllowPhysicalOutput -Confirm WEAPON_FIRE_TEST'
        [IO.File]::WriteAllText((Join-Path $runPath 'launch-test.ps1'), ($launch + "`r`n"), (New-Object Text.UTF8Encoding($true)))
        $start = "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0launch-test.ps1`"`r`npause`r`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'start-test.bat'), $start, [Text.Encoding]::ASCII)
        [IO.File]::WriteAllText((Join-Path $runPath 'edit-config.bat'), "@echo off`r`nnotepad.exe `"%~dp0fire-settings.json`"`r`n", [Text.Encoding]::ASCII)
        $check = '& ' + (Quote-PS $scriptPath) + ' -Mode Validate -RunDirectory ' + (Quote-PS $runPath)
        [IO.File]::WriteAllText((Join-Path $runPath 'check-config.ps1'), ($check + "`r`n"), (New-Object Text.UTF8Encoding($true)))
        [IO.File]::WriteAllText((Join-Path $runPath 'check-config.bat'), $start.Replace('launch-test.ps1', 'check-config.ps1'), [Text.Encoding]::ASCII)
        $binding.launcher_sha256 = Get-Digest (Join-Path $runPath 'launch-test.ps1')
        Write-Json (Join-Path $runPath 'task.json') $binding
        $taskText = "# 原地射击测试`n`nPREPARED_NOT_LAUNCHED。固定每组15次左键按住，不代表15颗子弹；无移动动作，关闭模型HUD。`n`nfire-settings.json仅接受shot_hold_ms和fire_interval_ms两个整数字段。按住1..2000ms；相邻左键DOWN提交最小间隔1..5000ms且大于按住时长。默认80/800ms是观察与操作余量起点，不是人体反应常数，也不证明后座已恢复。`n`nedit-config.bat编辑参数，check-config.bat仅校验并生成preview-plan.json，不发送输入或创建真实组；start-test.bat由用户前台启动。每组独立保存到runs/时间GUID，期间编辑只影响下一组。End或人工输入取消沿用正式引擎；结束后需人工观察，不自动判断准确度。`n`n``````powershell`n$launch`n```````n"
        [IO.File]::WriteAllText((Join-Path $runPath 'TASK.md'), $taskText, $utf8)
        [Console]::WriteLine('PREPARED_NOT_LAUNCHED；未发送设备输入。')
    } else {
        if ($Mode -eq 'Launch') {
            if (-not $AllowPhysicalOutput -or $Confirm -cne 'WEAPON_FIRE_TEST') { throw '缺少本轮物理输出授权。' }
            if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) { throw '仅允许用户本机前台启动。' }
        } elseif ($AllowPhysicalOutput -or $Confirm) { throw 'Validate不接受物理授权。' }
        foreach ($name in @('EngineScript','Executable','ConfigPath','CredentialDirectory','Scope')) {
            if ($PSBoundParameters.ContainsKey($name)) { throw 'Launch只使用Prepare绑定，不接受路径或作用域覆盖。' }
        }
        $lockPath = Join-Path $runPath '.weapon-fire.lock'; Assert-PlainPath $lockPath
        $runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $taskPath = Join-Path $runPath 'task.json'; Assert-PlainPath $taskPath
        if ((Get-Item -LiteralPath $taskPath).Length -gt 64KB) { throw '绑定清单超限。' }
        $task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($task.schema_version -ne 1 -or $task.owner -cne 'XEN_WEAPON_FIRE_TEST' -or $task.status -cne 'PREPARED_NOT_LAUNCHED' -or $task.run_directory -cne $runPath -or $task.script -cne $scriptPath -or $task.scope -notin @('CurrentUser','LocalMachine')) { throw '绑定清单无效。' }
        foreach ($name in @('engine', 'executable', 'config', 'script')) {
            Assert-PlainPath $task.$name
            if ((Get-Digest $task.$name) -cne $task.($name + '_sha256')) { throw '正式文件已变化，需重新Prepare。' }
        }
        $launcher = Join-Path $runPath 'launch-test.ps1'; Assert-PlainPath $launcher
        if ((Get-Digest $launcher) -cne $task.launcher_sha256) { throw '启动入口已变化。' }
        if ($task.credential_directory) { Assert-PlainPath $task.credential_directory }
        $settings = Read-FireSettings (Join-Path $runPath 'fire-settings.json')
        $plan = New-FirePlan $settings
        Test-FirePlan $task.executable $runPath $plan
        if ($Mode -eq 'Validate') { [Console]::WriteLine('VALIDATED_NO_PHYSICAL_OUTPUT；仅更新preview-plan.json，未创建真实组。'); return }
        $runs = Join-Path $runPath 'runs'; Assert-PlainPath $runs
        $null = [IO.Directory]::CreateDirectory($runs); Assert-PlainPath $runs
        $group = Join-Path $runs ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')); Assert-PlainPath $group
        $prepare = @{ Mode='Prepare'; RunDirectory=$group; Executable=$task.executable; ConfigPath=$task.config;
            Baseline=$plan.baseline; Shots=$plan.shots; FireDelayMs=$plan.fire_delay_ms; FireIntervalMs=$plan.fire_interval_ms; MoveDuringFireDelay=$plan.move_during_fire_delay;
            Direction='A'; MoveMs=$plan.move_ms; CounterHoldMs=$plan.counter_hold_ms; CounterDelayMs=$plan.counter_delay_ms; ShotAfterReleaseMs=$plan.shot_after_release_ms; ShotHoldMs=$plan.shot_hold_ms;
            LateToleranceMs=$plan.late_tolerance_ms; Scope=$task.scope }
        if ($task.credential_directory) { $prepare.CredentialDirectory = $task.credential_directory }
        & $task.engine @prepare
        if (-not $?) { throw '正式Prepare失败，未启动。' }
        Assert-PlainPath $group
        Write-Json (Join-Path $group 'fire-settings.json') $settings
        # 固定关闭所有模型HUD；原地输入模型零速不能用于判断后座恢复。
        $samplingPath = Join-Path $group 'sampling-settings.json'; Assert-PlainPath $samplingPath
        if ((Get-Item -LiteralPath $samplingPath).Length -gt 16KB) { throw '模型配置超限。' }
        $samplingText = Get-Content -LiteralPath $samplingPath -Raw -Encoding UTF8
        $samplingText = [regex]::Replace($samplingText, '"hud_enabled"\s*:\s*true', '"hud_enabled": false')
        if ($samplingText -notmatch '"hud_enabled"\s*:\s*false') { throw '无法确认HUD关闭，未启动。' }
        [IO.File]::WriteAllText($samplingPath, $samplingText, $utf8)
        [Console]::WriteLine('本组固定15次左键按住；不代表子弹数。结果目录：' + $group)
        $launchArguments = @{ Mode='Launch'; RunDirectory=$group; AllowPhysicalOutput=$true; Confirm='AUTO_STOP_COUNTERPULSE'; Scope=$task.scope }
        if ($task.credential_directory) { $launchArguments.CredentialDirectory = $task.credential_directory }
        & $task.engine @launchArguments
        if (-not $?) { throw '本组未完成；保留本组诊断，不自动重试。' }
    }
} catch {
    [Console]::Error.WriteLine('[WEAPON_FIRE_TEST_FAILED] 未启动或未完成；检查前台授权、两项参数、绑定文件和组内诊断。')
    exit 1
} finally {
    if ($null -ne $runLock) { $runLock.Dispose() }
}
