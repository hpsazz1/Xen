[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare', 'Launch')][string]$Mode,
    [Parameter(Mandatory)][string]$RunDirectory,
    [switch]$ReuseRunDirectory,
    [switch]$Repeatable,
    [string]$Executable,
    [string]$ConfigPath,
    [ValidateSet('stationary', 'no_counter', 'counter')][string]$Baseline = 'counter',
    [ValidateRange(1, 30)][int]$Shots = 20,
    [ValidateRange(0, 2000)][int]$FireDelayMs = 300,
    [ValidateRange(0, 5000)][int]$FireIntervalMs = 0,
    [ValidateRange(0, 2000)][int]$RestartIntervalMs = 0,
    [bool]$OverlapFireInterval = $false,
    [bool]$MoveDuringFireDelay = $true,
    [ValidateSet('A', 'D')][string]$Direction = 'A',
    [ValidateRange(1, 500)][int]$MoveMs = 300,
    [ValidateRange(1, 200)][int]$CounterHoldMs = 5,
    [ValidateRange(0, 200)][int]$CounterDelayMs = 50,
    [ValidateRange(0, 20)][int]$ShotAfterReleaseMs = 0,
    [ValidateRange(1, 2000)][int]$ShotHoldMs = 5,
    [ValidateRange(0, 10)][int]$LateToleranceMs = 5,
    [switch]$AllowPhysicalOutput,
    [string]$Confirm,
    [string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$script:FailureCode = 'ENTRY_VALIDATION_FAILED'
function Show-Startup([string]$Directory, [string]$Previous) {
    $path = Join-Path $Directory 'startup.json'
    if (-not (Test-Path -LiteralPath $path)) { return $Previous }
    try {
        $state = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        $phase = [string]$state.stage
        $reason = if ($state.PSObject.Properties.Name -contains 'readiness') { [string]$state.readiness.reason } else { '' }
        if ($phase -notmatch '^[A-Z_]{1,40}$' -or ($reason -and $reason -notmatch '^[A-Z_]{1,60}$')) { return $Previous }
        $current = $phase + ':' + $reason
        if ($current -ne $Previous) {
            $hints = @{
                SOURCE_NOT_FOCUSED = '请将测试游戏切到前台'
                SOURCE_UNAVAILABLE = '源焦点服务暂无有效报告'
                MONITOR_WAITING = '尚未收到首个键态报告，请单独轻按松开Shift，不按方向键或鼠标'
                MONITOR_INVALID = '键态报告无效'
                MONITOR_POLL_FAILED = '读取键态失败'
                PHYSICAL_KEYS_HELD = '请松开WASD和鼠标按钮'
                STABILIZING = '正在确认连续就绪'
                READY = '已就绪，即将自动执行本组'
            }
            $hint = if ($phase -eq 'READINESS' -and $hints.ContainsKey($reason)) { ' ' + $hints[$reason] } else { '' }
            [Console]::WriteLine('[COUNTERPULSE] ' + $current + $hint)
        }
        return $current
    } catch { return $Previous }
}
function Get-Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Quote-PS([string]$Value) { "'" + $Value.Replace("'", "''") + "'" }
# 注释只由固定字段说明生成；值仍由JSON序列化器转义。
$planHelp = [ordered]@{
    schema_version='固定为2，新版动作与评价契约。'; capture_enabled='固定false，不采集图像。';
    baseline='counter反向轻点；no_counter仅松键；stationary原地开火。';
    shots='开火次数1..30，不代表实际子弹数。';
    fire_delay_ms='上一轮左键UP ACK后的等待0..2000ms；与移动是否并行由下一开关决定。';
    fire_interval_ms='相邻左键DOWN提交的最小间隔0..5000ms；0关闭。冷却在下一轮移动前，不是精确周期。';
    restart_interval_ms='反向UP ACK至下一正向DOWN提交的最小间隔0..2000ms；与左键释放及既有目标取较晚时刻。0保持原行为；非counter必须0；无前次反向UP时不限制。';
    move_during_fire_delay='true等待与移动并行；false先等待再移动。';
    overlap_fire_interval='缺省false保持原顺序；true在武器间隔内安排移动，move_ms为上限，完整保留反向及松键后等待。需fire_delay_ms=0且move_during_fire_delay=false。';
    move_ms='正向键DOWN ACK起保持1..500ms；动态模式为上限，剩余窗口不足时缩短。';
    counter_hold_ms='反向键实际保持1..200ms。'; counter_delay_ms='正向UP ACK至反向DOWN的等待0..200ms；非counter必须0。';
    shot_after_release_ms='最后方向键UP ACK至计划开火的等待0..20ms；0立即计划开火。';
    shot_hold_ms='左键DOWN ACK起保持1..2000ms；1000即按住1秒。';
    late_tolerance_ms='动作调度迟到容差0..10ms。'; direction='2表示A，8表示D。'
}
function Write-CommentedPlan([string]$Path, $Value, $Descriptions = $planHelp) {
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add('{')
    $keys = @($Value.Keys)
    for ($index = 0; $index -lt $keys.Count; $index++) {
        $key = $keys[$index]
        $lines.Add('  // ' + $Descriptions[$key])
        $suffix = if ($index + 1 -lt $keys.Count) { ',' } else { '' }
        $lines.Add('  "' + $key + '": ' + (ConvertTo-Json -InputObject $Value[$key] -Compress) + $suffix)
    }
    $lines.Add('}')
    [IO.File]::WriteAllText($Path, ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($false)))
}
$samplingDefaults = [ordered]@{ max_move_speed=1.0; clean_shot_speed_ratio=0.34; accel_per_sec=5.5;
    natural_decel_per_sec=2.5; counter_strafe_accel_per_sec=14.0; fire_sample_delay_ms=18;
    tap_max_hold_ms=90; auto_fire_interval_ms=100; hud_enabled=$true }
$samplingHelp = [ordered]@{ max_move_speed='模型最大速度，不是游戏实测速率。'; clean_shot_speed_ratio='模型稳定阈值比例。';
    accel_per_sec='模型同向加速度。'; natural_decel_per_sec='模型自然减速度。'; counter_strafe_accel_per_sec='模型反向减速度。';
    fire_sample_delay_ms='首次模型采样相对按下的延时ms。'; tap_max_hold_ms='模型短按界限ms。';
    auto_fire_interval_ms='持续按住的模型采样间隔ms，不是实际射速或开火命令间隔。';
    hud_enabled='true显示本程序模型HUD，false关闭；一次启动冻结生效，不在运行中热改。' }
function Read-BoundedBytes([string]$Path) {
    Assert-PlainPath $Path
    $reader = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($reader.Length -gt 16384) { throw '配置超过大小限制。' }
        $bytes = New-Object byte[] ([int]$reader.Length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $count = $reader.Read($bytes, $offset, $bytes.Length - $offset)
            if ($count -le 0) { throw '配置读取不完整。' }
            $offset += $count
        }
        return ,$bytes
    } finally { $reader.Dispose() }
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
}
# 复用只清理本工具目录内的结果；先拒绝整条路径及结果树的重解析点。
function Assert-PlainPath([string]$Path) {
    for ($item = [IO.Path]::GetFullPath($Path); $item; $item = [IO.Path]::GetDirectoryName($item)) {
        if ((Test-Path -LiteralPath $item) -and
            (([IO.File]::GetAttributes($item) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw '路径含重解析点。' }
    }
}
function Assert-ResultTree([string]$Root) {
    Assert-PlainPath $Root
    if (-not (Test-Path -LiteralPath $Root)) { return }
    if (-not [IO.Directory]::Exists($Root)) { throw '结果路径不是目录。' }
    $pending = New-Object 'Collections.Generic.Stack[string]'
    $pending.Push($Root)
    while ($pending.Count -gt 0) {
        foreach ($item in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())) {
            $attrs = [IO.File]::GetAttributes($item)
            if (($attrs -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw '结果树含重解析点。' }
            if (($attrs -band [IO.FileAttributes]::Directory) -ne 0) { $pending.Push($item) }
        }
    }
}
function Assert-ProbeStopped([string]$Binary) {
    foreach ($process in @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Binary)) -ErrorAction SilentlyContinue)) {
        if (-not $process.Path -or $process.Path -ieq $Binary) { throw '绑定探针仍运行。' }
    }
}
function Assert-OwnedRun($Existing) {
    if ($Existing.schema_version -notin @(1, 2, 3, 4) -or $Existing.status -ne 'PREPARED_NOT_LAUNCHED' -or
        $Existing.plan -cne $planPath -or $Existing.run_id -cne [IO.Path]::GetFileName($runPath)) { throw '不是本工具绑定目录。' }
    if ($Existing.schema_version -ge 2 -and ($Existing.owner -cne 'XEN_AUTO_STOP_COUNTERPULSE' -or
        $Existing.run_directory -cne $runPath)) { throw '目录所有权无效。' }
    if ($Existing.schema_version -eq 3 -and $Existing.repeatable -ne $true) { throw '重复运行绑定无效。' }
    if ([IO.Path]::GetFileName($Existing.script) -notmatch '^invoke_auto_stop_counterpulse(-r[0-9]+)?\.ps1$') { throw '原入口身份无效。' }
    foreach ($name in @('executable', 'config', 'plan', 'script')) {
        Assert-PlainPath $Existing.$name
        if ($name -eq 'plan' -and ($Existing.schema_version -eq 3 -or ($Existing.schema_version -eq 4 -and $Existing.repeatable))) { continue }
        # 显式Prepare允许正式程序/入口升级后重新绑定；配置与计划仍按原绑定核对。
        if ($name -in @('executable', 'script')) { continue }
        if ((Get-Digest $Existing.$name) -cne $Existing.($name + '_sha256')) { throw '原目录绑定已变化。' }
    }
    # 旧 schema 的入口没有共享锁；迁移前还须排除仍运行的绑定探针。
    Assert-ProbeStopped $Existing.executable
}
function Invoke-ReportHud([string]$Binary, [string]$Report) {
    if (-not [IO.File]::Exists($Report)) { return }
    Assert-PlainPath $Report
    if ((Get-Item -LiteralPath $Report).Length -gt 64MB) { throw 'HUD报告超出预算。' }
    $analysis = Get-Content -LiteralPath $Report -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($analysis.settings.hud_enabled -ne $true) { return }
    foreach ($argument in @($Binary, $Report)) { if ($argument.Contains('"')) { throw 'HUD路径含引号。' } }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Binary; $info.UseShellExecute = $false
    $info.WorkingDirectory = Split-Path -Parent $Binary
    $info.Arguments = '--show-hud "' + $Report + '"'
    # 独立只读查看进程，不继承物理授权，也不进入物理探针的60秒等待。
    $child = [Diagnostics.Process]::Start($info)
    $child.Dispose()
}
function Invoke-Probe([string]$Binary, [string[]]$Arguments, [bool]$Physical) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Binary
    $info.WorkingDirectory = Split-Path -Parent $Binary
    $info.UseShellExecute = $false
    # 参数均为本地路径或固定参数；拒绝引号，避免命令行歧义。
    $quoted = foreach ($argument in $Arguments) {
        if ($argument.Contains('"')) { throw '参数包含不支持的引号。' }
        '"' + $argument.TrimEnd('\') + '"'
    }
    $info.Arguments = $quoted -join ' '
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $plain = $null
    try {
        if ($Physical -and $CredentialDirectory) {
            Add-Type -AssemblyName System.Security
            $credentialFile = Join-Path ([IO.Path]::GetFullPath($CredentialDirectory)) 'token.dpapi'
            for ($ancestor = $credentialFile; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
                if ((Test-Path -LiteralPath $ancestor) -and
                    (([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                    throw '凭据路径不能包含重解析点。'
                }
            }
            $plain = [Security.Cryptography.ProtectedData]::Unprotect(
                [IO.File]::ReadAllBytes($credentialFile), $null, [Security.Cryptography.DataProtectionScope]::$Scope)
            $info.EnvironmentVariables['XEN_SOURCE_CONTEXT_TOKEN'] = [Text.Encoding]::UTF8.GetString($plain)
        }
        $child = [Diagnostics.Process]::Start($info)
        try {
            # 不转发子进程原始输出，避免配置或异常携带凭据。
            $stdout = $child.StandardOutput.ReadToEndAsync()
            $stderr = $child.StandardError.ReadToEndAsync()
            $timeoutMs = if ($Physical) { 60000 } else { 30000 }
            $until = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
            $previous = ''
            while (-not $child.WaitForExit(100) -and [DateTime]::UtcNow -lt $until) {
                if ($Physical) { $previous = Show-Startup (Join-Path $runPath 'result') $previous }
            }
            if (-not $child.HasExited) {
                if ($Physical) {
                    # 只向本次Run写取消请求，不另开设备发送补偿命令。
                    try {
                        $outputIndex = [Array]::IndexOf($Arguments, '--output')
                        if ($outputIndex -ge 0 -and $outputIndex + 1 -lt $Arguments.Length) {
                            $stopDirectory = $Arguments[$outputIndex + 1]
                            $null = [IO.Directory]::CreateDirectory($stopDirectory)
                            [IO.File]::WriteAllText((Join-Path $stopDirectory 'STOP'), 'TIMEOUT_CANCEL_REQUESTED')
                        }
                    } catch { }
                    [Console]::Error.WriteLine('反向轻点进程超时，已尝试请求取消；清理状态未知，请人工确认设备。')
                }
                if (-not $child.WaitForExit(5000)) {
                    $child.Kill()
                    $null = $child.WaitForExit(5000)
                }
                throw '反向轻点进程超时，不能认定完成。'
            }
            $null = $stdout.GetAwaiter().GetResult()
            $null = $stderr.GetAwaiter().GetResult()
            if ($child.ExitCode -ne 0) {
                $script:FailureCode = 'PROBE_FAILED'
                if ($Physical) {
                    $previous = Show-Startup (Join-Path $runPath 'result') $previous
                    $failurePath = Join-Path $runPath 'result/failure.json'
                    if (-not (Test-Path -LiteralPath $failurePath)) { $failurePath = Join-Path $runPath 'result/result.json' }
                    if (Test-Path -LiteralPath $failurePath) {
                        try {
                            $failure = Get-Content -LiteralPath $failurePath -Raw -Encoding UTF8 | ConvertFrom-Json
                            foreach ($field in @('reason', 'failure', 'post_roll_failure')) {
                                if ($failure.PSObject.Properties.Name -contains $field -and [string]$failure.$field -match '^[A-Z_]{1,60}$') {
                                    $script:FailureCode = [string]$failure.$field
                                    break
                                }
                            }
                        } catch {}
                    }
                }
                throw '探针未通过；请检查计划或本次结果。'
            }
        } finally { $child.Dispose() }
    } finally {
        $info.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
        if ($null -ne $plain) { [Array]::Clear($plain, 0, $plain.Length) }
    }
}

$runLock = $null
$candidatePlan = $null
$candidateSettings = $null
try {
    $runPath = [IO.Path]::GetFullPath($RunDirectory)
    $scriptPath = [IO.Path]::GetFullPath($PSCommandPath)
    $planPath = Join-Path $runPath 'plan.json'
    $taskPath = Join-Path $runPath 'task.json'
    $samplingPath = Join-Path $runPath 'sampling-settings.json'
    Assert-PlainPath $runPath
    if ($Mode -eq 'Launch' -and ($ReuseRunDirectory -or $Repeatable)) { throw '复用模式仅适用于Prepare。' }
    if ($Mode -eq 'Prepare') {
        if ($PSBoundParameters.ContainsKey('AllowPhysicalOutput') -or $PSBoundParameters.ContainsKey('Confirm')) { throw 'Prepare禁止混入物理授权参数。' }
        if (-not $Executable -or -not $ConfigPath) { throw 'Prepare需要Executable和ConfigPath。' }
        $exists = Test-Path -LiteralPath $runPath
        if ($exists -and -not $ReuseRunDirectory) { throw 'Run目录已存在，拒绝覆盖。' }
        if ($exists -and -not [IO.File]::Exists($taskPath)) { throw '已有目录不属于本工具。' }
        $binary = (Resolve-Path -LiteralPath $Executable).Path
        $config = (Resolve-Path -LiteralPath $ConfigPath).Path
        if (-not [IO.File]::Exists($binary) -or -not [IO.File]::Exists($config)) { throw '需要有效文件。' }
        # 非反向基线没有反向等待；仅调整未由用户指定的默认值。
        if ($Baseline -ne 'counter' -and -not $PSBoundParameters.ContainsKey('CounterDelayMs')) { $CounterDelayMs = 0 }
        $plan = [ordered]@{ schema_version = 2; capture_enabled = $false; baseline = $Baseline; shots = $Shots; fire_delay_ms = $FireDelayMs; fire_interval_ms = $FireIntervalMs; move_during_fire_delay = $MoveDuringFireDelay;
            move_ms = $MoveMs; counter_hold_ms = $CounterHoldMs; counter_delay_ms = $CounterDelayMs; shot_after_release_ms = $ShotAfterReleaseMs; shot_hold_ms = $ShotHoldMs;
            late_tolerance_ms = $LateToleranceMs; direction = $(if ($Direction -eq 'A') { 2 } else { 8 }) }
        if ($OverlapFireInterval) { $plan['overlap_fire_interval'] = $true }
        if ($RestartIntervalMs -ne 0) { $plan['restart_interval_ms'] = $RestartIntervalMs }
        if (-not $exists) { $null = New-Item -ItemType Directory -Path $runPath }
        $lockPath = Join-Path $runPath '.counterpulse.lock'
        Assert-PlainPath $lockPath
        $runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        foreach ($leaf in @('task.json', 'plan.json', 'TASK.md', 'CONSUMED', 'start-test.bat', 'edit-config.bat', 'launch-test.ps1', 'PARAMETERS.md', 'open-report.bat', 'open-report.ps1', 'sampling-settings.json', 'execution-sampling-settings.json', 'edit-sampling.bat', 'start-recording.bat', 'record-manual.ps1', 'show-hud.bat', 'show-hud.ps1', 'manual-recordings', 'analyze-recording.ps1', 'analyze-recording.bat', 'manual-reviews', 'prepare-default-test.ps1', 'prepare-default-test.bat', 'default-baselines')) { Assert-PlainPath (Join-Path $runPath $leaf) }
        if ($exists) {
            $existingTask = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json
            Assert-OwnedRun $existingTask
            if (-not $PSBoundParameters.ContainsKey('Repeatable')) { $Repeatable = ($existingTask.schema_version -eq 3 -or ($existingTask.schema_version -eq 4 -and $existingTask.repeatable)) }
        }
        $resultPath = Join-Path $runPath 'result'
        Assert-ResultTree $resultPath
        $candidatePlan = Join-Path $runPath ('plan.' + [Guid]::NewGuid().ToString('N') + '.candidate.json')
        if ($exists) {
            # 正式解析器处理JSONC与版本迁移，不能用正则剥注释或将旧参数带入新计划。
            Invoke-Probe $binary @('--migrate-plan', $planPath, '--output', $candidatePlan) $false
            $inherited = Get-Content -LiteralPath $candidatePlan -Raw -Encoding UTF8 | ConvertFrom-Json
            $fields = [ordered]@{ Baseline='baseline'; Shots='shots'; FireDelayMs='fire_delay_ms'; FireIntervalMs='fire_interval_ms';
                MoveDuringFireDelay='move_during_fire_delay'; MoveMs='move_ms'; CounterHoldMs='counter_hold_ms';
                CounterDelayMs='counter_delay_ms'; ShotAfterReleaseMs='shot_after_release_ms';
                ShotHoldMs='shot_hold_ms'; LateToleranceMs='late_tolerance_ms'; Direction='direction' }
            foreach ($parameter in $fields.Keys) {
                $field = $fields[$parameter]
                if (-not $PSBoundParameters.ContainsKey($parameter)) { $plan[$field] = $inherited.$field }
            }
            # 旧计划没有该可选字段；未显式覆盖时完整保留动态调度语义。
            if (-not $PSBoundParameters.ContainsKey('OverlapFireInterval')) {
                $OverlapFireInterval = $inherited.PSObject.Properties.Name -contains 'overlap_fire_interval' -and $inherited.overlap_fire_interval
            }
            if ($OverlapFireInterval) { $plan['overlap_fire_interval'] = $true }
            else { $plan.Remove('overlap_fire_interval') }
            if (-not $PSBoundParameters.ContainsKey('RestartIntervalMs')) {
                $RestartIntervalMs = if ($inherited.PSObject.Properties.Name -contains 'restart_interval_ms') { $inherited.restart_interval_ms } else { 0 }
            }
            if ($RestartIntervalMs -ne 0) { $plan['restart_interval_ms'] = $RestartIntervalMs }
            else { $plan.Remove('restart_interval_ms') }
            if ($PSBoundParameters.ContainsKey('Baseline') -and $Baseline -ne 'counter' -and
                -not $PSBoundParameters.ContainsKey('CounterDelayMs')) { $plan['counter_delay_ms'] = 0 }
            foreach ($parameter in $fields.Keys) {
                $value = $plan[$fields[$parameter]]
                if ($parameter -eq 'Direction') { $value = if ($value -eq 2) { 'A' } else { 'D' } }
                Set-Variable -Name $parameter -Value $value
            }
        }
        if (-not (Test-Path -LiteralPath $samplingPath)) { Write-CommentedPlan $samplingPath $samplingDefaults $samplingHelp }
        Write-CommentedPlan $candidatePlan $plan
        Invoke-Probe $binary @('--plan', $candidatePlan, '--dry-run', '--require-current-plan', '--sampling-settings', $samplingPath) $false
        # 所有验证通过之后才移除上组结果，并重新武装用户前台的一次性命令。
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Recurse -Force }
        $consumed = Join-Path $runPath 'CONSUMED'
        if (Test-Path -LiteralPath $consumed) { Remove-Item -LiteralPath $consumed -Force }
        Move-Item -LiteralPath $candidatePlan -Destination $planPath -Force
        $candidatePlan = $null
        $task = [ordered]@{ schema_version = 4; repeatable = [bool]$Repeatable; owner = 'XEN_AUTO_STOP_COUNTERPULSE'; run_directory = $runPath; status = 'PREPARED_NOT_LAUNCHED'; run_id = [IO.Path]::GetFileName($runPath);
            executable = $binary; config = $config; plan = $planPath; script = $scriptPath;
            executable_sha256 = (Get-Digest $binary); config_sha256 = (Get-Digest $config);
            plan_sha256 = (Get-Digest $planPath); script_sha256 = (Get-Digest $scriptPath) }
        if ($Repeatable) { $task.repeatable = $true }
        Write-Json $taskPath $task
        $launch = '& ' + (Quote-PS $scriptPath) + ' -Mode Launch -RunDirectory ' + (Quote-PS $runPath) +
            ' -AllowPhysicalOutput -Confirm AUTO_STOP_COUNTERPULSE'
        if ($CredentialDirectory) {
            $launch += ' -CredentialDirectory ' + (Quote-PS ([IO.Path]::GetFullPath($CredentialDirectory))) + ' -Scope ' + $Scope
        }
        # BAT不嵌入绝对路径或参数，避免cmd再次解释路径中的%、!和&。
        $launchScript = "# 仅用户在前台触发；真实输入确认字符串不是凭据。`r`n" + $launch + "`r`nif (-not `$?) { exit 1 }`r`nexit 0`r`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'launch-test.ps1'), $launchScript, (New-Object Text.UTF8Encoding($true)))
        $startBat = '@echo off' + "`r`n" + 'setlocal DisableDelayedExpansion' + "`r`n" +
            '"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch-test.ps1"' + "`r`n" +
            'set "testExitCode=%errorlevel%"' + "`r`n" + 'pause' + "`r`n" + 'exit /b %testExitCode%' + "`r`n"
        $editBat = '@echo off' + "`r`n" + 'setlocal DisableDelayedExpansion' + "`r`n" +
            '"%SystemRoot%\System32\notepad.exe" "%~dp0plan.json"' + "`r`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'start-test.bat'), $startBat, [Text.Encoding]::ASCII)
        [IO.File]::WriteAllText((Join-Path $runPath 'edit-config.bat'), $editBat, [Text.Encoding]::ASCII)
        $editSamplingBat = $editBat.Replace('plan.json', 'sampling-settings.json')
        [IO.File]::WriteAllText((Join-Path $runPath 'edit-sampling.bat'), $editSamplingBat, [Text.Encoding]::ASCII)
        $manualCommon = @'
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
function Assert-LocalPlain([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path) -or $Path.StartsWith('\\') -or $Path.Contains('"')) { throw '路径必须是本机绝对路径。' }
    for ($item = [IO.Path]::GetFullPath($Path); $item; $item = [IO.Path]::GetDirectoryName($item)) {
        if ((Test-Path -LiteralPath $item) -and (([IO.File]::GetAttributes($item) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw '路径含重解析点。' }
    }
}
try {
    if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) { throw '只允许本机用户前台录制或查看。' }
    Assert-LocalPlain $PSScriptRoot
    $taskPath = Join-Path $PSScriptRoot 'task.json'; Assert-LocalPlain $taskPath
    if ((Get-Item -LiteralPath $taskPath).Length -gt 64KB) { throw '绑定清单超限。' }
    $task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($task.schema_version -ne 4 -or $task.owner -cne 'XEN_AUTO_STOP_COUNTERPULSE' -or
        $task.run_directory -cne $PSScriptRoot -or $task.status -cne 'PREPARED_NOT_LAUNCHED') { throw '绑定清单无效。' }
    foreach ($name in @('executable', 'config', 'script')) {
        Assert-LocalPlain $task.$name
        if ((Get-FileHash -LiteralPath $task.$name -Algorithm SHA256).Hash -cne $task.($name + '_sha256')) { throw '绑定文件已变化，请重新Prepare。' }
    }
    Assert-LocalPlain $PSCommandPath
    $hashField = if ($action -eq 'record') { 'record_manual_sha256' } elseif ($action -eq 'analyze') { 'analyze_recording_sha256' } elseif ($action -eq 'defaults') { 'prepare_default_test_sha256' } else { 'show_hud_sha256' }
    if ((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash -cne $task.$hashField) { throw '录制或查看入口已变化。' }
    if ($action -eq 'record') {
        $settings = Join-Path $PSScriptRoot 'sampling-settings.json'; Assert-LocalPlain $settings
        if ((Get-Item -LiteralPath $settings).Length -gt 64KB) { throw '模型设置超限。' }
        $base = Join-Path $PSScriptRoot 'manual-recordings'; Assert-LocalPlain $base
        $output = Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
        Assert-LocalPlain $output
        if (Test-Path -LiteralPath $output) { throw '人工记录目录已存在。' }
        $arguments = @('--record-manual', '--config', $task.config, '--output', $output,
            '--sampling-settings', $settings, '--recording-duration-ms', '120000')
        # 原生入口只创建本轮目录；父目录由入口准备，旧轮次始终保留。
        $null = [IO.Directory]::CreateDirectory($base)
        Assert-LocalPlain $base
    } elseif ($action -eq 'defaults') {
        $base = Join-Path $PSScriptRoot 'default-baselines'; Assert-LocalPlain $base
        $null = [IO.Directory]::CreateDirectory($base); Assert-LocalPlain $base
        $output = Join-Path $base ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
        Assert-LocalPlain $output
        if (Test-Path -LiteralPath $output) { throw '本次默认推导目录已存在。' }
        $arguments = @('--derive-defaults', '--output', $output)
        if ($SamplingSettingsPath) {
            $settings = [IO.Path]::GetFullPath($SamplingSettingsPath); Assert-LocalPlain $settings
            if ((Get-Item -LiteralPath $settings).Length -gt 64KB) { throw '模型设置超限。' }
            $arguments += @('--sampling-settings', $settings)
        }
    } elseif ($action -eq 'analyze') {
        $base = Join-Path $PSScriptRoot 'manual-recordings'; Assert-LocalPlain $base
        $reports = New-Object 'Collections.Generic.List[string]'
        if ([IO.Directory]::Exists($base)) {
            $count = 0
            foreach ($directory in [IO.Directory]::EnumerateDirectories($base)) {
                if (++$count -gt 1000) { throw '人工记录目录超过分析预算。' }
                Assert-LocalPlain $directory
                $labelsPath = Join-Path $directory 'labels.json'; Assert-LocalPlain $labelsPath
                if ([IO.File]::Exists($labelsPath)) {
                    if ((Get-Item -LiteralPath $labelsPath).Length -gt 16KB) { throw '录制标签超过预算。' }
                    $labels = Get-Content -LiteralPath $labelsPath -Raw -Encoding UTF8 | ConvertFrom-Json
                    if ($labels.PSObject.Properties.Name -contains 'recording_usable') {
                        if ($labels.recording_usable -isnot [bool]) { throw '录制标签recording_usable必须为布尔值。' }
                        if (-not $labels.recording_usable) {
                            [Console]::WriteLine('已按用户标记排除录制，不用于参数推荐：' + (Split-Path -Leaf $directory))
                            continue
                        }
                    }
                }
                $candidate = Join-Path $directory 'sampling-analysis.json'; Assert-LocalPlain $candidate
                if (-not [IO.File]::Exists($candidate) -or (Get-Item -LiteralPath $candidate).Length -gt 64MB) { continue }
                try { $value = Get-Content -LiteralPath $candidate -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }
                if ($value.source -ceq 'KMBOX_MONITOR' -and $value.analysis_mode -ceq 'MANUAL_RECEIVE_INPUT_MODEL') { $reports.Add($candidate) }
            }
        }
        if (-not $reports.Count) { throw '没有有效人工采样报告；先完成一次人工记录，不使用自动测试result。' }
        $report = $reports | Sort-Object { [IO.File]::GetLastWriteTimeUtc($_) } -Descending | Select-Object -First 1
        $recordDirectory = Split-Path -Parent $report
        $settings = Join-Path $PSScriptRoot 'sampling-settings.json'; Assert-LocalPlain $settings
        if ((Get-Item -LiteralPath $settings).Length -gt 64KB) { throw '模型设置超限。' }
        $reviews = Join-Path $PSScriptRoot 'manual-reviews'; Assert-LocalPlain $reviews
        $null = [IO.Directory]::CreateDirectory($reviews); Assert-LocalPlain $reviews
        $output = Join-Path $reviews ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
        Assert-LocalPlain $output
        if (Test-Path -LiteralPath $output) { throw '本次重评目录已存在。' }
        $arguments = @('--evaluate-manual', $recordDirectory, '--output', $output, '--sampling-settings', $settings)
    } else {
        $reports = New-Object 'Collections.Generic.List[string]'
        $report = Join-Path $PSScriptRoot 'result/sampling-analysis.json'; Assert-LocalPlain $report
        if ([IO.File]::Exists($report)) { $reports.Add($report) }
        $count = 0
        foreach ($reportRoot in @('manual-recordings', 'manual-reviews')) {
            $base = Join-Path $PSScriptRoot $reportRoot; Assert-LocalPlain $base
            if ([IO.Directory]::Exists($base)) {
                foreach ($directory in [IO.Directory]::EnumerateDirectories($base)) {
                    if (++$count -gt 1000) { throw '人工记录及重评目录超过查看预算。' }
                    Assert-LocalPlain $directory
                    $candidate = Join-Path $directory 'sampling-analysis.json'; Assert-LocalPlain $candidate
                    if ([IO.File]::Exists($candidate)) { $reports.Add($candidate) }
                }
            }
        }
        if (-not $reports.Count) { throw '没有可查看的采样报告，请先完成一次人工记录或测试。' }
        $report = $reports | Sort-Object { [IO.File]::GetLastWriteTimeUtc($_) } -Descending | Select-Object -First 1
        if ((Get-Item -LiteralPath $report).Length -gt 64MB) { throw 'HUD报告超出预算。' }
        $arguments = @('--show-hud', $report)
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $task.executable; $info.UseShellExecute = $false
    $info.WorkingDirectory = Split-Path -Parent $task.executable
    $info.Arguments = (($arguments | ForEach-Object { if ($_.Contains('"')) { throw '参数含引号。' }; '"' + $_.TrimEnd('\') + '"' }) -join ' ')
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $child = [Diagnostics.Process]::Start($info)
    $stdout = $child.StandardOutput.ReadToEndAsync(); $stderr = $child.StandardError.ReadToEndAsync()
    # 人工录制由程序120秒上限停止采集；HUD可继续显示，用户关闭才退出，不进行超时kill。
    $child.WaitForExit(); $code = $child.ExitCode; $child.Dispose()
    if ($code -ne 0) { throw '录制、查看或离线分析未完成，请检查本次记录。' }
    if ($action -eq 'analyze') {
        [Console]::WriteLine('离线分析目录：' + $output)
        [Console]::WriteLine('图文报告：' + (Join-Path $output 'debug-report.html'))
        Start-Process -FilePath (Join-Path $output 'debug-report.html') | Out-Null
        $proposalsPath = Join-Path $output 'manual-plan-proposals.json'; Assert-LocalPlain $proposalsPath
        if ((Get-Item -LiteralPath $proposalsPath).Length -gt 64MB) { throw '候选计划超过预算。' }
        $proposals = Get-Content -LiteralPath $proposalsPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $groups = @($proposals.groups)
        if ($groups.Count -gt 100) { throw '候选分组超过预算。' }
        $candidates = New-Object 'Collections.Generic.List[object]'
        [Console]::WriteLine((@($proposals.reasons) -join '；'))
        foreach ($group in $groups) {
            if ($null -eq $group.candidate_plan) {
                [Console]::WriteLine('不支持自动动作：' + [string]$group.baseline + '；' + (@($group.reasons) -join '；'))
                [Console]::WriteLine((@($group.validation_errors) -join '；'))
            }
            $proposal = if ($null -ne $group.candidate_plan) { $group.candidate_plan } else { $group.proposed_plan }
            if ($null -ne $proposal) {
                $candidates.Add($proposal)
                [Console]::WriteLine(('候选 {0}：{1}，方向 {2}，样本 {3}' -f $candidates.Count, $group.baseline, $group.direction, $group.samples))
                if ($null -eq $group.candidate_plan) { [Console]::WriteLine('须人工修改后通过校验：' + (@($group.validation_errors) -join '；')) }
            }
        }
        if (-not $candidates.Count) { [Console]::WriteLine('原操作无法直接映射，均值已保留；仅保存分析，没有启动设备。'); exit 0 }
        $selection = Read-Host '输入候选编号生成可编辑测试目录，输入0仅保留分析'
        $choice = 0
        if (-not [int]::TryParse($selection, [ref]$choice) -or $choice -lt 0 -or $choice -gt $candidates.Count) { throw '候选编号无效；未生成或启动测试。' }
        if ($choice -eq 0) { exit 0 }
        $editable = Join-Path $output 'editable-plan.json'; Assert-LocalPlain $editable
        $candidates[$choice - 1] | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $editable -Encoding UTF8
        while ($true) {
            [Console]::WriteLine('可编辑计划：' + $editable)
            Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + $editable + '"') | Out-Null
            $answer = Read-Host '保存并关闭编辑器后按回车校验；输入0仅保留分析和编辑文件'
            if ($answer -eq '0') { exit 0 }
            Assert-LocalPlain $editable
            if ((Get-Item -LiteralPath $editable).Length -gt 16KB) { throw '编辑计划超过预算。' }
            & $task.executable --plan $editable --dry-run --require-current-plan
            if ($LASTEXITCODE -eq 0) { break }
            [Console]::WriteLine('计划未通过正式校验，原值保留；请再次编辑，不会生成或启动测试。')
        }
        $plan = Get-Content -LiteralPath $editable -Raw -Encoding UTF8 | ConvertFrom-Json
        $reviewAnalysis = Join-Path $output 'sampling-analysis.json'; Assert-LocalPlain $reviewAnalysis
        if ((Get-Item -LiteralPath $reviewAnalysis).Length -gt 64MB) { throw '重评报告超过预算。' }
        $usedSettings = (Get-Content -LiteralPath $reviewAnalysis -Raw -Encoding UTF8 | ConvertFrom-Json).settings
    } elseif ($action -eq 'defaults') {
        [Console]::WriteLine('默认基线推导目录：' + $output)
        [Console]::WriteLine('推导值与待校准假设：' + (Join-Path $output 'default-baseline.json'))
        $planPath = Join-Path $output 'plan.json'; Assert-LocalPlain $planPath
        $settingsPath = Join-Path $output 'sampling-settings.json'; Assert-LocalPlain $settingsPath
        if ((Get-Item -LiteralPath $planPath).Length -gt 16KB -or (Get-Item -LiteralPath $settingsPath).Length -gt 64KB) { throw '默认推导参数超过预算。' }
        & $task.executable --plan $planPath --dry-run --require-current-plan
        if ($LASTEXITCODE -ne 0) { throw '默认推导计划未通过正式校验。' }
        $plan = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $usedSettings = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    if ($action -in @('analyze', 'defaults')) {
        foreach ($field in @('schema_version','baseline','capture_enabled','shots','fire_delay_ms','fire_interval_ms','move_during_fire_delay','move_ms','counter_hold_ms','counter_delay_ms','shot_after_release_ms','shot_hold_ms','late_tolerance_ms','direction')) {
            if ($plan.PSObject.Properties.Name -notcontains $field) { throw '候选计划字段缺失。' }
        }
        if ($plan.schema_version -ne 2 -or $plan.capture_enabled -ne $false -or $plan.move_during_fire_delay -isnot [bool] -or $plan.direction -notin @(2,8)) { throw '候选计划契约无效。' }
        $overlap = $false
        if ($plan.PSObject.Properties.Name -contains 'overlap_fire_interval') {
            if ($plan.overlap_fire_interval -isnot [bool]) { throw '候选动态调度开关类型无效。' }
            $overlap = $plan.overlap_fire_interval
        }
        $testDirectory = Join-Path $output 'test'; Assert-LocalPlain $testDirectory
        $restartInterval = 0
        if ($plan.PSObject.Properties.Name -contains 'restart_interval_ms') {
            if ($plan.restart_interval_ms -isnot [int] -and $plan.restart_interval_ms -isnot [long]) { throw '候选重新起步间隔类型无效。' }
            $restartInterval = $plan.restart_interval_ms
        }
        if ($null -eq $usedSettings) { throw '重评报告缺少实际模型设置；未生成测试。' }
        $prepare = @{ Mode='Prepare'; RunDirectory=$testDirectory; Repeatable=$true; Executable=$task.executable; ConfigPath=$task.config;
            Baseline=$plan.baseline; Shots=$plan.shots; FireDelayMs=$plan.fire_delay_ms; FireIntervalMs=$plan.fire_interval_ms;
            RestartIntervalMs=$restartInterval;
            OverlapFireInterval=$overlap; MoveDuringFireDelay=$plan.move_during_fire_delay; Direction=$(if ($plan.direction -eq 2) { 'A' } else { 'D' });
            MoveMs=$plan.move_ms; CounterHoldMs=$plan.counter_hold_ms; CounterDelayMs=$plan.counter_delay_ms;
            ShotAfterReleaseMs=$plan.shot_after_release_ms; ShotHoldMs=$plan.shot_hold_ms; LateToleranceMs=$plan.late_tolerance_ms;
            Scope=$originalScope }
        if ($originalCredentialDirectory) { $prepare.CredentialDirectory = $originalCredentialDirectory }
        & $task.script @prepare
        if (-not $?) { throw '候选Prepare失败；未启动设备。' }
        $usedSettings | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $testDirectory 'sampling-settings.json') -Encoding UTF8
        [Console]::WriteLine('编辑动作参数：' + (Join-Path $testDirectory 'edit-config.bat'))
        [Console]::WriteLine('编辑模型参数：' + (Join-Path $testDirectory 'edit-sampling.bat'))
        [Console]::WriteLine('仅用户前台启动：' + (Join-Path $testDirectory 'start-test.bat'))
    }
    exit 0
} catch {
    if ($action -in @('analyze', 'defaults')) { [Console]::Error.WriteLine('离线分析或测试准备未完成：' + $_.Exception.Message) }
    else { [Console]::Error.WriteLine('人工录制或HUD查看未启动/未完成；检查前台会话、绑定文件和报告是否存在。') }
    exit 1
}
'@
        foreach ($entry in @(@('record-manual.ps1', 'record', 'start-recording.bat'), @('show-hud.ps1', 'show', 'show-hud.bat'), @('analyze-recording.ps1', 'analyze', 'analyze-recording.bat'), @('prepare-default-test.ps1', 'defaults', 'prepare-default-test.bat'))) {
            $body = '$action = ' + (Quote-PS $entry[1]) + "`r`n" + $manualCommon
            if ($entry[1] -in @('analyze', 'defaults')) {
                $credentialPath = if ($CredentialDirectory) { [IO.Path]::GetFullPath($CredentialDirectory) } else { '' }
                $body = '$originalCredentialDirectory = ' + (Quote-PS $credentialPath) + "`r`n" +
                    '$originalScope = ' + (Quote-PS $Scope) + "`r`n" + $body
            }
            if ($entry[1] -eq 'defaults') { $body = 'param([string]$SamplingSettingsPath)' + "`r`n" + $body }
            [IO.File]::WriteAllText((Join-Path $runPath $entry[0]), $body, (New-Object Text.UTF8Encoding($true)))
            [IO.File]::WriteAllText((Join-Path $runPath $entry[2]), $startBat.Replace('launch-test.ps1', $entry[0]), [Text.Encoding]::ASCII)
        }
        $task.record_manual_sha256 = Get-Digest (Join-Path $runPath 'record-manual.ps1')
        $task.show_hud_sha256 = Get-Digest (Join-Path $runPath 'show-hud.ps1')
        $task.analyze_recording_sha256 = Get-Digest (Join-Path $runPath 'analyze-recording.ps1')
        $task.prepare_default_test_sha256 = Get-Digest (Join-Path $runPath 'prepare-default-test.ps1')
        Write-Json $taskPath $task
        # 查看入口只在用户手动调用时打开已存在的报告；失败Run也可保留诊断报告。
        $reportScript = @'
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$reportPath = Join-Path $PSScriptRoot 'result/debug-report.html'
if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
    [Console]::Error.WriteLine('报告尚未生成，请先启动测试；失败时请检查result中的诊断记录。')
    exit 1
}
if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) {
    [Console]::Error.WriteLine('请在本机前台双击open-report.bat查看报告，远程会话不自动打开浏览器。')
    exit 1
}
try {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $reportPath
    $info.UseShellExecute = $true
    $null = [Diagnostics.Process]::Start($info)
    exit 0
} catch {
    [Console]::Error.WriteLine('无法打开报告，请在result目录手动打开debug-report.html。')
    exit 1
}
'@
        [IO.File]::WriteAllText((Join-Path $runPath 'open-report.ps1'), $reportScript, (New-Object Text.UTF8Encoding($true)))
        $reportBat = '@echo off' + "`r`n" + 'setlocal DisableDelayedExpansion' + "`r`n" +
            '"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0open-report.ps1"' + "`r`n" +
            'set "reportExitCode=%errorlevel%"' + "`r`n" + 'pause' + "`r`n" + 'exit /b %reportExitCode%' + "`r`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'open-report.bat'), $reportBat, [Text.Encoding]::ASCII)
        $parameterLines = New-Object 'Collections.Generic.List[string]'
        $parameterLines.Add('# 参数说明')
        $parameterLines.Add('')
        $parameterLines.Add('人工练习：start-recording.bat仅监听KMBOX人工输入，不发送动作；每次在manual-recordings创建独立记录，最多采集120秒，HUD保持至用户关闭。show-hud.bat从result、manual-recordings及manual-reviews重开修改时间最新的采样报告，显示该报告模型参数；隐藏或关闭查看器不触发真实输入。')
        $parameterLines.Add('素材转动作：用户前台打开analyze-recording.bat，仅从manual-recordings选择最新有效人工报告，按当前sampling-settings.json离线重评到独立manual-reviews目录，保留原始录制。查看间隔和不支持原因后，选择候选编辑editable-plan.json；超界和负值保留供人工修改，不自动截断。保存后正式程序dry-run校验，通过才Prepare本次review/test，输入0仅分析。新测试沿用本次重评实际模型设置和原CredentialDirectory/Scope。显示edit-config.bat/start-test.bat完整路径供用户前台启动，绝不自动Launch，也不复制程序包。')
        $parameterLines.Add('录制排除：人工记录labels.json中的recording_usable=false表示不用于参数推荐，分析入口会跳过并提示；缺少此字段沿用可用默认值。原始记录和诊断报告保留，show-hud仍可查看。')
        $parameterLines.Add('默认基线：前台打开prepare-default-test.bat，用参考seed推导模型和动作参数，不读取人工录制或当前模型覆盖。default-baselines/独立ID下的default-baseline.json区分推导值与待校准假设；test子目录提供edit-config.bat、edit-sampling.bat和start-test.bat，只准备不启动。需要按自行编辑模型重新推导时，在PowerShell前台调用prepare-default-test.ps1 -SamplingSettingsPath "模型JSONC绝对路径"；每次创建独立目录，保留原方案。')
        $parameterLines.Add('三步调试：双击edit-config.bat编辑plan.json并保存；由用户前台双击start-test.bat启动；结束后双击open-report.bat查看报告。启动会发送真实移动与开火输入。')
        $parameterLines.Add('Repeatable模式每次启动冻结本轮计划，编辑只影响下一轮；成功校验后覆盖上一组result。正式比较请另存证据。')
        $parameterLines.Add('报告是基于ACK回执与输入模型的采样分析，不是游戏速度、实际子弹或命中率测量；失败Run已生成的报告仍可查看。')
        $parameterLines.Add('三组参数分开：plan.json控制真实动作；sampling-settings.json经edit-sampling.bat调整模型和HUD；报告采样结果只用于核对模型，不能作为游戏测量。auto_fire_interval_ms是模型采样间隔，fire_interval_ms是开火命令最小间隔，shot_hold_ms是实际按住时长。模型设置只在文件不存在时创建，重复Prepare保留用户校准值；每轮冻结后生效。')
        $parameterLines.Add('以下数值是本次Prepare实际采用值；之后手工编辑以plan.json为准。')
        $parameterLines.Add('')
        $parameterLines.Add('| 字段 | 本次Prepare实际值 | 含义 |')
        $parameterLines.Add('|---|---|---|')
        foreach ($key in $plan.Keys) {
            $parameterLines.Add('| ' + $key + ' | ' + (ConvertTo-Json -InputObject $plan[$key] -Compress) + ' | ' + $planHelp[$key] + ' |')
        }
        [IO.File]::WriteAllText((Join-Path $runPath 'PARAMETERS.md'), ($parameterLines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($false)))
        $action = if ($Baseline -eq 'no_counter') { '仅松键，不按反向键' } else { "反向轻点$($CounterHoldMs)ms（ACK计时）" }
        $timing = if ($ShotAfterReleaseMs -eq 0) { '最后方向键UP ACK后立即计划开枪' } else { "最后方向键UP ACK后$($ShotAfterReleaseMs)ms计划开枪" }
        $behavior = if ($Baseline -eq 'stationary') {
            "原地静止基线，不发送A/D移动；上一轮左键UP ACK后等待$($FireDelayMs)ms，再计划开火。"
        } elseif ($OverlapFireInterval) {
            "首轮原地；后续在武器间隔内按${Direction}，DOWN ACK起最多计划保持$($MoveMs)ms，按剩余窗口缩短；收到UP ACK后等待$($CounterDelayMs)ms，再$action；$timing，且不早于上一轮DOWN提交加武器间隔。"
        } elseif ($MoveDuringFireDelay) {
            "首轮原地；上一轮左键UP ACK后开始$($FireDelayMs)ms间隔，立刻按$Direction，方向键从DOWN ACK起至少保持$($MoveMs)ms。间隔与保持时间都满足后才松键，即等待两者结束时刻的较晚者；收到该键UP ACK后等待$($CounterDelayMs)ms，再$action；$timing。"
        } else {
            "首轮原地；上一轮左键UP ACK后静止等待$($FireDelayMs)ms，然后按${Direction}保持$($MoveMs)ms；收到该键UP ACK后等待$($CounterDelayMs)ms，再$action；$timing。"
        }
        if ($RestartIntervalMs -gt 0) { $behavior += "反向UP ACK到下一正向DOWN提交至少$($RestartIntervalMs)ms，与左键释放和既有目标取较晚时刻；无前次反向UP时不限制。" }
        $behavior += "各动作迟到超过$($LateToleranceMs)ms则拒绝该组；固定瞄准，不人为按方向或射击键。"
        $observation = '默认不采集图像；自动保存输入训练原始报告和评价。接收域换键评分不代表人物停稳，命令ACK不代表实际开火或弹着稳定'
        $cadence = "每次左键按住$($ShotHoldMs)ms（DOWN ACK起计）；fire_interval_ms=$($FireIntervalMs)ms是相邻左键DOWN提交的最小间隔，不是精确周期。额外冷却等待在下一轮移动开始前完成，避免急停后再补等；0表示不添加此间隔。fire_delay_ms仍表示上一轮松左键ACK后的等待"
        if ($OverlapFireInterval) { $cadence = "每次左键按住$($ShotHoldMs)ms（DOWN ACK起计）；动态移动占用fire_interval_ms=$($FireIntervalMs)ms的剩余窗口，move_ms=$($MoveMs)ms为上限；完整保留反向和松键后等待，相邻DOWN提交仍不早于武器间隔，不是精确周期。" }
        $reuseInstructions = if ($Repeatable) {
            '本目录允许重复手动Launch，无需再次Prepare或打包。直接复用绑定的正式脚本和Executable路径，不复制程序、DLL或模型。编辑plan.json：shots（开火次数1..30，不代表实际子弹数）、fire_delay_ms（0..2000ms）、fire_interval_ms（0..5000ms，相邻DOWN提交最小间隔；0为关闭）、restart_interval_ms（0..2000ms，反向UP ACK到下一正向DOWN提交最小间隔，0保持原行为；非counter必须0）、move_during_fire_delay（true为等待与移动并行，false为先等待再移动）、overlap_fire_interval（缺省false；true在武器间隔内动态移动）、move_ms（1..500ms，动态模式为上限）、counter_delay_ms（0..200ms）、counter_hold_ms（1..200ms）、shot_after_release_ms（0..20ms，0即立即计划开枪）、shot_hold_ms（1..2000ms，1000即按住1秒）、late_tolerance_ms（0..10ms）及baseline/direction。schema_version固定2，capture_enabled固定false，不接受旧shot_interval_ms/brake_window_ms。每次Launch冻结execution-plan.json，运行中编辑原文件仅影响下一组。验证通过后覆盖上组result；失败不自动重试。正式对比另建目录。'
        } else {
            '参数探索可通过Prepare -ReuseRunDirectory复用本目录；新计划验证通过后替换参数并清理上次result和CONSUMED。每次Prepare后仍由用户前台触发Launch。固定正式文件不复制打包；需要重复调参时Prepare -Repeatable。'
        }
        $manualMode = if ($Repeatable) { '仅用户在当前前台每次手动执行一组，可重复启动' } else { '仅用户在当前前台执行一次' }
        $markdown = "# 反向轻点人工Run`n`n状态：PREPARED_NOT_LAUNCHED。$manualMode；会发送真实开火输入。`n`n$behavior`n`n请先确认测试场景、源焦点和独占设备；End或人工输入取消。`n`n``````powershell`n$launch`n```````n`n一组$Shots 次开火；$observation。$cadence；结果不代表已经稳定。结果目录：result。`n`n$reuseInstructions`n`n三步调试：edit-config.bat编辑并保存参数 → start-test.bat前台启动 → open-report.bat查看报告。报告为ACK输入模型，不是游戏测量；失败Run的已生成报告仍可查看。模型参数和HUD开关由edit-sampling.bat编辑sampling-settings.json，每轮启动冻结生效。字段说明见PARAMETERS.md。`n`n迁移旧目录后仅使用本TASK中的新入口。`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'TASK.md'), $markdown, (New-Object Text.UTF8Encoding($false)))
        Write-Output 'PREPARED_NOT_LAUNCHED；未发送设备输入。'
    } else {
        if (-not $AllowPhysicalOutput -or $Confirm -cne 'AUTO_STOP_COUNTERPULSE') { throw '缺少本轮物理输出授权参数。' }
        if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) { throw 'Launch仅允许用户本机交互会话。' }
        $lockPath = Join-Path $runPath '.counterpulse.lock'
        Assert-PlainPath $lockPath
        $runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($task.schema_version -ne 4) { $script:FailureCode = 'LEGACY_RUN_REQUIRES_PREPARE'; throw '旧Run须重新Prepare，未触发设备。' }
        if ($task.schema_version -ne 4 -or $task.status -ne 'PREPARED_NOT_LAUNCHED' -or
            $task.plan -cne $planPath -or $task.script -cne $scriptPath) { throw 'Run绑定无效。' }
        if ($task.schema_version -ge 2 -and ($task.owner -cne 'XEN_AUTO_STOP_COUNTERPULSE' -or $task.run_directory -cne $runPath)) { throw 'Run所有权无效。' }
        if ($task.repeatable -isnot [bool]) { throw '重复运行标记须为布尔值。' }
        $isRepeatable = [bool]$task.repeatable
        if ($isRepeatable -and $task.repeatable -ne $true) { throw '重复运行绑定无效。' }
        foreach ($name in @('executable', 'config', 'plan', 'script')) {
            Assert-PlainPath $task.$name
            if ($isRepeatable -and $name -eq 'plan') { continue }
            if ((Get-Digest $task.$name) -cne $task.($name + '_sha256')) { throw 'Run绑定文件已变化，必须新建Run。' }
        }
        $output = Join-Path $runPath 'result'
        $executionPlan = $planPath
        $executionSettings = $samplingPath
        Assert-PlainPath $samplingPath
        if ($isRepeatable) {
            Assert-ProbeStopped $task.executable
            $executionPlan = Join-Path $runPath 'execution-plan.json'
            Assert-PlainPath $executionPlan
            Assert-PlainPath (Join-Path $runPath 'CONSUMED')
            Assert-ResultTree $output
            # 原计划只读取一次；dry-run 与本轮 Worker 使用同一份冻结内容。
            try {
                $planBytes = Read-BoundedBytes $planPath
                $settingsBytes = Read-BoundedBytes $samplingPath
                $candidateSettings = Join-Path $runPath ('sampling.' + [Guid]::NewGuid().ToString('N') + '.candidate.json')
                [IO.File]::WriteAllBytes($candidateSettings, $settingsBytes)
                $candidatePlan = Join-Path $runPath ('plan.' + [Guid]::NewGuid().ToString('N') + '.candidate.json')
                [IO.File]::WriteAllBytes($candidatePlan, $planBytes)
                Invoke-Probe $task.executable @('--plan', $candidatePlan, '--dry-run', '--require-current-plan', '--sampling-settings', $candidateSettings) $false
            } catch {
                $script:FailureCode = 'PLAN_VALIDATION_FAILED'
                throw '本轮计划校验失败，保留上次结果。'
            }
            $executionSettings = Join-Path $runPath 'execution-sampling-settings.json'
            Assert-PlainPath $executionSettings
            Move-Item -LiteralPath $candidateSettings -Destination $executionSettings -Force
            $candidateSettings = $null
            Move-Item -LiteralPath $candidatePlan -Destination $executionPlan -Force
            $candidatePlan = $null
            if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
            $consumed = Join-Path $runPath 'CONSUMED'
            if (Test-Path -LiteralPath $consumed) { Remove-Item -LiteralPath $consumed -Force }
        } elseif (Test-Path -LiteralPath $output) { throw '结果目录已存在，拒绝重复执行。' }
        # CreateNew原子抢占；失败或取消也不自动重试此Run。
        $marker = [IO.File]::Open((Join-Path $runPath 'CONSUMED'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $marker.Dispose()
        [Console]::WriteLine('请将游戏切到前台并松开移动键和鼠标按钮；等待首个键态报告时可单独轻按松开Shift，不按方向键或鼠标；人物须事先静止并固定瞄准。下方实时显示就绪阶段。')
        try {
            Invoke-Probe $task.executable @('--config', $task.config, '--plan', $executionPlan, '--output', $output,
                '--allow-physical-output', '--confirm', 'AUTO_STOP_COUNTERPULSE', '--require-current-plan', '--sampling-settings', $executionSettings) $true
        } finally {
            try { Invoke-ReportHud $task.executable (Join-Path $output 'sampling-analysis.json') }
            catch { [Console]::WriteLine('采样HUD未打开，可由show-hud.bat重新查看已有报告。') }
        }
        Write-Output '本次有界Run结束；请回收result及人工观察，不能自动认定停稳。'
    }
} catch {
    # 不输出异常对象或路径内容，尤其不泄漏解密或配置解析异常。
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    [Console]::Error.WriteLine('[COUNTERPULSE_FAILED] ' + $script:FailureCode + '：未自动重试，请查看上方阶段及本次报告。')
    exit 1
} finally {
    if ($candidateSettings -and [IO.File]::Exists($candidateSettings)) { Remove-Item -LiteralPath $candidateSettings -Force }
    if ($candidatePlan -and [IO.File]::Exists($candidatePlan)) { Remove-Item -LiteralPath $candidatePlan -Force }
    if ($null -ne $runLock) { $runLock.Dispose() }
}
