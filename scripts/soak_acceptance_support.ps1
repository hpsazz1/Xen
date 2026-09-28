# 只定义无设备合同函数；加载模块不会启动、终止或武装任何进程。
function Get-XenSoakPlanHash($Task) {
    $text = '{0}|{1}|{2}|{3}|{4}|{5}|{6}' -f $Task.profile,
        $Task.runtime_duration_seconds, $Task.resource_interval_milliseconds,
        $Task.startup_wait_budget_seconds, $Task.close_wait_budget_seconds,
        $Task.resource_max_duration_seconds, $Task.duration_basis
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Assert-XenSoakTask($Task) {
    if ($Task.profile -cne 'Soak' -or $Task.runtime_duration_seconds -lt 1 -or
        $Task.runtime_duration_seconds -gt 82800 -or $Task.resource_interval_milliseconds -lt 100 -or
        $Task.resource_interval_milliseconds -gt 60000 -or $Task.startup_wait_budget_seconds -lt 1 -or
        $Task.startup_wait_budget_seconds -gt 1800 -or $Task.close_wait_budget_seconds -lt 1 -or
        $Task.close_wait_budget_seconds -gt 1800 -or
        $Task.resource_max_duration_seconds -ne ($Task.runtime_duration_seconds +
            $Task.startup_wait_budget_seconds + $Task.close_wait_budget_seconds) -or
        $Task.plan_sha256 -cne (Get-XenSoakPlanHash $Task)) { throw 'Soak冻结计划无效或已变化，请重新Prepare。' }
}

function Get-XenProcessIdentity([Diagnostics.Process]$Process) {
    $Process.Refresh()
    if ($Process.HasExited) { throw '身份读取期间进程已退出。' }
    $started = $Process.StartTime.ToUniversalTime()
    $path = [IO.Path]::GetFullPath($Process.MainModule.FileName)
    $session = $Process.SessionId
    $Process.Refresh()
    if ($Process.HasExited -or $Process.StartTime.ToUniversalTime().Ticks -ne $started.Ticks -or
        -not [string]::Equals([IO.Path]::GetFullPath($Process.MainModule.FileName), $path,
            [StringComparison]::OrdinalIgnoreCase)) { throw '读取期间进程身份变化。' }
    [pscustomobject]@{
        process_id = $Process.Id; executable_path = $path
        start_time_utc = $started.ToString('o'); start_time_utc_ticks = $started.Ticks.ToString()
        session_id = $session
    }
}

function Test-XenProcessIdentityEqual($Expected, $Actual) {
    return [int]$Expected.process_id -eq [int]$Actual.process_id -and
        [string]::Equals([string]$Expected.executable_path, [string]$Actual.executable_path,
            [StringComparison]::OrdinalIgnoreCase) -and
        [string]$Expected.start_time_utc_ticks -ceq [string]$Actual.start_time_utc_ticks -and
        [int]$Expected.session_id -eq [int]$Actual.session_id
}

function Get-XenBoundProcessState($Identity) {
    $process = $null
    try {
        try { $process = [Diagnostics.Process]::GetProcessById([int]$Identity.process_id) }
        catch [ArgumentException] { return 'exited' }
        if ($process.HasExited) { return 'exited' }
        try { $actual = Get-XenProcessIdentity $process }
        catch { if ($process.HasExited) { return 'exited' }; throw }
        if (Test-XenProcessIdentityEqual $Identity $actual) { return 'running' }
        return 'identity_mismatch'
    } finally { if ($null -ne $process) { $process.Dispose() } }
}

function Select-XenSoakWorker($LauncherIdentity, [string]$WorkerPath, [object[]]$Candidates) {
    $expected = [IO.Path]::GetFullPath($WorkerPath)
    $matches = @($Candidates | Where-Object {
        [int]$_.ParentProcessId -eq [int]$LauncherIdentity.process_id -and
        $_.ExecutablePath -and [string]::Equals([IO.Path]::GetFullPath($_.ExecutablePath),
            $expected, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matches.Count -gt 1) { throw '同一Launcher有多个目标Worker，拒绝模糊附加。' }
    if ($matches.Count -eq 0) { return $null }
    $candidate = $matches[0]
    if ([int]$candidate.SessionId -ne [int]$LauncherIdentity.session_id -or
        $candidate.CreationDate.ToUniversalTime() -lt [datetime]::Parse($LauncherIdentity.start_time_utc).ToUniversalTime()) {
        throw 'Worker会话或父子创建顺序不符。'
    }
    return $candidate
}

function Find-XenSoakWorker($LauncherIdentity, [string]$WorkerPath) {
    if ((Get-XenBoundProcessState $LauncherIdentity) -cne 'running') { throw 'Launcher身份不再有效。' }
    $candidates = @(Get-CimInstance Win32_Process -Filter (
        'ParentProcessId={0}' -f [int]$LauncherIdentity.process_id) -ErrorAction Stop)
    $candidate = Select-XenSoakWorker $LauncherIdentity $WorkerPath $candidates
    if ($null -eq $candidate) { return $null }
    $process = [Diagnostics.Process]::GetProcessById([int]$candidate.ProcessId)
    try { $identity = Get-XenProcessIdentity $process } finally { $process.Dispose() }
    if ($identity.executable_path -ine [IO.Path]::GetFullPath($WorkerPath) -or
        $identity.session_id -ne $LauncherIdentity.session_id -or
        [datetime]::Parse($identity.start_time_utc).ToUniversalTime() -lt
            [datetime]::Parse($LauncherIdentity.start_time_utc).ToUniversalTime() -or
        (Get-XenBoundProcessState $LauncherIdentity) -cne 'running') {
        throw 'Worker精确身份或Launcher复核失败。'
    }
    # 再查父关系，避免CIM枚举后PID已被其他父进程复用。
    $again = @(Get-CimInstance Win32_Process -Filter ('ProcessId={0}' -f $identity.process_id) -ErrorAction Stop)
    $confirmed = Select-XenSoakWorker $LauncherIdentity $WorkerPath $again
    if ($null -eq $confirmed -or (Get-XenBoundProcessState $identity) -cne 'running') {
        throw 'Worker复核期间退出或父关系变化。'
    }
    return $identity
}

function Test-XenResourceReady([string]$Directory, $WorkerIdentity) {
    $partial = Join-Path $Directory 'summary.partial.json'
    $csv = Join-Path $Directory 'samples.partial.csv'
    if (-not [IO.File]::Exists($partial) -or -not [IO.File]::Exists($csv)) { return $false }
    $summary = Get-Content -LiteralPath $partial -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$summary.target_process_id -ne [int]$WorkerIdentity.process_id -or
        $summary.expected_executable_path -ine $WorkerIdentity.executable_path -or
        ([datetimeoffset]$summary.expected_start_time_utc).UtcDateTime.Ticks -ne
            [long]$WorkerIdentity.start_time_utc_ticks) { throw '资源附件绑定身份不符。' }
    # 文件存在只说明已打开；必须确有表头后的第一条完整有效样本。
    $lines = @(Get-Content -LiteralPath $csv -Encoding UTF8 -TotalCount 2)
    if ($lines.Count -ne 2 -or -not $lines[1].EndsWith('"')) { return $false }
    $rows = @($lines | ConvertFrom-Csv)
    if ($rows.Count -ne 1) { return $false }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $time = [double]::Parse($rows[0].elapsed_seconds, $culture)
    return $time -ge 0 -and [long]$rows[0].private_bytes -gt 0 -and [int]$rows[0].threads -gt 0
}

function Get-XenSoakRecoveryState($Launch, $ResourceSummary, [bool]$BindingsUnchanged) {
    # Runtime全摄入schema由独立报告实现接入；资源满时长永远不授予长稳通过。
    $resourceState = if ($null -eq $ResourceSummary) { 'missing_or_partial' } else { [string]$ResourceSummary.status }
    $resourceIdentityMatches = $false
    if ($null -ne $ResourceSummary -and $null -ne $Launch.PSObject.Properties['worker_identity'] -and
        $null -ne $Launch.worker_identity -and $null -ne $ResourceSummary.PSObject.Properties['target_process_id']) {
        $worker = $Launch.worker_identity
        $resourceIdentityMatches = [int]$ResourceSummary.target_process_id -eq [int]$worker.process_id -and
            $ResourceSummary.expected_executable_path -ieq $worker.executable_path -and
            ([datetimeoffset]$ResourceSummary.expected_start_time_utc).UtcDateTime.Ticks -eq
                [long]$worker.start_time_utc_ticks
    }
    [ordered]@{
        execution_status = [string]$Launch.supervision_status
        resource_status = $resourceState; bindings_unchanged = $BindingsUnchanged
        resource_identity_matches = $resourceIdentityMatches
        resource_runtime_window_coverage = 'not_evaluated_cross_clock'
        runtime_evaluation = 'pending_full_session_report_validation'
        soak_completed = $false; physical_effect_verified = $false
        human_observation_required = $true
    }
}

function Invoke-XenSoakSupervision($Task, $Launch, [string]$LauncherPath, [string]$WorkerPath,
    [string]$RunDirectory, [string]$SamplerPath, [scriptblock]$SaveState) {
    $collector = $null
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        $receiptPath = Join-Path $RunDirectory 'launcher-identity.json'
        while (-not [IO.File]::Exists($receiptPath)) {
            if ($clock.Elapsed.TotalSeconds -ge $Task.startup_wait_budget_seconds) {
                throw '等待Launcher身份回执超时。'
            }
            Start-Sleep -Milliseconds 100
        }
        $identity = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $current = [Diagnostics.Process]::GetCurrentProcess()
        try { $session = $current.SessionId } finally { $current.Dispose() }
        if ($identity.schema -ne 1 -or $identity.executable_path -ine [IO.Path]::GetFullPath($LauncherPath) -or
            $identity.session_id -ne $session -or $session -eq 0 -or
            (Get-XenBoundProcessState $identity) -cne 'running') { throw 'Launcher公开身份回执不符。' }
        $Launch.launcher_identity = $identity
        & $SaveState $Launch
        while ($null -eq $Launch.worker_identity) {
            $Launch.worker_identity = Find-XenSoakWorker $identity $WorkerPath
            if ($clock.Elapsed.TotalSeconds -ge $Task.startup_wait_budget_seconds) { throw '等待唯一Worker超时。' }
            if ($null -eq $Launch.worker_identity) { Start-Sleep -Milliseconds 100 }
        }
        & $SaveState $Launch
        $resources = Join-Path $RunDirectory 'resources'
        $engine = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell/v1.0/powershell.exe'
        $worker = $Launch.worker_identity
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $SamplerPath + '"'),
            '-TargetProcessId',$worker.process_id,'-ExpectedExecutablePath',('"' + $worker.executable_path + '"'),
            '-ExpectedStartTimeUtc',$worker.start_time_utc,'-OutputDirectory',('"' + $resources + '"'),
            '-DurationSeconds',$Task.resource_max_duration_seconds,
            '-IntervalMilliseconds',$Task.resource_interval_milliseconds)
        $collector = Start-Process -FilePath $engine -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $RunDirectory 'resource-collector.stdout.log') `
            -RedirectStandardError (Join-Path $RunDirectory 'resource-collector.stderr.log')
        # PS5.1的PassThru对象须在进程存活时持有句柄，退出后才能可靠读取退出码。
        $null = $collector.Handle
        # 新进程尚未装载主模块时MainModule可暂不可读；只等待本次持有句柄的子进程。
        while ($null -eq $Launch.collector_identity) {
            try { $Launch.collector_identity = Get-XenProcessIdentity $collector }
            catch {
                if ($collector.HasExited -or $clock.Elapsed.TotalSeconds -ge $Task.startup_wait_budget_seconds) { throw }
                Start-Sleep -Milliseconds 50
            }
        }
        & $SaveState $Launch
        while (-not (Test-XenResourceReady $resources $worker)) {
            if ($collector.HasExited) { throw '资源采集器未生成有效首样本即退出。' }
            if ($clock.Elapsed.TotalSeconds -ge $Task.startup_wait_budget_seconds) { throw '资源首样本就绪超时。' }
            Start-Sleep -Milliseconds 100
        }
        $Launch.resource_ready_utc = [datetime]::UtcNow.ToString('o')
        $Launch.supervision_status = 'observing_worker'
        & $SaveState $Launch
        Write-Host ('Worker PID={0}身份已绑定，资源首样本就绪。现在可由用户启动Runtime；目标{1}秒，默认值不代表用户已确认。' -f
            $worker.process_id, $Task.runtime_duration_seconds)
        $readyClock = [Diagnostics.Stopwatch]::StartNew()
        $notified = $false
        while ($true) {
            $state = Get-XenBoundProcessState $worker
            if ($state -cne 'running') {
                $Launch.supervision_status = if ($state -ceq 'exited') { 'worker_exited' } else { 'worker_identity_changed' }
                break
            }
            if ($collector.HasExited) {
                $Launch.resource_exit_code = $collector.ExitCode
                $Launch.supervision_status = 'resource_collector_ended_before_worker'
                Write-Host '资源采集已结束，覆盖可能不足。请用户自行停止Runtime并退出；脚本不代停。'
                break
            }
            if (-not $notified -and $readyClock.Elapsed.TotalSeconds -ge $Task.runtime_duration_seconds) {
                Write-Host '距资源就绪已达到计划时长；这不是Runtime连续时长证明。请依据实际Runtime开始时间自行完成并停止保存。'
                $notified = $true
            }
            Start-Sleep -Milliseconds 500
        }
        & $SaveState $Launch
        if ($Launch.supervision_status -eq 'worker_exited') {
            $closing = [Diagnostics.Stopwatch]::StartNew()
            while ((Get-XenBoundProcessState $identity) -ceq 'running' -and
                $closing.Elapsed.TotalSeconds -lt $Task.close_wait_budget_seconds) {
                $next = Find-XenSoakWorker $identity $WorkerPath
                if ($null -ne $next) {
                    $Launch.supervision_status = 'worker_restarted'
                    $Launch.replacement_worker_identity = $next
                    break
                }
                Start-Sleep -Milliseconds 100
            }
            if ($Launch.supervision_status -eq 'worker_exited' -and
                (Get-XenBoundProcessState $identity) -ceq 'running') { $Launch.supervision_status = 'launcher_exit_unconfirmed' }
            $null = $collector.WaitForExit([int]($Task.resource_interval_milliseconds + 6000))
        }
        if ($collector.HasExited) { $collector.WaitForExit(); $Launch.resource_exit_code = $collector.ExitCode }
    } catch {
        $Launch.supervision_status = 'supervision_failed'
        $Launch.failure_type = $_.Exception.GetType().FullName
        $Launch.failure_line = $_.InvocationInfo.ScriptLineNumber
        Write-Host '长稳监督未完成，公开失败类型已记录；请用户自行停止并保存，脚本未停止目标。'
    } finally {
        if ($Launch.supervision_status -eq 'observing_worker') { $Launch.supervision_status = 'interrupted' }
        $Launch.ended_utc = [datetime]::UtcNow.ToString('o')
        & $SaveState $Launch
        if ($null -ne $collector) { $collector.Dispose() }
    }
}
