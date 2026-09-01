param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [ValidateRange(1, 1000000)]
    [uint64]$BaselineSamples = 240,
    [ValidateRange(1, 1000000)]
    [uint64]$ResponseSamples = 120,
    [ValidateRange(1, 1000000)]
    [uint64]$GuardSamples = 120,
    [ValidateRange(1, 2400)]
    [uint64]$SidecarFrames = 1600,
    [ValidateRange(1, 60)]
    [uint64]$MaxSeconds = 15,
    [string]$PythonExecutable = "python",
    [string]$LeftWitnessRoi = "16,48,96,224",
    [string]$RightWitnessRoi = "208,48,96,224"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-FileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-FileIdentity([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不是普通文件：$Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $before = Get-Item -LiteralPath $resolved
    $hash = Get-FileSha256 $resolved
    $after = Get-Item -LiteralPath $resolved
    if ($before.Length -ne $after.Length -or
        $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) {
        throw "$Description 在哈希期间发生变化：$resolved"
    }
    return [ordered]@{
        path = $resolved
        size = [uint64]$after.Length
        sha256 = $hash
    }
}

function Read-IniSection(
        [string]$Path,
        [string]$SectionName) {
    $values = [ordered]@{}
    $current = ""
    foreach ($rawLine in [IO.File]::ReadAllLines($Path)) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(";") -or
            $line.StartsWith("#")) {
            continue
        }
        if ($line -match '^\[(.+)\]$') {
            $current = $Matches[1].Trim()
            continue
        }
        if (-not [string]::Equals(
                $current, $SectionName,
                [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            throw "config [$SectionName] 存在非法行：$line"
        }
        $key = $line.Substring(0, $separator).Trim().ToLowerInvariant()
        $value = $line.Substring($separator + 1).Trim()
        if ($values.Contains($key)) {
            throw "config [$SectionName] 存在重复键：$key"
        }
        $values[$key] = $value
    }
    if ($values.Count -eq 0) {
        throw "config 缺少 [$SectionName]"
    }
    return $values
}

function Get-RequiredValue(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    if (-not $Values.Contains($Name) -or
        [string]::IsNullOrWhiteSpace([string]$Values[$Name])) {
        throw "Capture config 缺少 $Name"
    }
    return [string]$Values[$Name]
}

function Get-RequiredInteger(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $text = Get-RequiredValue $Values $Name
    if ($text -notmatch '^[0-9]+$') {
        throw "Capture config 的 $Name 不是非负整数"
    }
    return [int]$text
}

function Get-RequiredBoolean(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $text = (Get-RequiredValue $Values $Name).ToLowerInvariant()
    if ($text -eq "true") { return $true }
    if ($text -eq "false") { return $false }
    throw "Capture config 的 $Name 不是布尔值"
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    if (Test-Path -LiteralPath $Path) {
        throw "JSON 发布目标已存在，拒绝覆盖：$Path"
    }
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "JSON 发布父目录不存在：$parent"
    }
    $pending = "$Path.pending-$PID-$([guid]::NewGuid().ToString('N'))"
    $utf8 = [Text.UTF8Encoding]::new($false)
    try {
        $json = $Value | ConvertTo-Json -Depth 20
        [IO.File]::WriteAllText($pending, $json + "`n", $utf8)
        [IO.File]::Move($pending, $Path)
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

function Quote-NativeArgument([string]$Value) {
    if ($Value.Contains('"')) {
        throw "原生命令参数不得包含双引号。"
    }
    return '"' + $Value + '"'
}

function Wait-SidecarIncoming(
        [Diagnostics.Process]$Process,
        [string]$Parent,
        [int]$TimeoutMilliseconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
            $Process.Refresh()
            if ($Process.HasExited) {
                throw "sidecar 在 incoming 就绪前退出，ExitCode=$($Process.ExitCode)"
            }
            $pattern = ".pixel-evidence.incoming-$($Process.Id)-*"
            $matches = @(Get-ChildItem -LiteralPath $Parent -Directory |
                Where-Object { $_.Name -like $pattern })
            if ($matches.Count -gt 1) {
                throw "当前 sidecar PID 匹配多个 incoming 目录"
            }
            if ($matches.Count -eq 1 -and
                (Test-Path -LiteralPath (
                    Join-Path $matches[0].FullName "source-binding.json") `
                    -PathType Leaf)) {
                return $matches[0].FullName
            }
            Start-Sleep -Milliseconds 25
        }
    } finally {
        $watch.Stop()
    }
    throw "等待 sidecar incoming 就绪超时"
}

$forbiddenProcesses = @(
    Get-Process -Name Xen, XenLauncher, XenMouseBenchmark, XenMouseEffectProbe `
        -ErrorAction SilentlyContinue)
if ($forbiddenProcesses.Count -ne 0) {
    throw "output-off rehearsal 要求正常 Aim/其他 Mouse 工具关闭"
}

$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path
$config = Get-FileIdentity $ConfigPath "config.ini"
$obsBinding = Get-FileIdentity $ObsSourceBindingPath "OBS source binding"
$sequenceTool = Get-FileIdentity (
    Join-Path $resolvedToolRoot "XenMouseEffectProbeSequence.exe") `
    "probe sequence tool"
$probeTool = Get-FileIdentity (
    Join-Path $resolvedToolRoot "XenMouseEffectProbe.exe") `
    "probe runner"
$sidecarTool = Get-FileIdentity (
    Join-Path $resolvedToolRoot "XenCaptureEvidence.exe") `
    "pixel sidecar"
[void](Get-FileIdentity (
    Join-Path $resolvedToolRoot "opencv_world4140.dll") "OpenCV runtime")
[void](Get-FileIdentity (
    Join-Path $resolvedToolRoot "Processing.NDI.Lib.x64.dll") "NDI runtime")

if (Test-Path -LiteralPath $RunDirectory) {
    throw "RunDirectory 已存在，拒绝覆盖：$RunDirectory"
}
$runParent = Split-Path -Parent ([IO.Path]::GetFullPath($RunDirectory))
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
[void](New-Item -ItemType Directory -Path $resolvedRun)

$capture = Read-IniSection $config.path "capture"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi") {
    throw "output-off probe 只接受 NDI Capture"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
if ($ndiSource -eq "Auto") {
    throw "output-off probe 不接受 Auto NDI source"
}
$clockSyncUrl = Get-RequiredValue $capture "ndi_clock_sync_url"
$frameLayout = (Get-RequiredValue $capture "ndi_frame_layout").ToLowerInvariant()
if ($frameLayout -notin @(
        "center_crop_1_to_1", "full_frame_1_to_1", "full_frame_scaled")) {
    throw "Capture ndi_frame_layout 非法"
}
$sourceWidth = Get-RequiredInteger $capture "ndi_source_width"
$sourceHeight = Get-RequiredInteger $capture "ndi_source_height"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$centerRoi = Get-RequiredBoolean $capture "center_roi"
$roiX = Get-RequiredInteger $capture "roi_x"
$roiY = Get-RequiredInteger $capture "roi_y"
$discoveryTimeout = Get-RequiredInteger $capture "ndi_discovery_timeout_ms"
$receiveTimeout = Get-RequiredInteger $capture "ndi_receive_timeout_ms"
$disconnectTimeout = Get-RequiredInteger $capture "ndi_disconnect_timeout_ms"
$clockInterval = Get-RequiredInteger $capture "ndi_clock_sync_interval_ms"
$clockTimeout = Get-RequiredInteger $capture "ndi_clock_sync_timeout_ms"
$clockMaxAge = Get-RequiredInteger $capture "ndi_clock_mapping_max_age_ms"
$requireMetadata = Get-RequiredBoolean $capture "ndi_require_frame_metadata"

$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$sequencePath = Join-Path $resolvedRun "sequence.json"
& $sequenceTool.path --output $sequencePath `
    --baseline-samples $BaselineSamples `
    --response-samples $ResponseSamples `
    --guard-samples $GuardSamples
if ($LASTEXITCODE -ne 0) {
    throw "probe sequence tool 失败，ExitCode=$LASTEXITCODE"
}
$sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
    ConvertFrom-Json
$samples = @($sequence.samples)
$pulses = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
$pulseDirections = @($pulses | ForEach-Object { [int]$_.dx_counts })
if ([int]$sequence.schema -ne 1 -or
    [string]$sequence.profile -ne "sparse_pulse_a" -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
    @($samples | Where-Object { [int]$_.dy_counts -ne 0 }).Count -ne 0 -or
    ($pulseDirections -join ",") -ne "1,-1,-1,1") {
    throw "生成的 sparse-pulse A 序列不满足 X-only/net/prefix 合同"
}
$sequenceIdentity = Get-FileIdentity $sequencePath "probe sequence"

$bindingPath = Join-Path $resolvedRun "probe-binding.json"
$binding = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_binding"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "output_off_rehearsal"
    profile = "sparse_pulse_a"
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $config
    obs_source_binding = $obsBinding
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 1
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-FileIdentity $bindingPath "probe binding"

$pixelOutput = Join-Path $resolvedRun "pixel-evidence"
$sidecarStdout = Join-Path $resolvedRun "pixel-sidecar.stdout.log"
$sidecarStderr = Join-Path $resolvedRun "pixel-sidecar.stderr.log"
$sidecarArguments = @(
    "--ndi-source", (Quote-NativeArgument $ndiSource),
    "--binding", (Quote-NativeArgument $bindingPath),
    "--output", (Quote-NativeArgument $pixelOutput),
    "--frames", [string]$SidecarFrames,
    "--max-seconds", [string]$MaxSeconds,
    "--frame-layout", $frameLayout,
    "--source-width", [string]$sourceWidth,
    "--source-height", [string]$sourceHeight,
    "--roi-width", [string]$roiWidth,
    "--roi-height", [string]$roiHeight,
    "--discovery-timeout-ms", [string]$discoveryTimeout,
    "--receive-timeout-ms", [string]$receiveTimeout,
    "--disconnect-timeout-ms", [string]$disconnectTimeout,
    "--clock-sync-url", (Quote-NativeArgument $clockSyncUrl),
    "--clock-sync-interval-ms", [string]$clockInterval,
    "--clock-sync-timeout-ms", [string]$clockTimeout,
    "--clock-mapping-max-age-ms", [string]$clockMaxAge,
    "--require-source-timing"
)
if (-not $centerRoi) {
    $sidecarArguments += @(
        "--roi-x", [string]$roiX, "--roi-y", [string]$roiY)
}
if ($requireMetadata) {
    $sidecarArguments += "--require-frame-metadata"
}
$sidecarProcess = Start-Process -FilePath $sidecarTool.path `
    -WorkingDirectory $resolvedToolRoot `
    -ArgumentList ($sidecarArguments -join " ") -WindowStyle Hidden `
    -RedirectStandardOutput $sidecarStdout `
    -RedirectStandardError $sidecarStderr -PassThru
# Windows PowerShell 5 在重定向输出时可能于子进程退出后丢失 ExitCode；
# 启动后立即取得句柄，使后续 WaitForExit/ExitCode 保持同一进程身份。
[void]$sidecarProcess.Handle

$incoming = $null
try {
    $incoming = Wait-SidecarIncoming `
        $sidecarProcess $resolvedRun 10000
    $copiedBinding = Get-FileIdentity (
        Join-Path $incoming "source-binding.json") "sidecar binding copy"
    if ($copiedBinding.sha256 -ne $bindingIdentity.sha256) {
        throw "sidecar binding copy 与 probe binding SHA 不一致"
    }

    $reportPath = Join-Path $resolvedRun "command-report.json"
    $probeArguments = @(
        "--mode", "output-off-rehearsal",
        "--config", $config.path,
        "--sequence", $sequencePath,
        "--binding", $bindingPath,
        "--binding-sha256", $bindingIdentity.sha256,
        "--sidecar-pid", [string]$sidecarProcess.Id,
        "--sidecar-incoming", $incoming,
        "--report", $reportPath,
        "--run-uuid", $runUuid,
        "--activation-epoch", [string]$activationEpoch,
        "--max-seconds", [string]$MaxSeconds
    )
    & $probeTool.path @probeArguments
    $probeExitCode = $LASTEXITCODE

    if (-not $sidecarProcess.WaitForExit(
            [int](($MaxSeconds + 15) * 1000))) {
        Stop-Process -Id $sidecarProcess.Id -Force
        throw "等待本脚本启动的 output-off sidecar 退出超时"
    }
    $sidecarProcess.Refresh()
    if ($probeExitCode -ne 0) {
        throw "XenMouseEffectProbe output-off 失败，ExitCode=$probeExitCode"
    }
    if ($sidecarProcess.ExitCode -ne 0) {
        throw "XenCaptureEvidence 失败，ExitCode=$($sidecarProcess.ExitCode)"
    }

    $manifestPath = Join-Path $pixelOutput "manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "output-off 缺少 sidecar manifest 或 command report"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $pixelFrames = @($manifest.frames)
    $events = @($report.result.events)
    if ([int]$manifest.schema_version -ne 1 -or
        [string]$manifest.evidence_type -ne "output_off_capture" -or
        [bool]$manifest.physical_output_capability -or
        [string]$manifest.capture_source_name -ne $ndiSource -or
        [uint64]$manifest.requested_frame_count -ne $SidecarFrames -or
        [uint64]$manifest.recorded_frame_count -ne $SidecarFrames -or
        $pixelFrames.Count -ne $SidecarFrames -or
        [string]$manifest.source_binding.sha256 -ne $bindingIdentity.sha256) {
        throw "sidecar manifest 顶层身份或完整帧数合同无效"
    }
    if ([string]$report.run_uuid -ne $runUuid -or
        [uint64]$report.activation_epoch -ne $activationEpoch -or
        [string]$report.dispatch_mode -ne "output_off_rehearsal" -or
        [string]$report.sequence_sha256 -ne
            [string]$sequence.sequence_sha256 -or
        [string]$report.binding.probe_binding_sha256 -ne
            $bindingIdentity.sha256 -or
        [string]$report.binding.sidecar_run_uuid -ne $runUuid -or
        [string]$report.result.state -ne "completed" -or
        -not [bool]$report.result.complete -or
        [int64]$report.result.cumulative_requested_x_counts -ne 0 -or
        [int64]$report.result.cumulative_backend_completed_x_counts -ne 0 -or
        $events.Count -ne $samples.Count) {
        throw "command report 的 run/sequence/binding/未发送合同无效"
    }

    $pixelTimestamps = [Collections.Generic.HashSet[int64]]::new()
    $pngHashesVerified = 0
    foreach ($frame in $pixelFrames) {
        if (-not [bool]$frame.source_timestamp_valid -or
            -not [bool]$frame.source_time_timing_valid -or
            [string]$frame.source_time_basis -ne "NDI_SDK_SUBMISSION" -or
            [string]$frame.source_clock_status -ne "VALID" -or
            [uint64]$frame.source_clock_sample_count -lt 1 -or
            [uint64]$frame.source_dropped_frames -ne 0 -or
            [uint64]$frame.transport_dropped_frames -ne 0 -or
            [uint64]$frame.transport_invalid_packets -ne 0) {
            throw "sidecar frame source timing/drop 合同无效：index=$($frame.index)"
        }
        [void]$pixelTimestamps.Add([int64]$frame.source_timestamp)
        $pngPath = Join-Path $pixelOutput ([string]$frame.file)
        if ((Get-FileSha256 $pngPath) -ne [string]$frame.png_sha256) {
            throw "sidecar PNG SHA 不匹配：$pngPath"
        }
        $pngHashesVerified++
    }

    $matchedEvents = 0
    $nominalPulses = [Collections.Generic.List[int]]::new()
    $previousSequence = $null
    $previousTimestamp = $null
    foreach ($event in $events) {
        if ([bool]$event.dispatch_attempted -or
            [int]$event.requested_dx_counts -ne 0 -or
            [int]$event.requested_dy_counts -ne 0 -or
            [bool]$event.backend_succeeded -or
            [bool]$event.protocol_ack_received -or
            [int]$event.nominal_dy_counts -ne 0 -or
            [string]$event.source_time_basis -ne "NDI_SDK_SUBMISSION" -or
            [string]$event.source_clock_status -ne "VALID" -or
            [uint64]$event.source_dropped_frames -ne 0 -or
            [uint64]$event.transport_dropped_frames -ne 0 -or
            [uint64]$event.transport_invalid_packets -ne 0) {
            throw "output-off command event 冒充实际输入或 source 合同无效"
        }
        if ($null -ne $previousSequence -and
            [uint64]$event.source_frame_sequence -ne
                [uint64]$previousSequence + 1) {
            throw "command event source frame 不连续"
        }
        if ($null -ne $previousTimestamp -and
            [int64]$event.source_timestamp -le [int64]$previousTimestamp) {
            throw "command event source timestamp 未严格递增"
        }
        $previousSequence = [uint64]$event.source_frame_sequence
        $previousTimestamp = [int64]$event.source_timestamp
        if (-not $pixelTimestamps.Contains([int64]$event.source_timestamp)) {
            throw "command event 无法按 NDI raw timestamp 对齐 sidecar frame"
        }
        $matchedEvents++
        if ([int]$event.nominal_dx_counts -ne 0) {
            $nominalPulses.Add([int]$event.nominal_dx_counts)
        }
    }
    if (($nominalPulses -join ",") -ne "1,-1,-1,1") {
        throw "output-off report 未保留完整 +1/-1、-1/+1 名义序列"
    }

    $analyzer = Join-Path $PSScriptRoot `
        "analyze_mouse_effect_probe_pixels.py"
    if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) {
        throw "缺少 background witness analyzer：$analyzer"
    }
    $witnessPath = Join-Path $resolvedRun `
        "background-witness-baseline.json"
    $witnessPairsPath = Join-Path $resolvedRun `
        "background-witness-pairs.csv"
    $previousPythonUtf8 = $env:PYTHONUTF8
    try {
        $env:PYTHONUTF8 = "1"
        & $PythonExecutable $analyzer `
            --manifest $manifestPath `
            --command-report $reportPath `
            --sequence $sequencePath `
            --left-roi $LeftWitnessRoi `
            --right-roi $RightWitnessRoi `
            --output $witnessPath `
            --pairs-csv $witnessPairsPath
        $analyzerExitCode = $LASTEXITCODE
    } finally {
        $env:PYTHONUTF8 = $previousPythonUtf8
    }
    if ($analyzerExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $witnessPath -PathType Leaf)) {
        throw "background witness baseline 分析失败，ExitCode=$analyzerExitCode"
    }
    $witness = Get-Content -LiteralPath $witnessPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([string]$witness.status -ne "VALID" -or
        [uint64]$witness.matched_event_frame_count -ne $events.Count -or
        [string]$witness.run_binding.run_uuid -ne $runUuid -or
        [string]$witness.run_binding.probe_binding_sha256 -ne
            $bindingIdentity.sha256) {
        throw "background witness baseline 身份或状态无效"
    }

    $summaryPath = Join-Path $resolvedRun "output-off-summary.json"
    $summary = [ordered]@{
        schema_version = 1
        evidence_type = "mouse_effect_probe_output_off_rehearsal"
        status = "VALID"
        physical_output_capability = $false
        run_uuid = $runUuid
        activation_epoch = $activationEpoch
        sequence_sha256 = [string]$sequence.sequence_sha256
        probe_binding_sha256 = $bindingIdentity.sha256
        command_report_sha256 = [string]$report.report_sha256
        sidecar_manifest_sha256 = Get-FileSha256 $manifestPath
        sequence_sample_count = [uint64]$samples.Count
        nominal_pulse_count = [uint64]$nominalPulses.Count
        command_event_count = [uint64]$events.Count
        sidecar_frame_count = [uint64]$pixelFrames.Count
        source_timestamp_matched_event_count = [uint64]$matchedEvents
        png_hash_verified_count = [uint64]$pngHashesVerified
        background_witness_status = [string]$witness.status
        background_witness_sha256 = Get-FileSha256 $witnessPath
        left_witness_roi = $LeftWitnessRoi
        right_witness_roi = $RightWitnessRoi
        actual_requested_x_counts = 0
        backend_completed_x_counts = 0
        visible_effect_analyzed = $false
    }
    Write-NewUtf8Json $summaryPath $summary
    $successMessage = ((
        "output-off probe VALID：Run={0}；events={1}/{1}；" +
        "sidecar={2}/{2}；PNG SHA={2}/{2}；未发送 Mouse。") -f
        $runUuid, $events.Count, $pixelFrames.Count)
    Write-Host $successMessage
    Write-Host "RunDirectory=$resolvedRun"
} finally {
    if ($null -ne $sidecarProcess) {
        $sidecarProcess.Refresh()
        if (-not $sidecarProcess.HasExited) {
            Stop-Process -Id $sidecarProcess.Id -Force
        }
        $sidecarProcess.Dispose()
    }
}
