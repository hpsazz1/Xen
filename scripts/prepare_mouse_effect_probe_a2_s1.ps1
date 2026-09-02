param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [Parameter(Mandatory = $true)]
    [ValidateSet("primary", "validation")]
    [string]$RunRole,
    [ValidateRange(1, 64)]
    [uint64]$ChallengePulseCount = 16,
    [ValidateRange(1, 64)]
    [uint64]$ChallengeStrideSampleCount = 4,
    [ValidateRange(1, 1024)]
    [uint64]$PeakHoldSampleCount = 64,
    [ValidateRange(32, 1024)]
    [uint64]$SettleSampleCount = 128,
    [ValidateRange(32, 1600)]
    [uint64]$BaselineSampleCount = 512,
    [ValidateRange(32, 2400)]
    [uint64]$SidecarFrames = 2400,
    [ValidateRange(1, 60)]
    [uint64]$MaxSeconds = 15,
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

function Get-TextSha256([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($Text))).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-FileIdentity([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不是普通文件：$Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).ProviderPath
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

function Get-PublishedIdentity(
        [string]$LocalPath,
        [string]$PublishedPath,
        [string]$Description) {
    $identity = Get-FileIdentity $LocalPath $Description
    return [ordered]@{
        path = [IO.Path]::GetFullPath($PublishedPath)
        size = $identity.size
        sha256 = $identity.sha256
    }
}

function Copy-NewPublishedFile(
        [string]$Source,
        [string]$LocalDestination,
        [string]$PublishedDestination,
        [string]$Description) {
    if (Test-Path -LiteralPath $LocalDestination) {
        throw "$Description 目标已存在，拒绝覆盖：$LocalDestination"
    }
    Copy-Item -LiteralPath $Source -Destination $LocalDestination
    return Get-PublishedIdentity `
        $LocalDestination $PublishedDestination $Description
}

function Write-NewUtf8Text([string]$Path, [string]$Content) {
    if (Test-Path -LiteralPath $Path) {
        throw "文本发布目标已存在，拒绝覆盖：$Path"
    }
    [IO.File]::WriteAllText(
        $Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    Write-NewUtf8Text $Path (
        ($Value | ConvertTo-Json -Depth 24) + [Environment]::NewLine)
}

function Read-IniSection([string]$Path, [string]$SectionName) {
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
        if ($values.Contains($key)) {
            throw "config [$SectionName] 存在重复键：$key"
        }
        $values[$key] = $line.Substring($separator + 1).Trim()
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
        throw "config 缺少 $Name"
    }
    return [string]$Values[$Name]
}

function Get-RequiredInteger(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $text = Get-RequiredValue $Values $Name
    if ($text -notmatch '^[0-9]+$') {
        throw "config 的 $Name 不是非负整数"
    }
    return [int]$text
}

function Get-RequiredBoolean(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $text = (Get-RequiredValue $Values $Name).ToLowerInvariant()
    if ($text -eq "true") { return $true }
    if ($text -eq "false") { return $false }
    throw "config 的 $Name 不是布尔值"
}

function Parse-Roi([string]$Text, [string]$Name) {
    if ($Text -notmatch '^(\d+),(\d+),(\d+),(\d+)$') {
        throw "$Name 必须为 x,y,width,height"
    }
    $roi = [ordered]@{
        x = [int]$Matches[1]
        y = [int]$Matches[2]
        width = [int]$Matches[3]
        height = [int]$Matches[4]
    }
    if ($roi.width -le 1 -or $roi.height -le 1) {
        throw "$Name 尺寸不足"
    }
    return $roi
}

$sequenceSampleCount =
    4 * $ChallengePulseCount * $ChallengeStrideSampleCount +
    2 * $PeakHoldSampleCount +
    $SettleSampleCount + $BaselineSampleCount
if ($sequenceSampleCount -gt 2400 -or
    $sequenceSampleCount -ge $SidecarFrames) {
    throw "A2 S1 序列必须小于 sidecar frame 容量且不超过 2400 samples"
}

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "A2 S1 RunDirectory 已存在，拒绝覆盖：$resolvedRun"
}
foreach ($candidate in @($resolvedRun, $resolvedPublishedRun)) {
    if ([string]::Equals(
            $candidate, [IO.Path]::GetPathRoot($candidate),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "A2 S1 RunDirectory 不能是根目录"
    }
}
$runParent = Split-Path -Parent $resolvedRun
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedRun
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "A2 S1 staging 已存在：$stagingDirectory"
}
[void](New-Item -ItemType Directory -Path $stagingDirectory)
$toolDirectory = Join-Path $stagingDirectory "tool"
[void](New-Item -ItemType Directory -Path $toolDirectory)

$sourceConfig = Get-FileIdentity $ConfigPath "A2 S1 config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "A2 S1 OBS source binding"
$capture = Read-IniSection $sourceConfig.path "capture"
$mouse = Read-IniSection $sourceConfig.path "mouse"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouse "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouse "allow_send_input")) {
    throw "A2 S1 Prepare 要求精确 NDI/source clock、KMBOX NET 且默认输出关闭"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$leftRoi = Parse-Roi $LeftWitnessRoi "LeftWitnessRoi"
$rightRoi = Parse-Roi $RightWitnessRoi "RightWitnessRoi"
foreach ($roi in @($leftRoi, $rightRoi)) {
    if ($roi.x + $roi.width -gt $roiWidth -or
        $roi.y + $roi.height -gt $roiHeight) {
        throw "A2 S1 witness ROI 超出 Capture ROI"
    }
}
if ($leftRoi.x + $leftRoi.width -gt $rightRoi.x) {
    throw "A2 S1 左右 witness ROI 不得重叠"
}
$obsBinding = Get-Content -LiteralPath $sourceObsBinding.path `
    -Raw -Encoding utf8 | ConvertFrom-Json
$bindingOutputName = [string]$obsBinding.ndi_main_output.name
$ndiSourceMatchesBinding = [string]::Equals(
    $ndiSource, $bindingOutputName, [StringComparison]::Ordinal) -or
    $ndiSource.EndsWith(" ($bindingOutputName)")
if ([string]$obsBinding.evidence_type -ne "obs_source_binding" -or
    [bool]$obsBinding.physical_output_capability -or
    -not $ndiSourceMatchesBinding -or
    [int]$obsBinding.program_geometry.roi_width -ne $roiWidth -or
    [int]$obsBinding.program_geometry.roi_height -ne $roiHeight) {
    throw "A2 S1 OBS binding 与 config/ROI scope 不一致"
}

$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).ProviderPath
$publishedTool = Join-Path $resolvedPublishedRun "tool"
function Copy-Tool([string]$Name, [string]$Description) {
    return Copy-NewPublishedFile `
        (Join-Path $resolvedToolRoot $Name) `
        (Join-Path $toolDirectory $Name) `
        (Join-Path $publishedTool $Name) $Description
}
$probeExecutable = Copy-Tool `
    "XenMouseEffectProbe.exe" "A2 S1 probe executable"
$sidecarExecutable = Copy-Tool `
    "XenCaptureEvidence.exe" "A2 S1 sidecar executable"
$sequenceExecutable = Copy-Tool `
    "XenMouseEffectProbeSequence.exe" "A2 S1 sequence executable"
$opencvRuntime = Copy-Tool "opencv_world4140.dll" "A2 S1 OpenCV runtime"
$ndiRuntime = Copy-Tool `
    "Processing.NDI.Lib.x64.dll" "A2 S1 NDI runtime"
$ndiLicense = Copy-Tool `
    "Processing.NDI.Lib.Licenses.txt" "A2 S1 NDI license"
$launchScript = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $toolDirectory "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $publishedTool "launch_mouse_effect_probe_a.ps1") `
    "A2 S1 Launch script"
$calibrator = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "calibrate_mouse_effect_probe_a2.py") `
    (Join-Path $toolDirectory "calibrate_mouse_effect_probe_a2.py") `
    (Join-Path $publishedTool "calibrate_mouse_effect_probe_a2.py") `
    "A2 S1 dependency calibrator"
$configCopy = Copy-NewPublishedFile `
    $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
    (Join-Path $resolvedPublishedRun "config.ini") "A2 S1 config copy"
$obsCopy = Copy-NewPublishedFile `
    $sourceObsBinding.path `
    (Join-Path $stagingDirectory "obs-source-binding.json") `
    (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
    "A2 S1 OBS binding copy"

$sequencePath = Join-Path $stagingDirectory "sequence.json"
& (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
    --output $sequencePath `
    --profile s1-liveness-a2 `
    --run-role $RunRole `
    --challenge-pulses $ChallengePulseCount `
    --challenge-stride-samples $ChallengeStrideSampleCount `
    --peak-hold-samples $PeakHoldSampleCount `
    --settle-samples $SettleSampleCount `
    --baseline-samples $BaselineSampleCount
if ($LASTEXITCODE -ne 0) {
    throw "A2 S1 sequence tool 失败，ExitCode=$LASTEXITCODE"
}
$sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
    ConvertFrom-Json
$samples = @($sequence.samples)
$pulses = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
$expectedProfile = "dependency_calibration_a2_s1_$RunRole"
$expectedFirstDirections = if ($RunRole -eq "primary") {
    @(1, -1)
} else {
    @(-1, 1)
}
$actualFirstDirections = @($sequence.blocks | ForEach-Object {
    [int]$_.first_pulse_dx_counts
})
if ([int]$sequence.schema -ne 3 -or
    [string]$sequence.profile -ne $expectedProfile -or
    [uint64]$sequence.request.peak_hold_sample_count -ne
        $PeakHoldSampleCount -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne
        $ChallengePulseCount -or
    $samples.Count -ne $sequenceSampleCount -or
    $pulses.Count -ne 4 * $ChallengePulseCount -or
    @($samples | Where-Object { [int]$_.dy_counts -ne 0 }).Count -ne 0 -or
    @($samples | Where-Object {
        [int]$_.dx_counts -lt -1 -or [int]$_.dx_counts -gt 1
    }).Count -ne 0 -or
    ($actualFirstDirections -join ",") -ne
        ($expectedFirstDirections -join ",")) {
    throw "A2 S1 sequence 与 role/X-only/net/prefix 合同不一致"
}
$holdSamples = @($samples | Where-Object {
    [string]$_.phase -eq "hold"
})
if ($holdSamples.Count -ne 2 * $PeakHoldSampleCount -or
    @($holdSamples | Where-Object {
        [int]$_.dx_counts -ne 0 -or [int]$_.dy_counts -ne 0
    }).Count -ne 0) {
    throw "A2 S1 peak hold 不是两段预注册的连续零命令平台"
}
$baselineSamples = @($samples | Where-Object {
    [string]$_.phase -eq "baseline"
})
$settleSamples = @($samples | Where-Object {
    [string]$_.phase -eq "guard"
})
if ($baselineSamples.Count -ne $BaselineSampleCount -or
    $settleSamples.Count -ne $SettleSampleCount -or
    @($baselineSamples + $settleSamples | Where-Object {
        [int]$_.dx_counts -ne 0 -or [int]$_.dy_counts -ne 0
    }).Count -ne 0) {
    throw "A2 S1 settle/baseline 不是预注册的连续零命令窗口"
}
$sequenceIdentity = Get-PublishedIdentity `
    $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
    "A2 S1 sequence"

$scope = [ordered]@{
    schema_version = 2
    config_sha256 = $configCopy.sha256
    obs_source_binding_sha256 = $obsCopy.sha256
    probe_executable_sha256 = $probeExecutable.sha256
    sidecar_executable_sha256 = $sidecarExecutable.sha256
    opencv_runtime_sha256 = $opencvRuntime.sha256
    ndi_runtime_sha256 = $ndiRuntime.sha256
    ndi_source_name = $ndiSource
    frame_layout = Get-RequiredValue $capture "ndi_frame_layout"
    source_width = Get-RequiredInteger $capture "ndi_source_width"
    source_height = Get-RequiredInteger $capture "ndi_source_height"
    roi_width = $roiWidth
    roi_height = $roiHeight
    left_witness_roi = $LeftWitnessRoi
    right_witness_roi = $RightWitnessRoi
    challenge_pulse_count = $ChallengePulseCount
    challenge_stride_sample_count = $ChallengeStrideSampleCount
    peak_hold_sample_count = $PeakHoldSampleCount
    settle_sample_count = $SettleSampleCount
    baseline_sample_count = $BaselineSampleCount
    require_source_timing = $true
}
$scopeId = Get-TextSha256 (
    $scope | ConvertTo-Json -Depth 8 -Compress)

$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$bindingPath = Join-Path $stagingDirectory "probe-binding.json"
$binding = [ordered]@{
    schema_version = 3
    evidence_type = "mouse_effect_probe_binding"
    experiment = "physical_a2_s1_liveness_bracket"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = $expectedProfile
    run_role = $RunRole
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $configCopy
    obs_source_binding = $obsCopy
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 1
    max_abs_prefix_x_counts = $ChallengePulseCount
    challenge_and_settle_eligible_for_estimands = $false
    peak_hold_frames_eligible_for_estimands = $false
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-PublishedIdentity `
    $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
    "A2 S1 probe binding"

$task = [ordered]@{
    schema_version = 3
    evidence_type = "mouse_effect_probe_a2_s1_task"
    status = "PREPARED"
    experiment = "physical_a2_s1_liveness_bracket"
    run_role = $RunRole
    run_directory = $resolvedPublishedRun
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = $expectedProfile
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = [uint64]$samples.Count
    expected_nonzero_transition_count = [uint64]$pulses.Count
    physical_output_capability = $true
    physical_output_confirmation =
        "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
    files = [ordered]@{
        launch_script = $launchScript
        probe_executable = $probeExecutable
        sidecar_executable = $sidecarExecutable
        sequence_executable = $sequenceExecutable
        opencv_runtime = $opencvRuntime
        ndi_runtime = $ndiRuntime
        ndi_license = $ndiLicense
        dependency_calibrator = $calibrator
        config = $configCopy
        sequence = $sequenceIdentity
        probe_binding = $bindingIdentity
        obs_source_binding = $obsCopy
    }
    capture = [ordered]@{
        source_name = $ndiSource
        frame_layout = (Get-RequiredValue `
            $capture "ndi_frame_layout").ToLowerInvariant()
        source_width = Get-RequiredInteger $capture "ndi_source_width"
        source_height = Get-RequiredInteger $capture "ndi_source_height"
        roi_width = $roiWidth
        roi_height = $roiHeight
        center_roi = Get-RequiredBoolean $capture "center_roi"
        roi_x = Get-RequiredInteger $capture "roi_x"
        roi_y = Get-RequiredInteger $capture "roi_y"
        discovery_timeout_ms = Get-RequiredInteger `
            $capture "ndi_discovery_timeout_ms"
        receive_timeout_ms = Get-RequiredInteger `
            $capture "ndi_receive_timeout_ms"
        disconnect_timeout_ms = Get-RequiredInteger `
            $capture "ndi_disconnect_timeout_ms"
        clock_sync_url = Get-RequiredValue $capture "ndi_clock_sync_url"
        clock_sync_interval_ms = Get-RequiredInteger `
            $capture "ndi_clock_sync_interval_ms"
        clock_sync_timeout_ms = Get-RequiredInteger `
            $capture "ndi_clock_sync_timeout_ms"
        clock_mapping_max_age_ms = Get-RequiredInteger `
            $capture "ndi_clock_mapping_max_age_ms"
        require_frame_metadata = Get-RequiredBoolean `
            $capture "ndi_require_frame_metadata"
    }
    sidecar = [ordered]@{
        frames = $SidecarFrames
        max_seconds = $MaxSeconds
        # PNG/双哈希 publishing 与采集窗分离；继续使用正式 Aim
        # sidecar 的 60 秒原子发布终局。
        publishing_max_seconds = 60
        physical_output_capability = $false
        left_witness_roi = $LeftWitnessRoi
        right_witness_roi = $RightWitnessRoi
        require_source_timing = $true
    }
    liveness_policy = [ordered]@{
        policy_id = "a2-s1-kmbox-bracket-peak-hold-v1"
        challenge_pulse_count = $ChallengePulseCount
        challenge_stride_sample_count = $ChallengeStrideSampleCount
        peak_hold_sample_count = $PeakHoldSampleCount
        settle_sample_count = $SettleSampleCount
        baseline_frame_count = $BaselineSampleCount
        challenge_frames_eligible_for_estimands = $false
        peak_hold_frames_eligible_for_estimands = $false
        settle_frames_eligible_for_estimands = $false
        fixed_pixel_speed_used_as_gate = $false
    }
    safety = [ordered]@{
        normal_aim_must_be_closed = $true
        emergency_virtual_keys = @(35, 119)
        right_button_deadman_required = $true
        any_failure_stops_without_compensation = $true
        zero_y_required = $true
        max_abs_prefix_x_counts = $ChallengePulseCount
        manual_mouse_motion_or_wasd_forbidden = $true
        no_runtime_amplitude_or_repetition_change = $true
        no_runtime_peak_hold_change = $true
    }
}
Write-NewUtf8Json (Join-Path $stagingDirectory "task.json") $task

$launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
    '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
    '-PhysicalOutputConfirmation {2}') -f
    $launchScript.path, $resolvedPublishedRun,
    "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
$roleLabel = if ($RunRole -eq "primary") { "PRIMARY" } else { "VALIDATION" }
$taskMarkdown = @(
    "# Mouse Effect Probe Physical A2 S1 $roleLabel",
    "",
    "- Run UUID：``$runUuid``；scope：``$scopeId``",
    "- 自动挑战：每 $ChallengeStrideSampleCount 个 source frame 发送 1 count，单向累计 $ChallengePulseCount counts；前后挑战均回锚，整段净 X=0、Y=0",
    "- 峰值停留=$PeakHoldSampleCount source samples：到达原 $ChallengePulseCount-count 峰值后只消费 source frame、不发送 Mouse command；实际毫秒数以后生 source timestamp 为准",
    "- 零基线：settle=$SettleSampleCount samples 后连续 baseline=$BaselineSampleCount samples；challenge/hold/settle 全部排除",
    "- 固定 command cadence 只作 decoded-image 活性正控制，不作为像素速度阈值，也不进入 noise/tail/gain/resolution",
    "- 启动命令时保持右键松开；看到 probe 提示 monitor 已就绪后，在 5 秒内按住右键并持续保持，直到出现 probe 时间线完成或未正常完成的终局提示后再松开；sidecar publishing 不是松键信号",
    "- 除右键 deadman 外，不要移动物理鼠标、不要按 WASD；视角变化由 KMBOX 序列自动完成",
    "- 松开右键、End、F8、sidecar/ACK/source 任一失败都会立即停发且不补偿",
    "",
    "下面命令会发送真实 KMBOX 输入，只能由用户在本机前台执行：",
    "",
    "``````powershell",
    $launchCommand,
    "``````",
    "") -join [Environment]::NewLine
Write-NewUtf8Text (Join-Path $stagingDirectory "TASK.md") $taskMarkdown

$observation = @(
    "# Physical A2 S1 现场观察",
    "",
    "- Run UUID：``$runUuid``",
    "- Run role：``$RunRole``",
    "- 是否全程未移动物理鼠标/未按 WASD：",
    "- 自动视角变化是否可见：",
    "- 是否发生异常或急停：",
    "- 用户原话：",
    "") -join [Environment]::NewLine
Write-NewUtf8Text `
    (Join-Path $stagingDirectory "OBSERVATION.md") $observation

$summary = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_s1_prepare"
    status = "PREPARED_NOT_LAUNCHED"
    run_uuid = $runUuid
    run_role = $RunRole
    profile = $expectedProfile
    scope_id = $scopeId
    local_bundle_directory = $resolvedRun
    published_run_directory = $resolvedPublishedRun
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = [uint64]$samples.Count
    expected_nonzero_transition_count = [uint64]$pulses.Count
    peak_hold_sample_count = $PeakHoldSampleCount
    physical_launch_executed = $false
}
Write-NewUtf8Json `
    (Join-Path $stagingDirectory "prepare-summary.json") $summary

Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
Write-Host "Physical A2 S1 $roleLabel Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
Write-Host "BundleDirectory=$resolvedRun"
Write-Host "PublishedRunDirectory=$resolvedPublishedRun"
Write-Host $launchCommand
