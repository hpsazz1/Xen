#requires -Version 5.1
<#
.SYNOPSIS
只读采集已存在进程的资源趋势；不启动、停止或武装目标。
.DESCRIPTION
身份必须同时匹配PID、完整映像路径、UTC启动时间。CPU百分比以单逻辑核为100%，可大于100。
退出码0仅表示采样时长完成；2目标退出、3目标不存在、4身份不符、5采集失败、6中断。
输出目录必须不存在且父目录已存在。硬终止留下partial文件，不能认作完成。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetProcessId,
    [Parameter(Mandatory=$true)][string]$ExpectedExecutablePath,
    [Parameter(Mandatory=$true)][datetimeoffset]$ExpectedStartTimeUtc,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,86400)][int]$DurationSeconds = 3600,
    [ValidateRange(100,60000)][int]$IntervalMilliseconds = 5000
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture
$encoding = New-Object Text.UTF8Encoding($true)
# 只接受完全限定的文件系统路径，不接受相对路径或提供程序路径。
foreach ($value in @($ExpectedExecutablePath, $OutputDirectory)) {
    if ($value -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)') {
        throw '必须提供完整文件系统路径。'
    }
}
$expectedPath = [IO.Path]::GetFullPath($ExpectedExecutablePath)
$outputPath = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
$parentPath = [IO.Path]::GetDirectoryName($outputPath)
if (-not [IO.Directory]::Exists($parentPath)) { throw '输出父目录必须已存在。' }
# New-Item不使用Force：现有目录即使为空也拒绝，不覆盖历史证据。
$null = New-Item -ItemType Directory -Path $outputPath -ErrorAction Stop
$summaryPartial = Join-Path $outputPath 'summary.partial.json'
$csvPartial = Join-Path $outputPath 'samples.partial.csv'
$summary = [ordered]@{
    schema_version = 1; status = 'interrupted'; resource_capture_completed = $false
    runtime_acceptance = 'not_evaluated'; exit_code = 6
    target_process_id = $TargetProcessId; expected_executable_path = $expectedPath
    expected_start_time_utc = $ExpectedStartTimeUtc.UtcDateTime.ToString('o')
    requested_duration_seconds = $DurationSeconds; interval_milliseconds = $IntervalMilliseconds
    started_utc = [datetime]::UtcNow.ToString('o'); ended_utc = $null
    elapsed_seconds = 0.0; sample_count = 0; cpu_percent_basis = 'single_logical_core_100_percent'
    cpu_observation_span_seconds = 0.0
    stopwatch_frequency = [Diagnostics.Stopwatch]::Frequency
    stopwatch_high_resolution = [Diagnostics.Stopwatch]::IsHighResolution
    first_sample = $null; last_sample = $null; maxima = @{}
    failure_type = $null; failure_hresult = $null
}
[IO.File]::WriteAllText($summaryPartial, ($summary | ConvertTo-Json -Depth 6), $encoding)
$timer = [Diagnostics.Stopwatch]::StartNew()
$targetProcess = $null
$writer = $null
$previousCpu = $null
$previousElapsed = $null
$firstCpuElapsed = $null
try {
    try { $targetProcess = [Diagnostics.Process]::GetProcessById($TargetProcessId) }
    catch [ArgumentException] { $summary.status = 'process_not_found'; $summary.exit_code = 3; throw }
    $stream = New-Object IO.FileStream($csvPartial, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $writer = New-Object IO.StreamWriter($stream, $encoding)
    $writer.AutoFlush = $true
    while ($true) {
        $targetProcess.Refresh()
        if ($targetProcess.HasExited) { $summary.status = 'process_exited'; $summary.exit_code = 2; break }
        $startUtc = $targetProcess.StartTime.ToUniversalTime()
        $actualPath = [IO.Path]::GetFullPath($targetProcess.MainModule.FileName)
        if ($startUtc.Ticks -ne $ExpectedStartTimeUtc.UtcDateTime.Ticks -or
            -not [string]::Equals($actualPath, $expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
            $summary.status = 'identity_mismatch'; $summary.exit_code = 4; break
        }
        # 属性读并非原子快照；身份在采样前后都复核，不跨进程拼接计数。
        # CPU累计值使用紧邻读取区间中点；后续内存/线程查询延迟不能错配CPU分母。
        $cpuReadBefore = $timer.Elapsed.TotalSeconds
        $cpu = $targetProcess.TotalProcessorTime.TotalSeconds
        $cpuReadAfter = $timer.Elapsed.TotalSeconds
        $elapsed = 0.5 * $cpuReadBefore + 0.5 * $cpuReadAfter
        $utc = [datetime]::UtcNow.ToString('o')
        $privateBytes = $targetProcess.PrivateMemorySize64
        $workingBytes = $targetProcess.WorkingSet64
        $handles = $targetProcess.HandleCount
        $threads = $targetProcess.Threads.Count
        $resourcesCompletedElapsed = $timer.Elapsed.TotalSeconds
        $targetProcess.Refresh()
        if ($targetProcess.HasExited) { $summary.status = 'process_exited'; $summary.exit_code = 2; break }
        if ($targetProcess.StartTime.ToUniversalTime().Ticks -ne $startUtc.Ticks -or
            -not [string]::Equals([IO.Path]::GetFullPath($targetProcess.MainModule.FileName), $expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
            $summary.status = 'identity_mismatch'; $summary.exit_code = 4; break
        }
        $deltaCpu = $null; $deltaTime = $null; $cpuPercent = $null
        if ($null -ne $previousCpu) {
            $deltaCpu = $cpu - $previousCpu
            $deltaTime = $elapsed - $previousElapsed
            if ($deltaCpu -lt 0 -or $deltaTime -le 0) { throw '进程累计时间或单调时钟无效。' }
            $cpuPercent = 100.0 * $deltaCpu / $deltaTime
        }
        $sample = [ordered]@{
            utc = $utc; elapsed_seconds = $elapsed; cpu_total_seconds = $cpu
            cpu_read_span_seconds = ($cpuReadAfter - $cpuReadBefore)
            resources_completed_elapsed_seconds = $resourcesCompletedElapsed
            cpu_delta_seconds = $deltaCpu; interval_seconds = $deltaTime; cpu_percent_one_core = $cpuPercent
            private_bytes = $privateBytes; working_set_bytes = $workingBytes; handles = $handles; threads = $threads
        }
        $csvSample = [ordered]@{}
        foreach ($key in $sample.Keys) {
            $item = $sample[$key]
            if ($item -is [double]) { $csvSample[$key] = $item.ToString('R', $invariant) }
            else { $csvSample[$key] = $item }
        }
        $lines = @([pscustomobject]$csvSample | ConvertTo-Csv -NoTypeInformation)
        if ($summary.sample_count -eq 0) {
            $writer.WriteLine($lines[0]); $summary.first_sample = $sample
            $firstCpuElapsed = $elapsed
        }
        $writer.WriteLine($lines[1])
        $summary.sample_count++
        $summary.last_sample = $sample
        foreach ($key in @('private_bytes','working_set_bytes','handles','threads','cpu_percent_one_core')) {
            if ($null -ne $sample[$key] -and
                (-not $summary.maxima.ContainsKey($key) -or $sample[$key] -gt $summary.maxima[$key])) {
                $summary.maxima[$key] = $sample[$key]
            }
        }
        $previousCpu = $cpu; $previousElapsed = $elapsed
        $summary.cpu_observation_span_seconds = $elapsed - $firstCpuElapsed
        if ($summary.cpu_observation_span_seconds -ge $DurationSeconds -and $summary.sample_count -ge 2) {
            $summary.status = 'duration_completed'; $summary.exit_code = 0
            $summary.resource_capture_completed = $true; break
        }
        $remainingMs = ($DurationSeconds - ($timer.Elapsed.TotalSeconds - $firstCpuElapsed)) * 1000.0
        $sleepMs = [math]::Max(1, [math]::Min($IntervalMilliseconds, $remainingMs))
        Start-Sleep -Milliseconds ([int][math]::Ceiling($sleepMs))
    }
}
catch {
    if ($summary.status -eq 'interrupted') {
        $exited = $false
        if ($null -ne $targetProcess) { try { $exited = $targetProcess.HasExited } catch {} }
        if ($exited) { $summary.status = 'process_exited'; $summary.exit_code = 2 }
        else { $summary.status = 'collection_failed'; $summary.exit_code = 5 }
    }
    # 不序列化异常文本、命令行、环境或其他可能含敏感信息的上下文。
    $summary.failure_type = $_.Exception.GetType().FullName
    $summary.failure_hresult = $_.Exception.HResult
}
finally {
    $timer.Stop()
    $summary.ended_utc = [datetime]::UtcNow.ToString('o')
    $summary.elapsed_seconds = $timer.Elapsed.TotalSeconds
    if ($null -ne $writer) { $writer.Dispose() }
    if ($null -ne $targetProcess) { $targetProcess.Dispose() }
    # 只发布本次独占目录中的文件。硬杀/断电时partial保留为未完成证据。
    try {
        if ([IO.File]::Exists($csvPartial)) { [IO.File]::Move($csvPartial, (Join-Path $outputPath 'samples.csv')) }
        [IO.File]::WriteAllText($summaryPartial, ($summary | ConvertTo-Json -Depth 6), $encoding)
        [IO.File]::Move($summaryPartial, (Join-Path $outputPath 'summary.json'))
    }
    catch {
        $summary.status = 'collection_failed'; $summary.exit_code = 5
        $summary.resource_capture_completed = $false
        $summary.failure_type = $_.Exception.GetType().FullName
        $summary.failure_hresult = $_.Exception.HResult
        # 若输出介质也不可写，仅保留此前partial，绝不把它认作最终完成。
        try {
            [IO.File]::WriteAllText($summaryPartial, ($summary | ConvertTo-Json -Depth 6), $encoding)
            [IO.File]::Move($summaryPartial, (Join-Path $outputPath 'summary.json'))
        } catch {}
    }
}
Write-Output ('资源采集结束：{0}，样本={1}；不代表Runtime验收。' -f $summary.status, $summary.sample_count)
exit $summary.exit_code
