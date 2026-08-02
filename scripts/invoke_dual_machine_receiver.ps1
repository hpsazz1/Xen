param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("GeometryStatic", "MoveLeft", "MoveRight", "Shuttle",
        "SuperJump", "Occlusion", "MultiTarget", "SoakFreeRun")]
    [string]$Scenario,
    [ValidateSet("xudp_jpeg", "udp_mjpeg", "ndi")]
    [string]$CaptureBackend = "xudp_jpeg",
    [ValidateSet("Prepare", "Run")]
    [string]$Mode = "Prepare",
    [string]$PackageRoot = (Join-Path $PSScriptRoot ".."),
    [string]$ReportRoot = "C:\XenLab\reports",
    [string]$RunRoot = "C:\XenLab\runs",
    [string]$ListenUrl = "udp://0.0.0.0:5000",
    [string]$NdiSourceName = "Auto"
)

$ErrorActionPreference = "Stop"

if ($CaptureBackend -ne "xudp_jpeg" -and
    $Scenario -notin @("GeometryStatic", "Shuttle", "SoakFreeRun")) {
    throw "UDP/NDI 只运行 GeometryStatic、Shuttle 和 SoakFreeRun 三个对照锚点。"
}

$PackageRoot = [System.IO.Path]::GetFullPath($PackageRoot)
$manifestPath = Join-Path $PackageRoot "package-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "便携包清单不存在：$manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([int]$manifest.schema -ne 1 -or -not [bool]$manifest.complete -or
    [string]$manifest.package_type -ne "xen-dual-machine-receiver") {
    throw "便携包清单无效：$manifestPath"
}
if (@($manifest.allowed_capture_backends) -notcontains $CaptureBackend) {
    throw "便携包不允许 Capture 后端：$CaptureBackend"
}

$modelPath = Join-Path $PackageRoot ([string]$manifest.model.relative_path)
$receiverScript = Join-Path $PackageRoot `
    "scripts\benchmark_network_receiver.ps1"
foreach ($path in @($modelPath, $receiverScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "便携包缺少运行输入：$path"
    }
}

$isSoak = $Scenario -eq "SoakFreeRun"
$minimumSamples = if ($isSoak) { 10000 } else { 3000 }
$minimumSeconds = if ($isSoak) { 300 } else { 60 }
$maximumSeconds = if ($isSoak) { 600 } else { 120 }
$runId = "{0}-{1}-{2}" -f $CaptureBackend, $Scenario,
    (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $ReportRoot, $RunRoot -Force |
    Out-Null
$reportPrefix = Join-Path $ReportRoot $runId
$readyPath = Join-Path $RunRoot "$runId.ready.json"

$arguments = @{
    ModelPath = $modelPath
    ReportPrefix = $reportPrefix
    CaptureBackend = $CaptureBackend
    Backend = "cpu"
    BuildDirectory = $PackageRoot
    PackageManifestPath = $manifestPath
    Configuration = "Release"
    ListenUrl = $ListenUrl
    NdiSourceName = $NdiSourceName
    SourceWidth = 2560
    SourceHeight = 1440
    EncodedWidth = 320
    EncodedHeight = 320
    RoiWidth = 320
    RoiHeight = 320
    WarmupSamples = 100
    MinimumSamples = $minimumSamples
    MinimumSeconds = $minimumSeconds
    MaximumSeconds = $maximumSeconds
    MaximumSourceDroppedFrames = 0
    MaximumTransportDroppedFrames = 0
    MaximumTransportInvalidPackets = 0
    MaximumRuntimeOverwrittenFrames = 0
    EnableFp16 = "off"
    EnableCudaGraph = "off"
    EnableGpuPreprocess = "off"
    ReadyFilePath = $readyPath
}
if ($Mode -eq "Prepare") {
    $arguments.PrepareOnly = $true
}

Write-Host "双机接收任务："
Write-Host "  package_id=$($manifest.package_id)"
Write-Host "  run_id=$runId"
Write-Host "  scenario=$Scenario"
Write-Host "  capture=$CaptureBackend"
Write-Host "  samples=$minimumSamples, seconds=$minimumSeconds"
Write-Host "  report_prefix=$reportPrefix"
Write-Host "  ready_file=$readyPath"
& $receiverScript @arguments
