param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$LaunchScript
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

foreach ($path in @($PrepareScript, $LaunchScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "A2 S1 script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "A2 S1 script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_a2_s1_task',
        'dependency_calibration_a2_s1_$RunRole',
        '[ValidateSet("primary", "validation")]',
        '--profile s1-liveness-a2',
        '--peak-hold-samples',
        '[uint64]$PeakHoldSampleCount = 64',
        'challenge_pulse_count',
        'challenge_stride_sample_count',
        'peak_hold_sample_count',
        'peak_hold_frames_eligible_for_estimands',
        'a2-s1-kmbox-bracket-peak-hold-v1',
        '【准备】保持右键松开',
        '【按住右键】',
        '【现在松开右键】',
        '.ProviderPath',
        'PREPARED_NOT_LAUNCHED',
        'physical_launch_executed = $false',
        'XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT',
        'Move-Item -LiteralPath $stagingDirectory')) {
    if (-not $prepare.Contains($required)) {
        throw "A2 S1 Prepare is missing contract text: $required"
    }
}
foreach ($required in @(
        'mouse_effect_probe_a2_s1_task',
        'bracketed_kmbox',
        's1-liveness-bracket.json',
        'baseline_actual_command_zero',
        'challenge_frames_excluded_from_estimands',
        'peak_hold_sample_count',
        'peak_hold_frames_eligible_for_estimands',
        'publishing_max_seconds',
        'PUBLISHING',
        'sidecar-lifecycle.json',
        'safety-ledger.json',
        '"--safety-ledger"',
        'safety_ledger_sha256',
        'safety_monitor_terminal_decision',
        'monitor_packets',
        'payload_sha256',
        'source_endpoint_valid',
        'monitor_sequence_before',
        'monitor_sequence_after',
        'safety_monitor_packet_count',
        'safety_monitor_packet_identity_complete',
        'ConvertTo-PhysicalProbeOperatorCue',
        '【准备】保持右键松开；等待“按住右键”。',
        '【按住右键】5 秒内按住并持续保持；直到看到“现在松开右键”。',
        '【现在松开右键】命令阶段完成；正在整理证据。',
        '【记录完成】本次尚未分析可见效果。',
        '2>&1 | ForEach-Object',
        'publishedFramesDirectory',
        'png_count_at_completion')) {
    if (-not $launch.Contains($required)) {
        throw "A2 S1 Launch recording contract is missing text: $required"
    }
}
if ($launch.Contains('$task.sidecar.max_seconds + 15')) {
    throw "A2 S1 Launch 不得继续用单一 30 秒窗口混合采集与 PNG publishing"
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw "A2 S1 Prepare must not execute its Physical Launch script"
}
foreach ($verbose in @(
        '即将执行 $probeLabel',
        'sidecar 已录满并进入 PNG/哈希 publishing',
        'Physical A 已完整记录：pulse/backend/ACK=',
        '请松开右键，并把方向/可见性/异常或急停情况直接发回当前对话。')) {
    if ($launch.Contains($verbose)) {
        throw "A2 S1 Launch 不得继续显示冗长操作文本：$verbose"
    }
}

# 只提取纯等待函数，用无物理能力的睡眠进程覆盖 RECORDING、PUBLISHING
# 及原子发布前的 PNG 计数；测试不得执行 Launch 主体。
$launchTokens = $null
$launchErrors = $null
$launchAst = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $LaunchScript).Path,
    [ref]$launchTokens,
    [ref]$launchErrors)
foreach ($functionName in @(
        'ConvertTo-PhysicalProbeOperatorCue',
        'Test-SidecarPublishingStarted',
        'Get-SidecarPngCount',
        'Wait-SidecarCompletion')) {
    $functionAst = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $functionName
    }, $true)
    if ($null -eq $functionAst) {
        throw "A2 S1 Launch 缺少可测试的 sidecar 函数：$functionName"
    }
    . ([scriptblock]::Create($functionAst.Extent.Text))
}

$monitorCue = ConvertTo-PhysicalProbeOperatorCue `
    'KMBOX monitor 已就绪；不要提前按住。'
$successCue = ConvertTo-PhysicalProbeOperatorCue `
    'Mouse Effect Probe 时间线完成: report="C:\detail.json"'
$failureCue = ConvertTo-PhysicalProbeOperatorCue `
    'Mouse Effect Probe 未正常完成: stop_reason=safety_released'
$ignoredCue = ConvertTo-PhysicalProbeOperatorCue `
    'report_sha256=0123456789abcdef'
if ($monitorCue -ne
        '【按住右键】5 秒内按住并持续保持；直到看到“现在松开右键”。' -or
    $successCue -ne
        '【现在松开右键】命令阶段完成；正在整理证据。' -or
    $failureCue -ne
        '【现在松开右键】命令阶段停止；正在整理证据。' -or
    $null -ne $ignoredCue) {
    throw 'A2 S1 native transcript 未精确映射为三个简洁操作提示'
}

function Start-SleepProcess([string]$Command) {
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($Command))
    return Start-Process -FilePath powershell.exe -ArgumentList @(
        '-NoProfile', '-EncodedCommand', $encoded) -WindowStyle Hidden -PassThru
}

function Stop-TestProcess([Diagnostics.Process]$Process) {
    $Process.Refresh()
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        [void]$Process.WaitForExit(5000)
    }
}

$lifecycleTestRoot = [IO.Path]::GetFullPath((Join-Path (
    [IO.Path]::GetTempPath()) (
    'xen-a2-s1-sidecar-lifecycle-' + [guid]::NewGuid().ToString('N'))))
[void](New-Item -ItemType Directory -Path $lifecycleTestRoot)
try {
    $recordingIncoming = Join-Path $lifecycleTestRoot 'recording.incoming'
    [void](New-Item -ItemType Directory -Path $recordingIncoming)
    $recordingProcess = Start-SleepProcess 'Start-Sleep -Seconds 2'
    try {
        $recordingResult = Wait-SidecarCompletion `
            $recordingProcess $recordingIncoming 200 1000
        if (-not [bool]$recordingResult.timed_out -or
            [string]$recordingResult.phase -ne 'RECORDING' -or
            [uint64]$recordingResult.png_count -ne 0) {
            throw 'A2 S1 RECORDING timeout contract failed'
        }
    } finally {
        Stop-TestProcess $recordingProcess
    }

    $publishingIncoming = Join-Path $lifecycleTestRoot 'publishing.incoming'
    $publishingFrames = Join-Path $publishingIncoming 'frames'
    [void](New-Item -ItemType Directory -Path $publishingFrames -Force)
    $publishingFrame = (Join-Path $publishingFrames '000001.png').Replace("'", "''")
    $publishingProcess = Start-SleepProcess (
        "[IO.File]::WriteAllBytes('$publishingFrame',[byte[]](1));" +
        'Start-Sleep -Seconds 2')
    try {
        $publishingResult = Wait-SidecarCompletion `
            $publishingProcess $publishingIncoming 1000 200
        if (-not [bool]$publishingResult.timed_out -or
            [string]$publishingResult.phase -ne 'PUBLISHING' -or
            [uint64]$publishingResult.png_count -ne 1 -or
            $null -eq $publishingResult.publishing_started_ms) {
            throw 'A2 S1 PUBLISHING timeout/count contract failed'
        }
    } finally {
        Stop-TestProcess $publishingProcess
    }

    $completedIncoming = Join-Path $lifecycleTestRoot 'completed.incoming'
    $completedFrames = Join-Path $completedIncoming 'frames'
    [void](New-Item -ItemType Directory -Path $completedFrames -Force)
    $completedFrame = (Join-Path $completedFrames '000001.png').Replace("'", "''")
    $completedProcess = Start-SleepProcess (
        "[IO.File]::WriteAllBytes('$completedFrame',[byte[]](1));" +
        'Start-Sleep -Milliseconds 300')
    try {
        $completedResult = Wait-SidecarCompletion `
            $completedProcess $completedIncoming 1000 1000
        if ([bool]$completedResult.timed_out -or
            [string]$completedResult.phase -ne 'PUBLISHING' -or
            [uint64]$completedResult.png_count -ne 1) {
            throw 'A2 S1 PUBLISHING completion contract failed'
        }
    } finally {
        Stop-TestProcess $completedProcess
    }
} finally {
    if (Test-Path -LiteralPath $lifecycleTestRoot -PathType Container) {
        Remove-Item -LiteralPath $lifecycleTestRoot -Recurse -Force
    }
}

Write-Host "Mouse Effect Probe A2 S1 Prepare/Launch contract passed."
