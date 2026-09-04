param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsLogPath,
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [ValidateRange(296, 2400)]
    [uint64]$SidecarFrames = 2400,
    [ValidateRange(8, 60)]
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
        ($Value | ConvertTo-Json -Depth 32) + [Environment]::NewLine)
}

function Read-IniSection([string]$Path, [string]$SectionName) {
    $values = [ordered]@{}
    $current = ""
    foreach ($rawLine in [IO.File]::ReadAllLines($Path)) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(";") -or
            $line.StartsWith("#")) { continue }
        if ($line -match '^\[(.+)\]$') {
            $current = $Matches[1].Trim()
            continue
        }
        if (-not [string]::Equals(
                $current, $SectionName,
                [StringComparison]::OrdinalIgnoreCase)) { continue }
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
    if ($values.Count -eq 0) { throw "config 缺少 [$SectionName]" }
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

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "Physical B composite-phase RunDirectory 已存在：$resolvedRun"
}
foreach ($candidate in @($resolvedRun, $resolvedPublishedRun)) {
    if ([string]::Equals(
            $candidate, [IO.Path]::GetPathRoot($candidate),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Physical B composite-phase RunDirectory 不能是根目录"
    }
}

$sourceConfig = Get-FileIdentity $ConfigPath "composite-phase config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "composite-phase OBS source binding"
$sourceObsLog = Get-FileIdentity $ObsLogPath "composite-phase OBS log"
$python = Get-FileIdentity $PythonExecutable "Python executable"
$capture = Read-IniSection $sourceConfig.path "capture"
$mouse = Read-IniSection $sourceConfig.path "mouse"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouse "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouse "allow_send_input")) {
    throw "Prepare 要求精确 NDI/source clock、KMBOX NET 且默认输出关闭"
}

$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$sourceWidth = Get-RequiredInteger $capture "ndi_source_width"
$sourceHeight = Get-RequiredInteger $capture "ndi_source_height"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$centerRoi = Get-RequiredBoolean $capture "center_roi"
$configuredRoiX = Get-RequiredInteger $capture "roi_x"
$configuredRoiY = Get-RequiredInteger $capture "roi_y"
$actualRoiX = if ($centerRoi) { [int](($sourceWidth - $roiWidth) / 2) } else {
    $configuredRoiX
}
$actualRoiY = if ($centerRoi) { [int](($sourceHeight - $roiHeight) / 2) } else {
    $configuredRoiY
}
$leftRoi = Parse-Roi $LeftWitnessRoi "LeftWitnessRoi"
$rightRoi = Parse-Roi $RightWitnessRoi "RightWitnessRoi"
foreach ($roi in @($leftRoi, $rightRoi)) {
    if ($roi.x + $roi.width -gt $roiWidth -or
        $roi.y + $roi.height -gt $roiHeight) {
        throw "composite-phase witness ROI 超出 Capture ROI"
    }
}
if ($leftRoi.x + $leftRoi.width -gt $rightRoi.x) {
    throw "composite-phase 左右 witness ROI 不得重叠"
}

$obsBinding = Get-Content -LiteralPath $sourceObsBinding.path `
    -Raw -Encoding utf8 | ConvertFrom-Json
$bindingOutputName = [string]$obsBinding.ndi_main_output.name
$ndiSourceMatchesBinding = [string]::Equals(
    $ndiSource, $bindingOutputName, [StringComparison]::Ordinal) -or
    $ndiSource.EndsWith(" ($bindingOutputName)")
if ([int]$obsBinding.schema_version -ne 2 -or
    [string]$obsBinding.evidence_type -ne "obs_source_binding" -or
    [string]$obsBinding.binding_mode -ne "real_game" -or
    [bool]$obsBinding.physical_output_capability -or
    [string]$obsBinding.state_basis -ne "obs_saved_scene_collection" -or
    -not [bool]$obsBinding.ndi_main_output.enabled -or
    -not $ndiSourceMatchesBinding -or
    [string]$obsBinding.program_geometry.mapping -ne
        "monitor_crop_filter_1_to_1" -or
    [int]$obsBinding.program_geometry.source_width -ne $sourceWidth -or
    [int]$obsBinding.program_geometry.source_height -ne $sourceHeight -or
    [int]$obsBinding.program_geometry.roi_width -ne $roiWidth -or
    [int]$obsBinding.program_geometry.roi_height -ne $roiHeight -or
    [int]$obsBinding.program_geometry.roi_x -ne $actualRoiX -or
    [int]$obsBinding.program_geometry.roi_y -ne $actualRoiY -or
    [string]$obsBinding.selected_source.id -ne "monitor_capture" -or
    [bool]$obsBinding.selected_source.capture_cursor -or
    -not [bool]$obsBinding.selected_source.crop_filter.enabled -or
    [bool]$obsBinding.selected_source.crop_filter.settings.relative) {
    throw "composite-phase OBS binding 与当前 NDI/ROI scope 不一致"
}
foreach ($live in @(
        [pscustomobject]@{
            path = [string]$obsBinding.scene_collection
            sha = [string]$obsBinding.scene_collection_sha256
            name = "OBS scene collection"
        },
        [pscustomobject]@{
            path = [string]$obsBinding.obs_profile_config.path
            sha = [string]$obsBinding.obs_profile_config.sha256
            name = "OBS profile"
        })) {
    if (-not (Test-Path -LiteralPath $live.path -PathType Leaf) -or
        (Get-FileSha256 $live.path) -ne $live.sha) {
        throw "$($live.name) 已相对 OBS binding 漂移"
    }
}
$profileVideo = Read-IniSection `
    ([string]$obsBinding.obs_profile_config.path) "Video"
$fpsNumerator = Get-RequiredInteger $profileVideo "fpsnum"
$fpsDenominator = Get-RequiredInteger $profileVideo "fpsden"
if ($fpsNumerator -ne 240 -or $fpsDenominator -ne 1) {
    throw "composite-phase 预注册 source rate 必须是当前 OBS 240/1"
}
$minimumSidecarFrames = [uint64]([Math]::Ceiling(
        6.0 * [double]$fpsNumerator / [double]$fpsDenominator)) + 295
if ($SidecarFrames -lt $minimumSidecarFrames) {
    throw ("composite-phase sidecar 帧数不足以覆盖 5 秒武装、295 帧采集" +
        "与 1 秒余量：至少 $minimumSidecarFrames")
}
$obsLogText = [IO.File]::ReadAllText($sourceObsLog.path)
$versionMatch = [regex]::Match(
    $obsLogText,
    '(?m)^\d\d:\d\d:\d\d\.\d+: OBS ([^ ]+) \(64-bit, windows\)\s*$')
$sourceName = [string]$obsBinding.selected_source.name
$sourceMarker = "[duplicator-monitor-capture: '$sourceName'] update settings:"
$methodMatch = [regex]::Match(
    $obsLogText, '(?m)^.*?method:\s*(DXGI|WGC)\s*$')
if (-not $versionMatch.Success -or
    -not $obsLogText.Contains($sourceMarker) -or
    -not $methodMatch.Success) {
    throw "OBS log 未证明选定 monitor source 的版本/实际 capture method"
}
$obsVersion = $versionMatch.Groups[1].Value
$resolvedCaptureMethod = $methodMatch.Groups[1].Value

$runParent = Split-Path -Parent $resolvedRun
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedRun
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "composite-phase staging 已存在：$stagingDirectory"
}
[void](New-Item -ItemType Directory -Path $stagingDirectory)
$toolDirectory = Join-Path $stagingDirectory "tool"
$inputDirectory = Join-Path $stagingDirectory "inputs"
[void](New-Item -ItemType Directory -Path $toolDirectory)
[void](New-Item -ItemType Directory -Path $inputDirectory)

try {
$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).ProviderPath
$publishedTool = Join-Path $resolvedPublishedRun "tool"
$publishedInputs = Join-Path $resolvedPublishedRun "inputs"
function Copy-Tool([string]$Name, [string]$Description) {
    return Copy-NewPublishedFile `
        (Join-Path $resolvedToolRoot $Name) `
        (Join-Path $toolDirectory $Name) `
        (Join-Path $publishedTool $Name) $Description
}
function Copy-Script([string]$Name, [string]$Description) {
    return Copy-NewPublishedFile `
        (Join-Path $PSScriptRoot $Name) `
        (Join-Path $toolDirectory $Name) `
        (Join-Path $publishedTool $Name) $Description
}
$probeExecutable = Copy-Tool `
    "XenMouseEffectProbe.exe" "composite-phase probe executable"
$sidecarExecutable = Copy-Tool `
    "XenCaptureEvidence.exe" "composite-phase sidecar executable"
$sequenceExecutable = Copy-Tool `
    "XenMouseEffectProbeSequence.exe" "composite-phase sequence executable"
$sealExecutable = Copy-Tool `
    "XenMouseEffectProbeCompositeSeal.exe" "composite-phase seal executable"
$opencvRuntime = Copy-Tool `
    "opencv_world4140.dll" "composite-phase OpenCV runtime"
$ndiRuntime = Copy-Tool `
    "Processing.NDI.Lib.x64.dll" "composite-phase NDI runtime"
$ndiLicense = Copy-Tool `
    "Processing.NDI.Lib.Licenses.txt" "composite-phase NDI license"
$launchScript = Copy-Script `
    "launch_mouse_effect_probe_a.ps1" "composite-phase Launch script"
$planGenerator = Copy-Script `
    "freeze_mouse_effect_probe_b_composite_phase_plan.py" `
    "composite-phase plan generator"
$ledgerProducer = Copy-Script `
    "produce_mouse_effect_probe_b_composite_phase_ledgers.py" `
    "composite-phase ledger producer"
$binder = Copy-Script `
    "bind_mouse_effect_probe_b_composite_phase_calibration.py" `
    "composite-phase binder"
$evaluator = Copy-Script `
    "evaluate_mouse_effect_probe_b_composite_phase.py" `
    "composite-phase evaluator"
$configCopy = Copy-NewPublishedFile `
    $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
    (Join-Path $resolvedPublishedRun "config.ini") "config copy"
$obsCopy = Copy-NewPublishedFile `
    $sourceObsBinding.path `
    (Join-Path $stagingDirectory "obs-source-binding.json") `
    (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
    "OBS binding copy"
$obsLogCopy = Copy-NewPublishedFile `
    $sourceObsLog.path (Join-Path $inputDirectory "obs-capture.log") `
    (Join-Path $publishedInputs "obs-capture.log") "OBS log copy"

$sequencePath = Join-Path $stagingDirectory "sequence.json"
& (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
    --output $sequencePath `
    --profile physical-b-composite-phase-calibration
if ($LASTEXITCODE -ne 0) {
    throw "composite-phase sequence tool 失败，ExitCode=$LASTEXITCODE"
}
$sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
    ConvertFrom-Json
$samples = @($sequence.samples)
$windows = @($sequence.windows)
$pulses = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
if ([int]$sequence.schema -ne 7 -or
    [string]$sequence.profile -ne
        "physical_b_composite_phase_calibration" -or
    $samples.Count -ne 295 -or $windows.Count -ne 42 -or
    $pulses.Count -ne 38 -or
    @($windows | Where-Object { [bool]$_.negative_control }).Count -ne 4 -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
    [uint64]$sequence.request.predictor_sample_count -ne 1 -or
    [uint64]$sequence.request.window_sample_count -ne 6 -or
    [uint64]$sequence.request.single_magnitude_counts -ne 1 -or
    [string]$sequence.request.timer_mode -ne
        "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL" -or
    @($samples | Where-Object {
        [int]$_.dy_counts -ne 0 -or [math]::Abs([int]$_.dx_counts) -gt 1
    }).Count -ne 0) {
    throw "composite-phase sequence 与冻结 295/42/38/4/X-only 合同不一致"
}
$sequenceIdentity = Get-PublishedIdentity `
    $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
    "composite-phase sequence"

# 字段按字典序构造，使 Windows PowerShell 的 compact JSON 与 Python
# generator 的 canonical JSON 具有相同字节语义。
$capturePolicy = [ordered]@{
    boundary_claim_limit = "use_exact_selected_semantic_only"
    boundary_is_capture_or_exposure = $false
    boundary_semantic = "NDI_SDK_SUBMISSION_UTC"
    capture_stack = [ordered]@{
        capture_method_configured = "AUTO"
        capture_method_resolved = $resolvedCaptureMethod
        producer = "OBS"
        producer_log_sha256 = $obsLogCopy.sha256
        producer_version = $obsVersion
    }
    clock_mapping_policy_id = "ndi-frame-timing-source-clock-v1"
    left_witness_roi = @(
        $leftRoi.x, $leftRoi.y, $leftRoi.width, $leftRoi.height)
    ndi_frame_sync_used = $false
    pixel_format = "CPU_BGR"
    right_witness_roi = @(
        $rightRoi.x, $rightRoi.y, $rightRoi.width, $rightRoi.height)
    roi_geometry = @($actualRoiX, $actualRoiY, $roiWidth, $roiHeight)
    scene_binding_sha256 = $obsCopy.sha256
    source_geometry = @($sourceWidth, $sourceHeight)
    source_name = $ndiSource
    source_rate = [ordered]@{
        denominator = $fpsDenominator
        numerator = $fpsNumerator
    }
}
$capturePolicy.semantic_sha256 = Get-TextSha256 (
    $capturePolicy | ConvertTo-Json -Depth 16 -Compress)
$capturePolicyPath = Join-Path $stagingDirectory "capture-policy.json"
Write-NewUtf8Json $capturePolicyPath $capturePolicy
$capturePolicyIdentity = Get-PublishedIdentity `
    $capturePolicyPath `
    (Join-Path $resolvedPublishedRun "capture-policy.json") `
    "composite-phase capture policy"

$scope = [ordered]@{
    capture_policy_sha256 = $capturePolicyIdentity.sha256
    config_sha256 = $configCopy.sha256
    evaluator_sha256 = $evaluator.sha256
    ledger_producer_sha256 = $ledgerProducer.sha256
    obs_log_sha256 = $obsLogCopy.sha256
    obs_source_binding_sha256 = $obsCopy.sha256
    plan_generator_sha256 = $planGenerator.sha256
    sequence_file_sha256 = $sequenceIdentity.sha256
    sequence_semantic_sha256 = [string]$sequence.sequence_sha256
}
$scopeId = Get-TextSha256 ($scope | ConvertTo-Json -Depth 8 -Compress)
$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$planSeedPath = Join-Path $stagingDirectory "composite-phase-plan-seed.json"
& $python.path (Join-Path $toolDirectory `
        "freeze_mouse_effect_probe_b_composite_phase_plan.py") `
    --sequence $sequencePath `
    --capture-policy $capturePolicyPath `
    --binder (Join-Path $toolDirectory `
        "bind_mouse_effect_probe_b_composite_phase_calibration.py") `
    --evaluator (Join-Path $toolDirectory `
        "evaluate_mouse_effect_probe_b_composite_phase.py") `
    --producer (Join-Path $toolDirectory `
        "produce_mouse_effect_probe_b_composite_phase_ledgers.py") `
    --report-verifier (Join-Path $toolDirectory `
        "XenMouseEffectProbeCompositeSeal.exe") `
    --run-uuid $runUuid `
    --activation-epoch $activationEpoch `
    --scope-id $scopeId `
    --output $planSeedPath
if ($LASTEXITCODE -ne 0) {
    throw "composite-phase plan seed 生成失败，ExitCode=$LASTEXITCODE"
}
$planSeed = Get-Content -LiteralPath $planSeedPath -Raw -Encoding utf8 |
    ConvertFrom-Json
if ([string]$planSeed.status -ne "AWAITING_AUXILIARY_PREFLIGHT" -or
    [string]$planSeed.run_uuid -ne $runUuid -or
    [uint64]$planSeed.activation_epoch -ne $activationEpoch -or
    [string]$planSeed.scope_id -ne $scopeId -or
    $null -ne $planSeed.frozen_at_utc_unix_ns -or
    $null -ne $planSeed.scheduler_policy.preflight_file_sha256 -or
    [string]$planSeed.sequence_binding.sequence_semantic_sha256 -ne
        [string]$sequence.sequence_sha256) {
    throw "Prepare 只能生成等待同辅机 preflight 的 sealed plan seed"
}
$planSeedIdentity = Get-PublishedIdentity `
    $planSeedPath `
    (Join-Path $resolvedPublishedRun "composite-phase-plan-seed.json") `
    "composite-phase plan seed"

$bindingPath = Join-Path $stagingDirectory "probe-binding.json"
$binding = [ordered]@{
    schema_version = 4
    evidence_type = "mouse_effect_probe_binding"
    experiment = "physical_b_composite_phase_calibration"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_b"
    profile = "physical_b_composite_phase_calibration"
    run_role = "calibration_deletion"
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $configCopy
    obs_source_binding = $obsCopy
    capture_policy = $capturePolicyIdentity
    plan_seed = $planSeedIdentity
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 1
    max_abs_prefix_x_counts = 1
    production_aim_changed = $false
    new_production_gain_claimed = $false
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-PublishedIdentity `
    $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
    "composite-phase probe binding"

$task = [ordered]@{
    schema_version = 10
    evidence_type = "mouse_effect_probe_b_composite_phase_task"
    status = "PREPARED"
    experiment = "physical_b_composite_phase_calibration"
    run_role = "calibration_deletion"
    run_directory = $resolvedPublishedRun
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_b"
    profile = "physical_b_composite_phase_calibration"
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = 295
    window_count = 42
    negative_control_count = 4
    expected_nonzero_transition_count = 38
    max_abs_prefix_x_counts = 1
    physical_output_capability = $true
    requires_user_frontend_launch = $true
    physical_output_confirmation =
        "XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT"
    files = [ordered]@{
        launch_script = $launchScript
        probe_executable = $probeExecutable
        sidecar_executable = $sidecarExecutable
        sequence_executable = $sequenceExecutable
        composite_seal_executable = $sealExecutable
        opencv_runtime = $opencvRuntime
        ndi_runtime = $ndiRuntime
        ndi_license = $ndiLicense
        plan_generator = $planGenerator
        ledger_producer = $ledgerProducer
        binder = $binder
        evaluator = $evaluator
        config = $configCopy
        sequence = $sequenceIdentity
        probe_binding = $bindingIdentity
        obs_source_binding = $obsCopy
        obs_log = $obsLogCopy
        capture_policy = $capturePolicyIdentity
        plan_seed = $planSeedIdentity
    }
    capture = [ordered]@{
        source_name = $ndiSource
        frame_layout = (Get-RequiredValue `
            $capture "ndi_frame_layout").ToLowerInvariant()
        source_width = $sourceWidth
        source_height = $sourceHeight
        roi_width = $roiWidth
        roi_height = $roiHeight
        center_roi = $centerRoi
        roi_x = $configuredRoiX
        roi_y = $configuredRoiY
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
        minimum_coverage_frames = $minimumSidecarFrames
        coverage_basis = "ARMING_5S_PLUS_295_SOURCE_EVENTS_PLUS_1S_MARGIN"
        max_seconds = $MaxSeconds
        publishing_max_seconds = 60
        physical_output_capability = $false
        require_source_timing = $true
    }
    composite_policy = [ordered]@{
        policy_id = "b-composite-phase-calibration-v1"
        plan_seed_status = "AWAITING_AUXILIARY_PREFLIGHT"
        final_plan_frozen_on_auxiliary_before_sidecar = $true
        same_auxiliary_host_preflight_required = $true
        response_revealed_before_final_plan = $false
        binder_physical_output_capability = $false
        production_aim_changed = $false
        new_production_gain_claimed = $false
        fixed_pixel_speed_used_as_gate = $false
    }
    safety = [ordered]@{
        normal_aim_must_be_closed = $true
        emergency_virtual_keys = @(35, 119)
        right_button_deadman_required = $true
        any_failure_stops_without_compensation = $true
        zero_y_required = $true
        max_abs_pulse_counts = 1
        max_abs_prefix_x_counts = 1
        manual_mouse_motion_or_wasd_forbidden = $true
        no_runtime_phase_or_schedule_tuning = $true
    }
}
Write-NewUtf8Json (Join-Path $stagingDirectory "task.json") $task

$launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
    '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
    '-PhysicalOutputConfirmation {2}') -f
    $launchScript.path, $resolvedPublishedRun,
    "XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT"
$taskMarkdown = @(
    "# Mouse Effect Probe Physical B Composite-Phase Calibration",
    "",
    "- Run UUID：``$runUuid``；scope：``$scopeId``",
    "- 固定序列：295 samples、42 windows（38 个 ±1 X pulse + 4 个零命令 control）；净 X=0、Y=0、最大前缀=1 count",
    "- Launch 先在本辅机执行 output-off scheduler preflight，并在 sidecar/响应揭示前封存最终 plan；preflight 失败不会启动 sidecar 或 KMBOX",
    "- 本 Run 只校准 composite completion→NDI submission phase；不修改生产 Aim/F1/Y/prediction，不声明新 gain",
    '- 操作只看提示：【预检】时保持右键松开；【按住右键】后 5 秒内按住并保持；【现在松开右键】立即松开；【记录完成】后回传观察',
    "- 除右键 deadman 外，不要移动物理鼠标、不要按 WASD；松开右键、End、F8 或任一失败会立即停发且不补偿",
    "",
    "下面命令会发送真实 KMBOX 输入，只能由用户在辅机前台执行：",
    "",
    "``````powershell",
    $launchCommand,
    "``````",
    "") -join [Environment]::NewLine
Write-NewUtf8Text (Join-Path $stagingDirectory "TASK.md") $taskMarkdown

$observation = @(
    "# Physical B Composite-Phase 现场观察",
    "",
    "- Run UUID：``$runUuid``",
    "- 是否看到视角偏移：",
    "- 是否全程未移动物理鼠标/未按 WASD：",
    "- 正/负方向是否相反且左右一致：",
    "- 是否发生遮挡或 scene cut：",
    "- 是否发生异常或急停：",
    "- 用户原话：",
    "") -join [Environment]::NewLine
Write-NewUtf8Text `
    (Join-Path $stagingDirectory "OBSERVATION.md") $observation

$summary = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_b_composite_phase_prepare"
    status = "PREPARED_NOT_LAUNCHED"
    run_uuid = $runUuid
    run_role = "calibration_deletion"
    profile = "physical_b_composite_phase_calibration"
    scope_id = $scopeId
    local_bundle_directory = $resolvedRun
    published_run_directory = $resolvedPublishedRun
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = 295
    window_count = 42
    expected_nonzero_transition_count = 38
    plan_seed_semantic_sha256 = [string]$planSeed.plan_seed_semantic_sha256
    scheduler_preflight_executed = $false
    final_plan_frozen = $false
    physical_launch_executed = $false
}
Write-NewUtf8Json `
    (Join-Path $stagingDirectory "prepare-summary.json") $summary

Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
Write-Host "Physical B composite-phase Prepare 完成；未执行 preflight、sidecar 或 Launch。"
Write-Host "BundleDirectory=$resolvedRun"
Write-Host "PublishedRunDirectory=$resolvedPublishedRun"
Write-Host $launchCommand
} catch {
    $failure = $_
    if (Test-Path -LiteralPath $stagingDirectory -PathType Container) {
        $stagingFull = [IO.Path]::GetFullPath($stagingDirectory)
        $runParentFull = [IO.Path]::GetFullPath($runParent)
        $runParentPrefix = $runParentFull.TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar) +
            [IO.Path]::DirectorySeparatorChar
        $expectedStagingPrefix = ".{0}.incoming-" -f $runName
        if (-not $stagingFull.StartsWith(
                $runParentPrefix,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not (Split-Path -Leaf $stagingFull).StartsWith(
                $expectedStagingPrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "composite-phase staging 清理目标越界：$stagingFull"
        }
        Remove-Item -LiteralPath $stagingFull -Recurse -Force
    }
    throw $failure
}
