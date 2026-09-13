[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare', 'Launch')][string]$Mode,
    [Parameter(Mandatory)][string]$RunDirectory,
    [switch]$ReuseRunDirectory,
    [switch]$Repeatable,
    [switch]$NoCapture,
    [string]$Executable,
    [string]$ConfigPath,
    [ValidateSet('stationary', 'no_counter', 'counter')][string]$Baseline = 'counter',
    [ValidateRange(7, 20)][int]$Shots = 8,
    [ValidateScript({ $_ -eq 0 -or ($_ -ge 280 -and $_ -le 650) })][int]$ShotIntervalMs = 280,
    [ValidateSet('A', 'D')][string]$Direction = 'A',
    [ValidateRange(1, 500)][int]$MoveMs = 120,
    [ValidateRange(1, 200)][int]$CounterHoldMs = 30,
    [ValidateRange(1, 200)][int]$BrakeWindowMs = 60,
    [ValidateRange(0, 20)][int]$ShotAfterReleaseMs = 0,
    [ValidateRange(1, 20)][int]$ShotHoldMs = 5,
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
    if ($Existing.schema_version -notin @(1, 2, 3) -or $Existing.status -ne 'PREPARED_NOT_LAUNCHED' -or
        $Existing.plan -cne $planPath -or $Existing.run_id -cne [IO.Path]::GetFileName($runPath)) { throw '不是本工具绑定目录。' }
    if ($Existing.schema_version -ge 2 -and ($Existing.owner -cne 'XEN_AUTO_STOP_COUNTERPULSE' -or
        $Existing.run_directory -cne $runPath)) { throw '目录所有权无效。' }
    if ($Existing.schema_version -eq 3 -and $Existing.repeatable -ne $true) { throw '重复运行绑定无效。' }
    if ([IO.Path]::GetFileName($Existing.script) -notmatch '^invoke_auto_stop_counterpulse(-r[0-9]+)?\.ps1$') { throw '原入口身份无效。' }
    foreach ($name in @('executable', 'config', 'plan', 'script')) {
        Assert-PlainPath $Existing.$name
        if ($name -eq 'plan' -and $Existing.schema_version -eq 3) { continue }
        if ((Get-Digest $Existing.$name) -cne $Existing.($name + '_sha256')) { throw '原目录绑定已变化。' }
    }
    # 旧 schema 的入口没有共享锁；迁移前还须排除仍运行的绑定探针。
    Assert-ProbeStopped $Existing.executable
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
try {
    $runPath = [IO.Path]::GetFullPath($RunDirectory)
    $scriptPath = [IO.Path]::GetFullPath($PSCommandPath)
    $planPath = Join-Path $runPath 'plan.json'
    $taskPath = Join-Path $runPath 'task.json'
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
        $plan = [ordered]@{ capture_enabled = [bool](-not $NoCapture); baseline = $Baseline; shots = $Shots; shot_interval_ms = $ShotIntervalMs;
            move_ms = $MoveMs; counter_hold_ms = $CounterHoldMs; brake_window_ms = $BrakeWindowMs; shot_after_release_ms = $ShotAfterReleaseMs; shot_hold_ms = $ShotHoldMs;
            late_tolerance_ms = $LateToleranceMs; direction = $(if ($Direction -eq 'A') { 2 } else { 8 }) }
        if (-not $exists) { $null = New-Item -ItemType Directory -Path $runPath }
        $lockPath = Join-Path $runPath '.counterpulse.lock'
        Assert-PlainPath $lockPath
        $runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        foreach ($leaf in @('task.json', 'plan.json', 'TASK.md', 'CONSUMED')) { Assert-PlainPath (Join-Path $runPath $leaf) }
        if ($exists) { Assert-OwnedRun (Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json) }
        $resultPath = Join-Path $runPath 'result'
        Assert-ResultTree $resultPath
        $candidatePlan = Join-Path $runPath ('plan.' + [Guid]::NewGuid().ToString('N') + '.candidate.json')
        Write-Json $candidatePlan $plan
        Invoke-Probe $binary @('--plan', $candidatePlan, '--dry-run') $false
        # 所有验证通过之后才移除上组结果，并重新武装用户前台的一次性命令。
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Recurse -Force }
        $consumed = Join-Path $runPath 'CONSUMED'
        if (Test-Path -LiteralPath $consumed) { Remove-Item -LiteralPath $consumed -Force }
        Move-Item -LiteralPath $candidatePlan -Destination $planPath -Force
        $candidatePlan = $null
        $task = [ordered]@{ schema_version = $(if ($Repeatable) { 3 } else { 2 }); owner = 'XEN_AUTO_STOP_COUNTERPULSE'; run_directory = $runPath; status = 'PREPARED_NOT_LAUNCHED'; run_id = [IO.Path]::GetFileName($runPath);
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
        $behavior = if ($Baseline -eq 'stationary') { '原地静止基线，不发送A/D移动；人物须事先静止并固定瞄准。' } else { '移动与急停测试；会发送A/D移动。' }
        if ($Baseline -ne 'stationary') {
            $action = if ($Baseline -eq 'no_counter') { '仅松键，不按反向键' } else { "反向轻点$($CounterHoldMs)ms（ACK计时）" }
            $timing = if ($ShotAfterReleaseMs -gt 0) { "最后方向键UP ACK后$($ShotAfterReleaseMs)ms计划开枪" } else { "移动UP ACK后$($BrakeWindowMs)ms计划开枪" }
            $recovery = if ($ShotIntervalMs -eq 0) { "按动作完成接续，不设最小枪间隔" } else { "枪间至少$($ShotIntervalMs)ms，ACK耗时计入实际枪间隔" }
            $behavior = "首枪原地，随后每次按$Direction 移动$($MoveMs)ms，$action；$timing。$recovery；松键后开枪迟到超过$($LateToleranceMs)ms则拒绝该组。固定瞄准，不人为按方向或射击键。"
        }
        $cadence = if ($ShotIntervalMs -eq 0) { "动作完成后接续下一次移动；实际枪间隔由移动、反向轻点、松键后等待和命令耗时决定" } else { "最小射击间隔$($ShotIntervalMs)ms；该间隔仅为候选" }
        $observation = if ($NoCapture) { '不采集图像，以人工观察判断；长组前面的弹着点可能消失，请连续观察' } else { '长组前面的弹着点可能消失，请连续观察，图像逐帧保存' }
        $reuseInstructions = if ($Repeatable) {
            "本目录允许重复手动Launch，无需再次Prepare。编辑plan.json中的move_ms（正向键保持，1..500ms）、counter_hold_ms（反向键保持，1..200ms）和shot_after_release_ms（最后方向键UP ACK后等待，当前模式使用1..20ms）。本次准备值分别为$MoveMs/$CounterHoldMs/$ShotAfterReleaseMs；执行以本次读取并验证的plan.json为准。每次Launch冻结execution-plan.json，运行中编辑原plan不改变本轮。新计划验证通过后会覆盖上次result；失败不自动重试。每次仅用户手动触发一组，开始正式对比时另建目录。"
        } else {
            '参数探索可通过Prepare -ReuseRunDirectory复用本目录；验证新计划后替换参数并清理上次result和CONSUMED。每次Prepare后仍须用户手动运行本TASK中的同一Launch命令，不会自动重试。开始正式对比时另建目录。'
        }
        $manualMode = if ($Repeatable) { '仅用户在当前前台每次手动执行一组，可重复启动' } else { '仅用户在当前前台执行一次' }
        $markdown = "# 反向轻点人工Run`n`n状态：PREPARED_NOT_LAUNCHED。$manualMode；会发送真实开火输入。`n`n$behavior`n`n请先确认测试场景、源焦点、独占设备和紫色弹着点显示；End或人工输入取消。`n`n``````powershell`n$launch`n```````n`n一组$Shots 发；$observation。$cadence；结果不代表已经稳定。结果目录：result。`n`n$reuseInstructions`n`n迁移旧目录后仅使用本TASK中的新入口。`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'TASK.md'), $markdown, (New-Object Text.UTF8Encoding($false)))
        Write-Output 'PREPARED_NOT_LAUNCHED；未发送设备输入。'
    } else {
        if (-not $AllowPhysicalOutput -or $Confirm -cne 'AUTO_STOP_COUNTERPULSE') { throw '缺少本轮物理输出授权参数。' }
        if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) { throw 'Launch仅允许用户本机交互会话。' }
        $lockPath = Join-Path $runPath '.counterpulse.lock'
        Assert-PlainPath $lockPath
        $runLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($task.schema_version -notin @(1, 2, 3) -or $task.status -ne 'PREPARED_NOT_LAUNCHED' -or
            $task.plan -cne $planPath -or $task.script -cne $scriptPath) { throw 'Run绑定无效。' }
        if ($task.schema_version -ge 2 -and ($task.owner -cne 'XEN_AUTO_STOP_COUNTERPULSE' -or $task.run_directory -cne $runPath)) { throw 'Run所有权无效。' }
        $isRepeatable = $task.schema_version -eq 3
        if ($isRepeatable -and $task.repeatable -ne $true) { throw '重复运行绑定无效。' }
        foreach ($name in @('executable', 'config', 'plan', 'script')) {
            Assert-PlainPath $task.$name
            if ($isRepeatable -and $name -eq 'plan') { continue }
            if ((Get-Digest $task.$name) -cne $task.($name + '_sha256')) { throw 'Run绑定文件已变化，必须新建Run。' }
        }
        $output = Join-Path $runPath 'result'
        $executionPlan = $planPath
        if ($isRepeatable) {
            Assert-ProbeStopped $task.executable
            $executionPlan = Join-Path $runPath 'execution-plan.json'
            Assert-PlainPath $executionPlan
            Assert-PlainPath (Join-Path $runPath 'CONSUMED')
            Assert-ResultTree $output
            # 原计划只读取一次；dry-run 与本轮 Worker 使用同一份冻结内容。
            try {
                $reader = [IO.File]::Open($planPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
                try {
                    if ($reader.Length -gt 16384) { throw '计划超过大小限制。' }
                    $planBytes = New-Object byte[] ([int]$reader.Length)
                    $offset = 0
                    while ($offset -lt $planBytes.Length) {
                        $count = $reader.Read($planBytes, $offset, $planBytes.Length - $offset)
                        if ($count -le 0) { throw '计划读取不完整。' }
                        $offset += $count
                    }
                } finally { $reader.Dispose() }
                $candidatePlan = Join-Path $runPath ('plan.' + [Guid]::NewGuid().ToString('N') + '.candidate.json')
                [IO.File]::WriteAllBytes($candidatePlan, $planBytes)
                Invoke-Probe $task.executable @('--plan', $candidatePlan, '--dry-run') $false
            } catch {
                $script:FailureCode = 'PLAN_VALIDATION_FAILED'
                throw '本轮计划校验失败，保留上次结果。'
            }
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
        Invoke-Probe $task.executable @('--config', $task.config, '--plan', $executionPlan, '--output', $output,
            '--allow-physical-output', '--confirm', 'AUTO_STOP_COUNTERPULSE') $true
        Write-Output '本次有界Run结束；请回收result及人工观察，不能自动认定停稳。'
    }
} catch {
    # 不输出异常对象或路径内容，尤其不泄漏解密或配置解析异常。
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    [Console]::Error.WriteLine('[COUNTERPULSE_FAILED] ' + $script:FailureCode + '：未自动重试，请查看上方阶段及本次报告。')
    exit 1
} finally {
    if ($candidatePlan -and [IO.File]::Exists($candidatePlan)) { Remove-Item -LiteralPath $candidatePlan -Force }
    if ($null -ne $runLock) { $runLock.Dispose() }
}
