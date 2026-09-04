param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$A2MagnitudeAnalysisPath,
    [Parameter(Mandatory = $true)]
    [string]$B0EvaluationPath,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [ValidateRange(1685, 4000)]
    [uint64]$SidecarFrames = 2200,
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

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "Physical B 多幅值 RunDirectory 已存在，拒绝覆盖：$resolvedRun"
}
foreach ($candidate in @($resolvedRun, $resolvedPublishedRun)) {
    if ([string]::Equals(
            $candidate, [IO.Path]::GetPathRoot($candidate),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Physical B 多幅值 RunDirectory 不能是根目录"
    }
}

$sourceConfig = Get-FileIdentity $ConfigPath "Physical B 多幅值 config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "Physical B 多幅值 OBS source binding"
$sourceA2 = Get-FileIdentity `
    $A2MagnitudeAnalysisPath "A2 magnitude analysis"
$sourceB0 = Get-FileIdentity $B0EvaluationPath "B0 fidelity evaluation"
$a2 = Get-Content -LiteralPath $sourceA2.path -Raw -Encoding utf8 |
    ConvertFrom-Json
$b0 = Get-Content -LiteralPath $sourceB0.path -Raw -Encoding utf8 |
    ConvertFrom-Json
if ([int]$a2.schema_version -ne 1 -or
    [string]$a2.evidence_type -ne
        "mouse_effect_probe_a2_magnitude_domain_analysis" -or
    [string]$a2.status -ne "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" -or
    [bool]$a2.physical_output_capability -or
    [int]$a2.physical_dispatch_count -ne 0 -or
    [bool]$a2.production_aim_changed -or
    -not [bool]$a2.evaluation.f1_deleted_for_magnitude_domain -or
    [bool]$a2.evaluation.new_production_gain_claimed -or
    [bool]$a2.evaluation.primary_model.validation_used_for_refit) {
    throw "A2 幅值分析未满足 F1 deletion/no-production 合同"
}
if ([string]$b0.status -ne "BASELINE_REPLAY_FIDELITY_INVALID" -or
    [bool]$b0.physical_output_capability -or
    [int]$b0.physical_dispatch_count -ne 0 -or
    [bool]$b0.production_aim_changed) {
    throw "B0 fidelity evidence 未保持 output-off invalid 合同"
}

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
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$leftRoi = Parse-Roi $LeftWitnessRoi "LeftWitnessRoi"
$rightRoi = Parse-Roi $RightWitnessRoi "RightWitnessRoi"
foreach ($roi in @($leftRoi, $rightRoi)) {
    if ($roi.x + $roi.width -gt $roiWidth -or
        $roi.y + $roi.height -gt $roiHeight) {
        throw "Physical B 多幅值 witness ROI 超出 Capture ROI"
    }
}
if ($leftRoi.x + $leftRoi.width -gt $rightRoi.x) {
    throw "Physical B 多幅值左右 witness ROI 不得重叠"
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
    throw "Physical B 多幅值 OBS binding 与 config/ROI scope 不一致"
}

$runParent = Split-Path -Parent $resolvedRun
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedRun
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "Physical B 多幅值 staging 已存在：$stagingDirectory"
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
$probeExecutable = Copy-Tool `
    "XenMouseEffectProbe.exe" "Physical B 多幅值 probe executable"
$sidecarExecutable = Copy-Tool `
    "XenCaptureEvidence.exe" "Physical B 多幅值 sidecar executable"
$sequenceExecutable = Copy-Tool `
    "XenMouseEffectProbeSequence.exe" "Physical B 多幅值 sequence executable"
$opencvRuntime = Copy-Tool `
    "opencv_world4140.dll" "Physical B 多幅值 OpenCV runtime"
$ndiRuntime = Copy-Tool `
    "Processing.NDI.Lib.x64.dll" "Physical B 多幅值 NDI runtime"
$ndiLicense = Copy-Tool `
    "Processing.NDI.Lib.Licenses.txt" "Physical B 多幅值 NDI license"
$launchScript = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $toolDirectory "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $publishedTool "launch_mouse_effect_probe_a.ps1") `
    "Physical B 多幅值 Launch script"
$analyzer = Copy-NewPublishedFile `
    (Join-Path $PSScriptRoot `
        "analyze_mouse_effect_probe_b_command_magnitude.py") `
    (Join-Path $toolDirectory `
        "analyze_mouse_effect_probe_b_command_magnitude.py") `
    (Join-Path $publishedTool `
        "analyze_mouse_effect_probe_b_command_magnitude.py") `
    "Physical B 多幅值 analyzer"
$configCopy = Copy-NewPublishedFile `
    $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
    (Join-Path $resolvedPublishedRun "config.ini") `
    "Physical B 多幅值 config copy"
$obsCopy = Copy-NewPublishedFile `
    $sourceObsBinding.path `
    (Join-Path $stagingDirectory "obs-source-binding.json") `
    (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
    "Physical B 多幅值 OBS binding copy"
$a2Copy = Copy-NewPublishedFile `
    $sourceA2.path (Join-Path $inputDirectory "a2-magnitude-analysis.json") `
    (Join-Path $publishedInputs "a2-magnitude-analysis.json") `
    "A2 magnitude analysis copy"
$b0Copy = Copy-NewPublishedFile `
    $sourceB0.path (Join-Path $inputDirectory "b0-fidelity-evaluation.json") `
    (Join-Path $publishedInputs "b0-fidelity-evaluation.json") `
    "B0 fidelity evaluation copy"

$sequencePath = Join-Path $stagingDirectory "sequence.json"
& (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
    --output $sequencePath `
    --profile physical-b-command-magnitude `
    --run-role primary `
    --baseline-samples 64 `
    --response-samples 48 `
    --guard-samples 32
if ($LASTEXITCODE -ne 0) {
    throw "Physical B 多幅值 sequence tool 失败，ExitCode=$LASTEXITCODE"
}
$sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
    ConvertFrom-Json
$samples = @($sequence.samples)
$blocks = @($sequence.blocks)
$pulses = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
$amplitudeOrder = @($blocks | ForEach-Object {
    [int]$_.amplitude_counts
})
$blockRoles = @($blocks | ForEach-Object { [string]$_.role })
$blockPolarities = @($blocks | ForEach-Object { [string]$_.polarity })
if ([int]$sequence.schema -ne 6 -or
    [string]$sequence.profile -ne
        "physical_b_command_magnitude_primary" -or
    [string]$sequence.request.run_role -ne "primary" -or
    [uint64]$sequence.request.baseline_sample_count -ne 64 -or
    [uint64]$sequence.request.response_sample_count -ne 48 -or
    [uint64]$sequence.request.guard_sample_count -ne 32 -or
    $samples.Count -ne 1684 -or $blocks.Count -ne 10 -or
    $pulses.Count -ne 20 -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 13 -or
    ($amplitudeOrder -join ",") -ne "1,1,4,4,13,13,2,2,8,8" -or
    ($blockRoles -join ",") -ne
        "estimation,estimation,estimation,estimation,estimation,estimation,confirmation,confirmation,confirmation,confirmation" -or
    ($blockPolarities -join ",") -ne
        "normal,inverted,normal,inverted,normal,inverted,normal,inverted,normal,inverted" -or
    @($samples | Where-Object {
        [int]$_.dy_counts -ne 0 -or
        ([math]::Abs([int]$_.dx_counts) -notin @(0, 1, 2, 4, 8, 13))
    }).Count -ne 0) {
    throw "Physical B 多幅值 sequence 与冻结 X-only/net/prefix/split 合同不一致"
}
$sequenceIdentity = Get-PublishedIdentity `
    $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
    "Physical B 多幅值 sequence"

$scope = [ordered]@{
    schema_version = 1
    config_sha256 = $configCopy.sha256
    obs_source_binding_sha256 = $obsCopy.sha256
    a2_magnitude_analysis_sha256 = $a2Copy.sha256
    b0_fidelity_evaluation_sha256 = $b0Copy.sha256
    analyzer_sha256 = $analyzer.sha256
    ndi_source_name = $ndiSource
    roi_width = $roiWidth
    roi_height = $roiHeight
    left_witness_roi = $LeftWitnessRoi
    right_witness_roi = $RightWitnessRoi
    sequence_sha256 = [string]$sequence.sequence_sha256
    primary_estimation_amplitudes = @(1, 4, 13)
    within_run_confirmation_amplitudes = @(2, 8)
    require_source_timing = $true
}
$scopeId = Get-TextSha256 ($scope | ConvertTo-Json -Depth 8 -Compress)
$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

$bindingPath = Join-Path $stagingDirectory "probe-binding.json"
$binding = [ordered]@{
    schema_version = 4
    evidence_type = "mouse_effect_probe_binding"
    experiment = "physical_b_command_magnitude_primary"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_b"
    profile = "physical_b_command_magnitude_primary"
    run_role = "primary"
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $configCopy
    obs_source_binding = $obsCopy
    a2_magnitude_analysis = $a2Copy
    b0_fidelity_evaluation = $b0Copy
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 13
    max_abs_prefix_x_counts = 13
    validation_used_for_refit = $false
    new_production_gain_claimed = $false
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-PublishedIdentity `
    $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
    "Physical B 多幅值 probe binding"

$task = [ordered]@{
    schema_version = 8
    evidence_type = "mouse_effect_probe_b_command_magnitude_task"
    status = "PREPARED"
    experiment = "physical_b_command_magnitude_primary"
    run_role = "primary"
    run_directory = $resolvedPublishedRun
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_b"
    profile = "physical_b_command_magnitude_primary"
    scope_id = $scopeId
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = 1684
    expected_nonzero_transition_count = 20
    max_abs_prefix_x_counts = 13
    physical_output_capability = $true
    requires_user_frontend_launch = $true
    physical_output_confirmation =
        "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT"
    files = [ordered]@{
        launch_script = $launchScript
        probe_executable = $probeExecutable
        sidecar_executable = $sidecarExecutable
        sequence_executable = $sequenceExecutable
        opencv_runtime = $opencvRuntime
        ndi_runtime = $ndiRuntime
        ndi_license = $ndiLicense
        magnitude_analyzer = $analyzer
        config = $configCopy
        sequence = $sequenceIdentity
        probe_binding = $bindingIdentity
        obs_source_binding = $obsCopy
        a2_magnitude_analysis = $a2Copy
        b0_fidelity_evaluation = $b0Copy
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
        publishing_max_seconds = 60
        physical_output_capability = $false
        left_witness_roi = $LeftWitnessRoi
        right_witness_roi = $RightWitnessRoi
        require_source_timing = $true
    }
    dynamics_policy = [ordered]@{
        policy_id = "b-command-magnitude-primary-v1"
        input_definition = "backend_completed_relative_command_dx_counts"
        primary_estimation_amplitudes = @(1, 4, 13)
        within_run_confirmation_amplitudes = @(2, 8)
        validation_used_for_refit = $false
        new_production_gain_claimed = $false
        cross_run_holdout_required_before_candidate = $true
        fixed_pixel_speed_used_as_gate = $false
    }
    safety = [ordered]@{
        normal_aim_must_be_closed = $true
        emergency_virtual_keys = @(35, 119)
        right_button_deadman_required = $true
        any_failure_stops_without_compensation = $true
        zero_y_required = $true
        max_abs_pulse_counts = 13
        max_abs_prefix_x_counts = 13
        manual_mouse_motion_or_wasd_forbidden = $true
        no_runtime_amplitude_or_repetition_change = $true
    }
}
Write-NewUtf8Json (Join-Path $stagingDirectory "task.json") $task

$launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
    '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
    '-PhysicalOutputConfirmation {2}') -f
    $launchScript.path, $resolvedPublishedRun,
    "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT"
$taskMarkdown = @(
    "# Mouse Effect Probe Physical B Command-Magnitude Primary",
    "",
    "- Run UUID：``$runUuid``；scope：``$scopeId``",
    "- 固定序列：1684 samples；幅值顺序 1,1,4,4,13,13,2,2,8,8；每块独立 pre/post guard 并自动回锚；净 X=0、Y=0、最大前缀=13 counts",
    "- 拟合只用 1/4/13；同 Run confirmation 只用 2/8 且不得重拟合；本 Run 不声明生产增益",
    '- 操作只看四个提示：【准备】保持右键松开；【按住右键】后 5 秒内按住并保持；【现在松开右键】立即松开；【记录完成】后回传观察',
    "- 除右键 deadman 外，不要移动物理鼠标、不要按 WASD；松开右键、End、F8 或任一失败会立即停发且不补偿",
    "",
    "下面命令会发送真实 KMBOX 输入，只能由用户在本机前台执行：",
    "",
    "``````powershell",
    $launchCommand,
    "``````",
    "") -join [Environment]::NewLine
Write-NewUtf8Text (Join-Path $stagingDirectory "TASK.md") $taskMarkdown

$observation = @(
    "# Physical B Command-Magnitude 现场观察",
    "",
    "- Run UUID：``$runUuid``",
    "- 是否全程未移动物理鼠标/未按 WASD：",
    "- 正/负方向是否相反且左右 witness 一致：",
    "- 1/2/4/8/13 counts 的可见性或异常：",
    "- 是否发生遮挡、scene cut、异常或急停：",
    "- 用户原话：",
    "") -join [Environment]::NewLine
Write-NewUtf8Text `
    (Join-Path $stagingDirectory "OBSERVATION.md") $observation

$summary = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_b_command_magnitude_prepare"
    status = "PREPARED_NOT_LAUNCHED"
    run_uuid = $runUuid
    run_role = "primary"
    profile = "physical_b_command_magnitude_primary"
    scope_id = $scopeId
    local_bundle_directory = $resolvedRun
    published_run_directory = $resolvedPublishedRun
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = 1684
    expected_nonzero_transition_count = 20
    max_abs_prefix_x_counts = 13
    physical_launch_executed = $false
}
Write-NewUtf8Json `
    (Join-Path $stagingDirectory "prepare-summary.json") $summary

Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
Write-Host "Physical B command-magnitude Primary Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
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
            throw "Physical B 多幅值 staging 清理目标越界：$stagingFull"
        }
        Remove-Item -LiteralPath $stagingFull -Recurse -Force
    }
    throw $failure
}
