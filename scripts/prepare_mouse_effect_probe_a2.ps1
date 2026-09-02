param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$SyntheticCalibrationPath,
    [Parameter(Mandatory = $true)]
    [string]$ZeroInputCalibrationPath,
    [Parameter(Mandatory = $true)]
    [string]$CalibrationPlanPath,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [Parameter(Mandatory = $true)]
    [ValidateSet("p-cal", "p-holdout")]
    [string]$RunRole
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
    $value = Get-RequiredValue $Values $Name
    if ($value -notmatch '^[0-9]+$') {
        throw "config 的 $Name 不是非负整数"
    }
    return [int]$value
}

function Get-RequiredBoolean(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $value = (Get-RequiredValue $Values $Name).ToLowerInvariant()
    if ($value -eq "true") { return $true }
    if ($value -eq "false") { return $false }
    throw "config 的 $Name 不是布尔值"
}

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "A2 RunDirectory 已存在，拒绝覆盖：$resolvedRun"
}
if ([string]::Equals(
        $resolvedRun, [IO.Path]::GetPathRoot($resolvedRun),
        [StringComparison]::OrdinalIgnoreCase) -or
    [string]::Equals(
        $resolvedPublishedRun, [IO.Path]::GetPathRoot($resolvedPublishedRun),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "A2 RunDirectory/PublishedRunDirectory 不能是根目录"
}
$runParent = Split-Path -Parent $resolvedRun
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedRun
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "A2 staging 已存在：$stagingDirectory"
}
[void](New-Item -ItemType Directory -Path $stagingDirectory)
$toolDirectory = Join-Path $stagingDirectory "tool"
$calibrationDirectory = Join-Path $stagingDirectory "calibration"
[void](New-Item -ItemType Directory -Path $toolDirectory)
[void](New-Item -ItemType Directory -Path $calibrationDirectory)

$s0Source = Get-FileIdentity `
    $SyntheticCalibrationPath "A2 S0 synthetic calibration"
$s1Source = Get-FileIdentity `
    $ZeroInputCalibrationPath "A2 S1 zero-input calibration"
$planSource = Get-FileIdentity `
    $CalibrationPlanPath "A2 dependency calibration plan"
$s0 = Get-Content -LiteralPath $s0Source.path -Raw -Encoding utf8 |
    ConvertFrom-Json
$s1 = Get-Content -LiteralPath $s1Source.path -Raw -Encoding utf8 |
    ConvertFrom-Json
$plan = Get-Content -LiteralPath $planSource.path -Raw -Encoding utf8 |
    ConvertFrom-Json
$s1ContinuousZero =
    -not [bool]$s1.probe_started -and
    -not [bool]$s1.mouse_opened -and
    [bool]$s1.actual_command_zero
$s1MeasurementState = if (
        $s1.PSObject.Properties.Name -contains "measurement_state") {
    [string]$s1.measurement_state
} else {
    ""
}
$s1BracketedCensoredZero =
    $s1MeasurementState -eq "VALID_BRACKETED_CENSORED_ZERO" -and
    [bool]$s1.probe_started -and
    [bool]$s1.mouse_opened -and
    [bool]$s1.physical_challenge_executed -and
    [bool]$s1.baseline_actual_command_zero -and
    -not [bool]$s1.actual_command_zero
if ([string]$s0.evidence_type -ne
        "mouse_effect_probe_a2_s0_synthetic_calibration" -or
    [string]$s0.status -ne "VALID" -or
    [bool]$s0.physical_output_capability -or
    [string]$s1.evidence_type -ne
        "mouse_effect_probe_a2_s1_zero_input_calibration" -or
    [string]$s1.status -ne "VALID" -or
    [bool]$s1.physical_output_capability -or
    (-not $s1ContinuousZero -and -not $s1BracketedCensoredZero) -or
    [string]$plan.evidence_type -ne
        "mouse_effect_probe_a2_dependency_calibration_plan" -or
    [string]$plan.status -ne "VALID_OFFLINE_PLAN" -or
    [bool]$plan.physical_output_capability -or
    [bool]$plan.physical_launch_authorized -or
    [string]$plan.scope_id -ne [string]$s1.scope_id) {
    throw "A2 Prepare 要求同 scope 的 VALID S0/S1/离线 plan"
}
$request = $plan.sequence_request
if ([uint64]$request.baseline_sample_count -eq 0 -or
    [uint64]$request.response_sample_count -eq 0 -or
    [uint64]$request.guard_sample_count -eq 0 -or
    [uint64]$request.block_count -lt 4 -or
    [uint64]$request.block_count % 4 -ne 0 -or
    [int64]$request.net_x_counts -ne 0 -or
    [uint64]$request.max_abs_prefix_x_counts -ne 1 -or
    [int]$request.dy_counts_required -ne 0 -or
    [uint64]$plan.sidecar.frame_count -gt 2400) {
    throw "A2 plan 的 sequence/sidecar 安全合同无效"
}
$expectedProfile = if ($RunRole -eq "p-cal") {
    "dependency_calibration_a2_p_cal"
} else {
    "dependency_calibration_a2_p_holdout"
}
$planRoleKey = $RunRole.Replace("-", "_")
$planRoleProperty = $plan.roles.PSObject.Properties[$planRoleKey]
if ($null -eq $planRoleProperty -or
    [string]$planRoleProperty.Value.profile -ne $expectedProfile) {
    throw "A2 plan 与 RunRole/profile 不一致"
}

$sourceConfig = Get-FileIdentity $ConfigPath "A2 config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "A2 OBS source binding"
$capture = Read-IniSection $sourceConfig.path "capture"
$mouse = Read-IniSection $sourceConfig.path "mouse"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouse "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouse "allow_send_input")) {
    throw "A2 Prepare 要求精确 NDI/source clock、KMBOX NET 且默认输出关闭"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$leftRoi = @($s1.geometry.left_roi)
$rightRoi = @($s1.geometry.right_roi)
if ($leftRoi.Count -ne 4 -or $rightRoi.Count -ne 4 -or
    [int]$leftRoi[0] + [int]$leftRoi[2] -gt $roiWidth -or
    [int]$rightRoi[0] + [int]$rightRoi[2] -gt $roiWidth -or
    [int]$leftRoi[1] + [int]$leftRoi[3] -gt $roiHeight -or
    [int]$rightRoi[1] + [int]$rightRoi[3] -gt $roiHeight) {
    throw "A2 S1 witness ROI 与 config 几何不一致"
}

$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).ProviderPath
$publishedTool = Join-Path $resolvedPublishedRun "tool"
$publishedCalibration = Join-Path $resolvedPublishedRun "calibration"
function Copy-Tool([string]$Name, [string]$Description) {
    return Copy-NewPublishedFile `
        (Join-Path $resolvedToolRoot $Name) `
        (Join-Path $toolDirectory $Name) `
        (Join-Path $publishedTool $Name) $Description
}
$probeExecutable = Copy-Tool `
    "XenMouseEffectProbe.exe" "A2 probe executable"
$sidecarExecutable = Copy-Tool `
    "XenCaptureEvidence.exe" "A2 sidecar executable"
$sequenceExecutable = Copy-Tool `
    "XenMouseEffectProbeSequence.exe" "A2 sequence executable"
$opencvRuntime = Copy-Tool "opencv_world4140.dll" "A2 OpenCV runtime"
$ndiRuntime = Copy-Tool `
    "Processing.NDI.Lib.x64.dll" "A2 NDI runtime"
$ndiLicense = Copy-Tool `
    "Processing.NDI.Lib.Licenses.txt" "A2 NDI license"
$launchScript = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $toolDirectory "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $publishedTool "launch_mouse_effect_probe_a.ps1") `
    "A2 Launch script"
$physicalAnalyzer = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "analyze_mouse_effect_probe_pixels.py") `
    (Join-Path $toolDirectory "analyze_mouse_effect_probe_pixels.py") `
    (Join-Path $publishedTool "analyze_mouse_effect_probe_pixels.py") `
    "A2 physical analyzer"
$calibrator = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "calibrate_mouse_effect_probe_a2.py") `
    (Join-Path $toolDirectory "calibrate_mouse_effect_probe_a2.py") `
    (Join-Path $publishedTool "calibrate_mouse_effect_probe_a2.py") `
    "A2 dependency calibrator"
$configCopy = Copy-NewPublishedFile `
    $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
    (Join-Path $resolvedPublishedRun "config.ini") "A2 config copy"
$obsCopy = Copy-NewPublishedFile `
    $sourceObsBinding.path `
    (Join-Path $stagingDirectory "obs-source-binding.json") `
    (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
    "A2 OBS binding copy"
$s0Copy = Copy-NewPublishedFile `
    $s0Source.path (Join-Path $calibrationDirectory "s0.json") `
    (Join-Path $publishedCalibration "s0.json") "A2 S0 copy"
$s1Copy = Copy-NewPublishedFile `
    $s1Source.path (Join-Path $calibrationDirectory "s1.json") `
    (Join-Path $publishedCalibration "s1.json") "A2 S1 copy"
$planCopy = Copy-NewPublishedFile `
    $planSource.path (Join-Path $calibrationDirectory "plan.json") `
    (Join-Path $publishedCalibration "plan.json") "A2 plan copy"

$sequencePath = Join-Path $stagingDirectory "sequence.json"
& (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
    --output $sequencePath `
    --baseline-samples ([uint64]$request.baseline_sample_count) `
    --response-samples ([uint64]$request.response_sample_count) `
    --guard-samples ([uint64]$request.guard_sample_count) `
    --profile dependency-calibration-a2 `
    --blocks ([uint64]$request.block_count) `
    --run-role $RunRole
if ($LASTEXITCODE -ne 0) {
    throw "A2 sequence tool 失败，ExitCode=$LASTEXITCODE"
}
$sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
    ConvertFrom-Json
$samples = @($sequence.samples)
$pulses = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
$expectedDirections = if ($RunRole -eq "p-cal") {
    @(1, -1, -1, 1)
} else {
    @(-1, 1, 1, -1)
}
$actualFirstDirections = @($sequence.blocks | ForEach-Object {
    [int]$_.first_pulse_dx_counts
})
if ([int]$sequence.schema -ne 2 -or
    [string]$sequence.profile -ne $expectedProfile -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
    $samples.Count -ne [uint64]$request.sample_count -or
    $pulses.Count -ne [uint64]$request.nonzero_transition_count -or
    ($actualFirstDirections -join ",") -ne ($expectedDirections -join ",")) {
    throw "A2 sequence 与 plan/role/X-only/net/prefix 合同不一致"
}
$sequenceIdentity = Get-PublishedIdentity `
    $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
    "A2 sequence"

$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$bindingPath = Join-Path $stagingDirectory "probe-binding.json"
$binding = [ordered]@{
    schema_version = 2
    evidence_type = "mouse_effect_probe_binding"
    experiment = "physical_a2_dependency_calibration"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = $expectedProfile
    run_role = $RunRole
    scope_id = [string]$plan.scope_id
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $configCopy
    obs_source_binding = $obsCopy
    calibration = [ordered]@{
        synthetic = $s0Copy
        zero_input = $s1Copy
        plan = $planCopy
    }
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 1
    max_abs_prefix_x_counts = 1
    expected_background_shift_sign_per_positive_count = -1
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-PublishedIdentity `
    $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
    "A2 probe binding"

$taskPath = Join-Path $stagingDirectory "task.json"
$task = [ordered]@{
    schema_version = 2
    evidence_type = "mouse_effect_probe_a2_task"
    status = "PREPARED"
    experiment = "physical_a2_dependency_calibration"
    run_role = $RunRole
    run_directory = $resolvedPublishedRun
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = $expectedProfile
    scope_id = [string]$plan.scope_id
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
        physical_analyzer = $physicalAnalyzer
        dependency_calibrator = $calibrator
        config = $configCopy
        sequence = $sequenceIdentity
        probe_binding = $bindingIdentity
        obs_source_binding = $obsCopy
        synthetic_calibration = $s0Copy
        zero_input_calibration = $s1Copy
        calibration_plan = $planCopy
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
        frames = [uint64]$plan.sidecar.frame_count
        max_seconds = [uint64]$plan.sidecar.max_seconds
        physical_output_capability = $false
        capture_source_name = $ndiSource
        frame_layout = Get-RequiredValue $capture "ndi_frame_layout"
        source_width = Get-RequiredInteger $capture "ndi_source_width"
        source_height = Get-RequiredInteger $capture "ndi_source_height"
        roi_width = $roiWidth
        roi_height = $roiHeight
        left_witness_roi = ($leftRoi -join ",")
        right_witness_roi = ($rightRoi -join ",")
        require_source_timing = $true
    }
    safety = [ordered]@{
        normal_aim_must_be_closed = $true
        emergency_virtual_keys = @(35, 119)
        any_failure_stops_without_compensation = $true
        zero_y_required = $true
        max_abs_prefix_x_counts = 1
        no_runtime_amplitude_or_repetition_change = $true
        p_holdout_never_used_for_retuning = $true
    }
    unresolved_until_user_physical = @(
        "INDEPENDENT_TAIL_SUPPORT",
        "WITNESS_OCCLUSION_MARGIN",
        "MAPPING_UNCERTAINTY_PX",
        "SINGLE_COUNT_GAIN_UPPER_SCOPE"
    )
}
Write-NewUtf8Json $taskPath $task

$launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
    '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
    '-PhysicalOutputConfirmation {2}') -f
    $launchScript.path, $resolvedPublishedRun,
    "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
$roleLabel = if ($RunRole -eq "p-cal") { "P-CAL" } else { "P-HOLDOUT" }
$taskMarkdown = @(
    "# Mouse Effect Probe Physical A2 $roleLabel",
    "",
    "- Run UUID：``$runUuid``",
    "- scope：``$($plan.scope_id)``；profile：``$expectedProfile``",
    "- 序列：samples=$($samples.Count)，transitions=$($pulses.Count)，每个 block 均为 pre-zero → ±1 → hold → return → post-zero",
    "- 硬边界：X-only、Y=0、净 X=0、最大前缀=1 count；运行期不加幅、不加测、不补偿",
    "- 场景：真实游戏静止高纹理背景；左右 witness 全支持域不得含人物、HUD、Overlay、遮挡或独立动画",
    "- 安全：启动命令时右键松开；看到 probe 提示 monitor 已就绪后在 5 秒内按住并持续保持，直到出现 probe 时间线完成或未正常完成的终局提示后再松开；sidecar publishing 不是松键信号；松键、End、F8 或任何失败立即停发",
    "- P-HOLDOUT 不得用于回调 nuisance/noise/tail/mask/mapping/gain；失败即 A2 red",
    "",
    "下面命令会发送真实 KMBOX 输入，只能由用户在本机前台执行：",
    "",
    "``````powershell",
    $launchCommand,
    "``````",
    "",
    "执行后请直接回传可见方向、左右一致性、遮挡/异常/急停与完成状态；不要自行编辑观察文件。",
    "") -join [Environment]::NewLine
Write-NewUtf8Text (Join-Path $stagingDirectory "TASK.md") $taskMarkdown
$observation = @(
    "# Physical A2 人工观察",
    "",
    "- Run UUID：``$runUuid``",
    "- Run role：``$RunRole``",
    "- 用户原话：",
    "- 正/负方向：",
    "- 左右 witness 一致性：",
    "- 遮挡/scene cut：",
    "- 异常/急停：",
    "- 人工结论：",
    "") -join [Environment]::NewLine
Write-NewUtf8Text `
    (Join-Path $stagingDirectory "OBSERVATION.md") $observation
$summary = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_prepare"
    status = "PREPARED_NOT_LAUNCHED"
    run_uuid = $runUuid
    run_role = $RunRole
    profile = $expectedProfile
    scope_id = [string]$plan.scope_id
    local_bundle_directory = $resolvedRun
    published_run_directory = $resolvedPublishedRun
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = [uint64]$samples.Count
    expected_nonzero_transition_count = [uint64]$pulses.Count
    physical_launch_executed = $false
}
Write-NewUtf8Json `
    (Join-Path $stagingDirectory "prepare-summary.json") $summary

Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
Write-Host "Physical A2 $roleLabel Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
Write-Host "BundleDirectory=$resolvedRun"
Write-Host "PublishedRunDirectory=$resolvedPublishedRun"
Write-Host $launchCommand
