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
    [ValidateSet("primary", "validation")]
    [string]$Role,
    [ValidateRange(32, 2400)]
    [uint64]$Frames = 960,
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
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return [BitConverter]::ToString($algorithm.ComputeHash($bytes)).
            Replace("-", "").ToLowerInvariant()
    } finally {
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

function Get-PublishedFileIdentity(
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

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    if (Test-Path -LiteralPath $Path) {
        throw "JSON 发布目标已存在，拒绝覆盖：$Path"
    }
    $json = ($Value | ConvertTo-Json -Depth 20) +
        [Environment]::NewLine
    [IO.File]::WriteAllText(
        $Path, $json, [Text.UTF8Encoding]::new($false))
}

$resolvedFinal = [IO.Path]::GetFullPath($RunDirectory)
if (Test-Path -LiteralPath $resolvedFinal) {
    throw "S1 RunDirectory 已存在，拒绝覆盖：$resolvedFinal"
}
$runParent = Split-Path -Parent $resolvedFinal
if (-not $runParent -or
    [string]::Equals(
        $resolvedFinal, [IO.Path]::GetPathRoot($resolvedFinal),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "S1 RunDirectory 不能是根目录"
}
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedFinal
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "S1 staging 目录已存在：$stagingDirectory"
}
[void](New-Item -ItemType Directory -Path $stagingDirectory)

$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path
$captureExecutable = Get-FileIdentity (
    Join-Path $resolvedToolRoot "XenCaptureEvidence.exe") `
    "XenCaptureEvidence.exe"
$opencvRuntime = Get-FileIdentity (
    Join-Path $resolvedToolRoot "opencv_world4140.dll") `
    "OpenCV runtime"
$ndiRuntime = Get-FileIdentity (
    Join-Path $resolvedToolRoot "Processing.NDI.Lib.x64.dll") `
    "NDI runtime"
$ndiLicense = Get-FileIdentity (
    Join-Path $resolvedToolRoot "Processing.NDI.Lib.Licenses.txt") `
    "NDI license"
$sourceConfig = Get-FileIdentity $ConfigPath "config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "OBS source binding"

$capture = Read-IniSection $sourceConfig.path "capture"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url"))) {
    throw "S1 只接受精确 NDI source 与显式 clock sync"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$frameLayout = Get-RequiredValue $capture "ndi_frame_layout"
$sourceWidth = Get-RequiredInteger $capture "ndi_source_width"
$sourceHeight = Get-RequiredInteger $capture "ndi_source_height"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
$leftRoi = Parse-Roi $LeftWitnessRoi "LeftWitnessRoi"
$rightRoi = Parse-Roi $RightWitnessRoi "RightWitnessRoi"
foreach ($roi in @($leftRoi, $rightRoi)) {
    if ($roi.x + $roi.width -gt $roiWidth -or
        $roi.y + $roi.height -gt $roiHeight) {
        throw "S1 witness ROI 超出 Capture ROI"
    }
}
if ($leftRoi.x + $leftRoi.width -gt $rightRoi.x) {
    throw "S1 左右 witness ROI 不得重叠"
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
    throw "S1 OBS binding 与 config/ROI scope 不一致"
}

$forbiddenProcesses = @(
    Get-Process -Name Xen, XenLauncher, XenMouseBenchmark, XenMouseEffectProbe `
        -ErrorAction SilentlyContinue)
if ($forbiddenProcesses.Count -ne 0) {
    throw "S1 要求正常 Aim 与全部 Mouse/probe 工具关闭"
}

$configCopyPath = Join-Path $stagingDirectory "config.ini"
$obsCopyPath = Join-Path $stagingDirectory "obs-source-binding.json"
Copy-Item -LiteralPath $sourceConfig.path -Destination $configCopyPath
Copy-Item -LiteralPath $sourceObsBinding.path -Destination $obsCopyPath
$publishedConfigPath = Join-Path $resolvedFinal "config.ini"
$publishedObsPath = Join-Path $resolvedFinal "obs-source-binding.json"
$configCopy = Get-PublishedFileIdentity `
    $configCopyPath $publishedConfigPath "S1 config copy"
$obsCopy = Get-PublishedFileIdentity `
    $obsCopyPath $publishedObsPath "S1 OBS binding copy"

$scope = [ordered]@{
    schema_version = 1
    config_sha256 = $configCopy.sha256
    obs_source_binding_sha256 = $obsCopy.sha256
    capture_executable_sha256 = $captureExecutable.sha256
    opencv_runtime_sha256 = $opencvRuntime.sha256
    ndi_runtime_sha256 = $ndiRuntime.sha256
    ndi_source_name = $ndiSource
    frame_layout = $frameLayout
    source_width = $sourceWidth
    source_height = $sourceHeight
    roi_width = $roiWidth
    roi_height = $roiHeight
    left_witness_roi = $LeftWitnessRoi
    right_witness_roi = $RightWitnessRoi
    require_source_timing = $true
    actual_command_definition = "none_output_off"
}
$scopeCanonical = $scope | ConvertTo-Json -Depth 8 -Compress
$scopeId = Get-TextSha256 $scopeCanonical
$scopePath = Join-Path $stagingDirectory "scope.json"
Write-NewUtf8Json $scopePath ([ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_scope"
    scope_id = $scopeId
    physical_output_capability = $false
    scope = $scope
})

$pixelEvidence = Join-Path $stagingDirectory "pixel-evidence"
$arguments = @(
    "--ndi-source", $ndiSource,
    "--binding", $obsCopyPath,
    "--output", $pixelEvidence,
    "--frames", [string]$Frames,
    "--max-seconds", [string]$MaxSeconds,
    "--frame-layout", $frameLayout,
    "--source-width", [string]$sourceWidth,
    "--source-height", [string]$sourceHeight,
    "--roi-width", [string]$roiWidth,
    "--roi-height", [string]$roiHeight,
    "--discovery-timeout-ms",
        (Get-RequiredValue $capture "ndi_discovery_timeout_ms"),
    "--receive-timeout-ms",
        (Get-RequiredValue $capture "ndi_receive_timeout_ms"),
    "--disconnect-timeout-ms",
        (Get-RequiredValue $capture "ndi_disconnect_timeout_ms"),
    "--clock-sync-url", (Get-RequiredValue $capture "ndi_clock_sync_url"),
    "--clock-sync-interval-ms",
        (Get-RequiredValue $capture "ndi_clock_sync_interval_ms"),
    "--clock-sync-timeout-ms",
        (Get-RequiredValue $capture "ndi_clock_sync_timeout_ms"),
    "--clock-mapping-max-age-ms",
        (Get-RequiredValue $capture "ndi_clock_mapping_max_age_ms"),
    "--require-source-timing"
)
if (Get-RequiredBoolean $capture "ndi_require_frame_metadata") {
    $arguments += "--require-frame-metadata"
}

& $captureExecutable.path @arguments
if ($LASTEXITCODE -ne 0) {
    throw "S1 CaptureEvidence 失败，ExitCode=$LASTEXITCODE；保留 incoming 供诊断"
}
$manifestPath = Join-Path $pixelEvidence "manifest.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 |
    ConvertFrom-Json
$manifestFrames = @($manifest.frames)
if ([string]$manifest.evidence_type -ne "output_off_capture" -or
    [bool]$manifest.physical_output_capability -or
    [string]$manifest.capture_source_name -ne $ndiSource -or
    [uint64]$manifest.requested_frame_count -ne $Frames -or
    [uint64]$manifest.recorded_frame_count -ne $Frames -or
    $manifestFrames.Count -ne $Frames -or
    [string]$manifest.source_binding.sha256 -ne $obsCopy.sha256) {
    throw "S1 manifest 顶层身份或 frame 容量无效"
}
foreach ($frame in $manifestFrames) {
    if (-not [bool]$frame.source_timestamp_valid -or
        -not [bool]$frame.source_time_timing_valid -or
        [string]$frame.source_clock_status -ne "VALID" -or
        [uint64]$frame.source_dropped_frames -ne 0 -or
        [uint64]$frame.transport_dropped_frames -ne 0 -or
        [uint64]$frame.transport_invalid_packets -ne 0) {
        throw "S1 manifest 存在 invalid timing/drop frame"
    }
}

$runUuid = [guid]::NewGuid().ToString()
$captureProcessSessionId = [guid]::NewGuid().ToString()
$sessionPath = Join-Path $stagingDirectory "s1-session.json"
$publishedScopePath = Join-Path $resolvedFinal "scope.json"
$publishedManifestPath = Join-Path $resolvedFinal "pixel-evidence\manifest.json"
$session = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_s1_session"
    status = "RECORDED_UNANALYZED"
    physical_output_capability = $false
    probe_started = $false
    mouse_opened = $false
    actual_command_zero = $true
    aim_off = $true
    run_uuid = $runUuid
    run_role = $Role
    scope_id = $scopeId
    capture_process_session_id = $captureProcessSessionId
    run_directory = $resolvedFinal
    frame_count = $Frames
    obs_source_binding_sha256 = $obsCopy.sha256
    manifest_sha256 = Get-FileSha256 $manifestPath
    files = [ordered]@{
        config = $configCopy
        obs_source_binding = $obsCopy
        scope = Get-PublishedFileIdentity `
            $scopePath $publishedScopePath "S1 scope"
        manifest = Get-PublishedFileIdentity `
            $manifestPath $publishedManifestPath "S1 manifest"
        capture_executable = $captureExecutable
        opencv_runtime = $opencvRuntime
        ndi_runtime = $ndiRuntime
        ndi_license = $ndiLicense
    }
}
Write-NewUtf8Json $sessionPath $session

Move-Item -LiteralPath $stagingDirectory -Destination $resolvedFinal
Write-Host "Physical A2 S1 $Role 已原子发布；无 Probe、Mouse 或物理输出。"
Write-Host "RunDirectory=$resolvedFinal"
Write-Host "RunUUID=$runUuid"
Write-Host "ScopeId=$scopeId"
