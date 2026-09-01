param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputOffRunDirectory,
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

function Write-NewUtf8Text([string]$Path, [string]$Content) {
    if (Test-Path -LiteralPath $Path) {
        throw "发布目标已存在，拒绝覆盖：$Path"
    }
    $pending = "$Path.pending-$PID-$([guid]::NewGuid().ToString('N'))"
    $utf8 = [Text.UTF8Encoding]::new($false)
    try {
        [IO.File]::WriteAllText($pending, $Content, $utf8)
        [IO.File]::Move($pending, $Path)
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    Write-NewUtf8Text $Path (($Value | ConvertTo-Json -Depth 20) + "`n")
}

function Copy-NewFile(
        [string]$Source,
        [string]$Destination,
        [string]$Description) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or
        (Test-Path -LiteralPath $Destination)) {
        throw "$Description 源不存在或目标已存在"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination
    return Get-FileIdentity $Destination $Description
}

function Parse-Roi([string]$Text, [string]$Name) {
    if ($Text -notmatch '^([0-9]+),([0-9]+),([1-9][0-9]*),([1-9][0-9]*)$') {
        throw "$Name 必须是 x,y,width,height"
    }
    return [ordered]@{
        x = [int]$Matches[1]
        y = [int]$Matches[2]
        width = [int]$Matches[3]
        height = [int]$Matches[4]
    }
}

if (Test-Path -LiteralPath $RunDirectory) {
    throw "RunDirectory 已存在，拒绝覆盖：$RunDirectory"
}
$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path
$sourceConfig = Get-FileIdentity $ConfigPath "config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "OBS source binding"
$resolvedOutputOff = (Resolve-Path -LiteralPath $OutputOffRunDirectory).Path
$outputOffSummaryPath = Join-Path $resolvedOutputOff "output-off-summary.json"
$outputOffWitnessPath = Join-Path $resolvedOutputOff `
    "background-witness-baseline.json"
$outputOffManifestPath = Join-Path $resolvedOutputOff `
    "pixel-evidence\manifest.json"
$outputOffReportPath = Join-Path $resolvedOutputOff "command-report.json"
$outputOffSequencePath = Join-Path $resolvedOutputOff "sequence.json"
foreach ($path in @(
        $outputOffSummaryPath, $outputOffWitnessPath,
        $outputOffManifestPath, $outputOffReportPath,
        $outputOffSequencePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Prepare 缺少 output-off 前置证据：$path"
    }
}
$outputOffSummary = Get-Content -LiteralPath $outputOffSummaryPath `
    -Raw -Encoding utf8 | ConvertFrom-Json
$outputOffWitness = Get-Content -LiteralPath $outputOffWitnessPath `
    -Raw -Encoding utf8 | ConvertFrom-Json
if ([string]$outputOffSummary.status -ne "VALID" -or
    [bool]$outputOffSummary.physical_output_capability -or
    [int64]$outputOffSummary.actual_requested_x_counts -ne 0 -or
    [int64]$outputOffSummary.backend_completed_x_counts -ne 0 -or
    [string]$outputOffWitness.status -ne "VALID" -or
    [bool]$outputOffWitness.physical_output_capability -or
    [bool]$outputOffWitness.visible_effect_analyzed) {
    throw "output-off 前置证据未证明 0 Mouse 与有效 witness baseline"
}

$captureConfig = Read-IniSection $sourceConfig.path "capture"
$mouseConfig = Read-IniSection $sourceConfig.path "mouse"
$aimConfig = Read-IniSection $sourceConfig.path "aim"
if ((Get-RequiredValue $captureConfig "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $captureConfig "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $captureConfig "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouseConfig "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouseConfig "allow_send_input")) {
    throw "Prepare 要求精确 NDI/source clock、KMBOX NET 且 config 默认输出关闭"
}
$ndiSource = Get-RequiredValue $captureConfig "ndi_source_name"
$sourceWidth = Get-RequiredInteger $captureConfig "ndi_source_width"
$sourceHeight = Get-RequiredInteger $captureConfig "ndi_source_height"
$roiWidth = Get-RequiredInteger $captureConfig "roi_width"
$roiHeight = Get-RequiredInteger $captureConfig "roi_height"
$leftRoi = Parse-Roi $LeftWitnessRoi "LeftWitnessRoi"
$rightRoi = Parse-Roi $RightWitnessRoi "RightWitnessRoi"
foreach ($roi in @($leftRoi, $rightRoi)) {
    if ($roi.x + $roi.width -gt $roiWidth -or
        $roi.y + $roi.height -gt $roiHeight) {
        throw "background witness ROI 超出 Capture ROI"
    }
}
if ($leftRoi.x + $leftRoi.width -gt $rightRoi.x) {
    throw "左右 background witness ROI 不得重叠"
}
$witnessMargins = @(
    [int]$leftRoi.x
    [int]($roiWidth - ([int]$leftRoi.x + [int]$leftRoi.width))
    [int]$leftRoi.y
    [int]($roiHeight - ([int]$leftRoi.y + [int]$leftRoi.height))
    [int]$rightRoi.x
    [int]($roiWidth - ([int]$rightRoi.x + [int]$rightRoi.width))
    [int]$rightRoi.y
    [int]($roiHeight - ([int]$rightRoi.y + [int]$rightRoi.height))
)
$minimumWitnessMargin = [int](
    ($witnessMargins | Measure-Object -Minimum).Minimum)
$countsPerPixelX = [double](Get-RequiredValue $aimConfig "counts_per_pixel_x")
if ([double]::IsNaN($countsPerPixelX) -or
    [double]::IsInfinity($countsPerPixelX) -or
    $countsPerPixelX -le 0.0) {
    throw "Aim counts_per_pixel_x 无效"
}
$configurationNominalPxPerCount = 1.0 / $countsPerPixelX
if ($configurationNominalPxPerCount -ge $minimumWitnessMargin) {
    throw "单 count 配置名义位移没有保守 witness 边界余量"
}

$runParent = Split-Path -Parent ([IO.Path]::GetFullPath($RunDirectory))
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
[void](New-Item -ItemType Directory -Path $resolvedRun)
$toolDirectory = Join-Path $resolvedRun "tool"
[void](New-Item -ItemType Directory -Path $toolDirectory)

$probeExecutable = Copy-NewFile `
    (Join-Path $resolvedToolRoot "XenMouseEffectProbe.exe") `
    (Join-Path $toolDirectory "XenMouseEffectProbe.exe") `
    "probe executable"
$sidecarExecutable = Copy-NewFile `
    (Join-Path $resolvedToolRoot "XenCaptureEvidence.exe") `
    (Join-Path $toolDirectory "XenCaptureEvidence.exe") `
    "sidecar executable"
$opencvRuntime = Copy-NewFile `
    (Join-Path $resolvedToolRoot "opencv_world4140.dll") `
    (Join-Path $toolDirectory "opencv_world4140.dll") `
    "OpenCV runtime"
$ndiRuntime = Copy-NewFile `
    (Join-Path $resolvedToolRoot "Processing.NDI.Lib.x64.dll") `
    (Join-Path $toolDirectory "Processing.NDI.Lib.x64.dll") `
    "NDI runtime"
$ndiLicense = Copy-NewFile `
    (Join-Path $resolvedToolRoot "Processing.NDI.Lib.Licenses.txt") `
    (Join-Path $toolDirectory "Processing.NDI.Lib.Licenses.txt") `
    "NDI license"
$launchScript = Copy-NewFile `
    (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
    (Join-Path $toolDirectory "launch_mouse_effect_probe_a.ps1") `
    "Launch script"
$analyzerScript = Copy-NewFile `
    (Join-Path $PSScriptRoot "analyze_mouse_effect_probe_pixels.py") `
    (Join-Path $toolDirectory "analyze_mouse_effect_probe_pixels.py") `
    "pixel analyzer"
$configCopy = Copy-NewFile $sourceConfig.path `
    (Join-Path $resolvedRun "config.ini") "Run config.ini"
$obsBindingCopy = Copy-NewFile $sourceObsBinding.path `
    (Join-Path $resolvedRun "obs-source-binding.json") `
    "Run OBS source binding"

$sequencePath = Join-Path $resolvedRun "sequence.json"
& (Join-Path $resolvedToolRoot "XenMouseEffectProbeSequence.exe") `
    --output $sequencePath `
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
if ([int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
    (@($pulses | ForEach-Object { [int]$_.dx_counts }) -join ",") -ne
        "1,-1,-1,1" -or
    [string]$outputOffSummary.sequence_sha256 -ne
        [string]$sequence.sequence_sha256) {
    throw "Physical A 序列与 output-off 已验证序列不一致"
}
$sequenceIdentity = Get-FileIdentity $sequencePath "Physical A sequence"

$runUuid = [guid]::NewGuid().ToString()
$activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$bindingPath = Join-Path $resolvedRun "probe-binding.json"
$binding = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_binding"
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = "sparse_pulse_a"
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_file = $sequenceIdentity
    capture_source_name = $ndiSource
    config = $configCopy
    obs_source_binding = $obsBindingCopy
    output_off_preflight = [ordered]@{
        run_directory = $resolvedOutputOff
        summary = Get-FileIdentity $outputOffSummaryPath "output-off summary"
        witness = Get-FileIdentity $outputOffWitnessPath "output-off witness"
        manifest = Get-FileIdentity $outputOffManifestPath "output-off manifest"
        command_report = Get-FileIdentity $outputOffReportPath "output-off command report"
        sequence = Get-FileIdentity $outputOffSequencePath "output-off sequence"
    }
    sidecar_physical_output_capability = $false
    normal_aim_output_required = $false
    dy_counts_required = 0
    max_abs_pulse_counts = 1
    expected_background_shift_sign_per_positive_count = -1
}
Write-NewUtf8Json $bindingPath $binding
$bindingIdentity = Get-FileIdentity $bindingPath "Physical A probe binding"

$taskPath = Join-Path $resolvedRun "task.json"
$task = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a_task"
    status = "PREPARED"
    run_directory = $resolvedRun
    run_uuid = $runUuid
    activation_epoch = $activationEpoch
    dispatch_mode = "physical_a"
    profile = "sparse_pulse_a"
    sequence_sha256 = [string]$sequence.sequence_sha256
    sequence_sample_count = [uint64]$samples.Count
    physical_output_capability = $true
    physical_output_confirmation =
        "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
    files = [ordered]@{
        launch_script = $launchScript
        probe_executable = $probeExecutable
        sidecar_executable = $sidecarExecutable
        opencv_runtime = $opencvRuntime
        ndi_runtime = $ndiRuntime
        ndi_license = $ndiLicense
        analyzer = $analyzerScript
        config = $configCopy
        sequence = $sequenceIdentity
        probe_binding = $bindingIdentity
        obs_source_binding = $obsBindingCopy
    }
    capture = [ordered]@{
        source_name = $ndiSource
        frame_layout = (Get-RequiredValue `
            $captureConfig "ndi_frame_layout").ToLowerInvariant()
        source_width = $sourceWidth
        source_height = $sourceHeight
        roi_width = $roiWidth
        roi_height = $roiHeight
        center_roi = Get-RequiredBoolean $captureConfig "center_roi"
        roi_x = Get-RequiredInteger $captureConfig "roi_x"
        roi_y = Get-RequiredInteger $captureConfig "roi_y"
        discovery_timeout_ms = Get-RequiredInteger `
            $captureConfig "ndi_discovery_timeout_ms"
        receive_timeout_ms = Get-RequiredInteger `
            $captureConfig "ndi_receive_timeout_ms"
        disconnect_timeout_ms = Get-RequiredInteger `
            $captureConfig "ndi_disconnect_timeout_ms"
        clock_sync_url = Get-RequiredValue `
            $captureConfig "ndi_clock_sync_url"
        clock_sync_interval_ms = Get-RequiredInteger `
            $captureConfig "ndi_clock_sync_interval_ms"
        clock_sync_timeout_ms = Get-RequiredInteger `
            $captureConfig "ndi_clock_sync_timeout_ms"
        clock_mapping_max_age_ms = Get-RequiredInteger `
            $captureConfig "ndi_clock_mapping_max_age_ms"
        require_frame_metadata = Get-RequiredBoolean `
            $captureConfig "ndi_require_frame_metadata"
    }
    sidecar = [ordered]@{
        frames = $SidecarFrames
        max_seconds = $MaxSeconds
        physical_output_capability = $false
    }
    witness = [ordered]@{
        left_roi = $leftRoi
        right_roi = $rightRoi
        minimum_image_boundary_margin_px = $minimumWitnessMargin
        configuration_nominal_px_per_count = $configurationNominalPxPerCount
        expected_background_shift_sign_per_positive_count = -1
        output_off_baseline = [ordered]@{
            left_abs_dx_p99 =
                [double]$outputOffWitness.zero_input_baseline.left_abs_dx_px.p99
            left_abs_dx_max =
                [double]$outputOffWitness.zero_input_baseline.left_abs_dx_px.max
            right_abs_dx_p99 =
                [double]$outputOffWitness.zero_input_baseline.right_abs_dx_px.p99
            right_abs_dx_max =
                [double]$outputOffWitness.zero_input_baseline.right_abs_dx_px.max
            left_right_dx_difference_p99 = [double]$outputOffWitness.
                zero_input_baseline.abs_left_right_dx_difference_px.p99
            left_right_dx_difference_max = [double]$outputOffWitness.
                zero_input_baseline.abs_left_right_dx_difference_px.max
        }
    }
    safety = [ordered]@{
        normal_aim_must_be_closed = $true
        right_button_deadman_required = $true
        emergency_virtual_keys = @(35, 119)
        any_failure_stops_without_compensation = $true
        zero_y_required = $true
        max_abs_prefix_x_counts = 1
    }
}
Write-NewUtf8Json $taskPath $task

$launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
    '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
    '-PhysicalOutputConfirmation {2}') -f
    $launchScript.path, $resolvedRun,
    "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
$taskMarkdown = @(
    "# Mouse Effect Probe A 级 Physical Run",
    "",
    "- Run UUID：``$runUuid``",
    "- 序列：``sparse_pulse_a``，samples=$($samples.Count)，``+1/-1、-1/+1``，净 X=0，最大前缀=1 count",
    "- 唯一变量：独立 X-only ±1 count 外生脉冲；正常 Aim 必须关闭，Y 永远为 0",
    "- sidecar：$SidecarFrames 帧、source timing 必须 VALID、physical_output_capability=false",
    "- witness：left=``$LeftWitnessRoi``；right=``$RightWitnessRoi``；正 X count 预注册为背景向左（负 shift）",
    "- 安全：进入静止、无人物/Overlay 的高对比背景后持续按住右键；松开右键、End、F8、Mouse/ACK/source/sidecar 任一失败都会立即停发且不补偿",
    "",
    "下面命令会发送真实 KMBOX 输入，只能由用户在本机前台执行：",
    "",
    "``````powershell",
    $launchCommand,
    "``````",
    "",
    "执行结束后请把以下事实直接发回当前对话：正/负单 count 是否可见、方向是否相反且符合背景左/右移动预期、是否出现异常/急停；不要自行编辑观察文件。",
    ""
) -join "`n"
Write-NewUtf8Text (Join-Path $resolvedRun "TASK.md") $taskMarkdown
$observation = @(
    "# Mouse Effect Probe A 人工观察",
    "",
    "- 执行人：",
    "- 执行时间：",
    "- 完成全部序列：",
    "- 发生急停/松键：",
    "- +1 count 背景可见方向：",
    "- -1 count 背景可见方向：",
    "- 左右 witness 一致性：",
    "- 异常：",
    "- 人工结论：",
    ""
) -join "`n"
Write-NewUtf8Text (Join-Path $resolvedRun "OBSERVATION.md") $observation

Write-Host "Physical A Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
Write-Host "RunDirectory=$resolvedRun"
Write-Host $launchCommand
