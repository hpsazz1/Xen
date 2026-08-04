param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,
    [ValidateSet("udp_mjpeg", "xudp_jpeg", "ndi")]
    [string]$CaptureBackend = "xudp_jpeg",
    [ValidateSet("tensorrt", "cuda", "directml", "cpu")]
    [string]$Backend = "tensorrt",
    [ValidateSet("auto", "channel_first", "objectness", "end_to_end")]
    [string]$OutputFormat = "auto",
    [string]$BuildDirectory = "",
    [string]$PackageManifestPath = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$ListenUrl = "udp://0.0.0.0:5000",
    [string]$NdiSourceName = "Auto",
    [ValidateSet("on", "off")]
    [string]$NdiRequireFrameMetadata = "off",
    [ValidateRange(1, 32768)]
    [int]$SourceWidth = 2560,
    [ValidateRange(1, 32768)]
    [int]$SourceHeight = 1440,
    [ValidateRange(1, 32768)]
    [int]$EncodedWidth = 320,
    [ValidateRange(1, 32768)]
    [int]$EncodedHeight = 320,
    [ValidateRange(1, 32768)]
    [int]$RoiWidth = 320,
    [ValidateRange(1, 32768)]
    [int]$RoiHeight = 320,
    [ValidateRange(0, 100000)]
    [int]$WarmupSamples = 100,
    [ValidateRange(1, 100000)]
    [int]$MinimumSamples = 10000,
    [ValidateRange(0, 86400)]
    [int]$MinimumSeconds = 300,
    [ValidateRange(1, 86400)]
    [int]$MaximumSeconds = 600,
    [ValidateRange(0, 1000000000)]
    [uint64]$MaximumSourceDroppedFrames = 0,
    [ValidateRange(0, 1000000000)]
    [uint64]$MaximumTransportDroppedFrames = 0,
    [ValidateRange(0, 1000000000)]
    [uint64]$MaximumTransportInvalidPackets = 0,
    [ValidateRange(0, 1000000000)]
    [uint64]$MaximumRuntimeOverwrittenFrames = 0,
    [ValidateSet("auto", "on", "off")]
    [string]$EnableFp16 = "auto",
    [ValidateSet("auto", "on", "off")]
    [string]$EnableCudaGraph = "auto",
    [ValidateSet("auto", "on", "off")]
    [string]$EnableGpuPreprocess = "auto",
    [ValidateSet("on", "off")]
    [string]$EnablePerformanceProbes = "off",
    [string]$ReadyFilePath = "",
    [switch]$PrepareOnly
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    if ([string]::IsNullOrWhiteSpace($PackageManifestPath)) {
        $BuildDirectory = Join-Path $PSScriptRoot "..\build"
    } else {
        $BuildDirectory = Split-Path -Parent (
            [System.IO.Path]::GetFullPath($PackageManifestPath))
    }
}

function Get-FileEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
    }
}

function Remove-OwnedOutputs {
    param([string[]]$Paths)
    foreach ($path in $Paths) {
        if (-not [string]::IsNullOrWhiteSpace($path)) {
            Remove-Item -LiteralPath $path -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

if ($ListenUrl.IndexOfAny([char[]]"`r`n") -ge 0 -or
    $NdiSourceName.IndexOfAny([char[]]"`r`n") -ge 0) {
    throw "网络地址和 NDI 源名称不得包含换行。"
}
if ($CaptureBackend -ne "ndi" -and
    $ListenUrl -notmatch '^udp://[^:]+:[0-9]+$') {
    throw "ListenUrl 必须是 udp://IPv4或主机名:端口。"
}
$roiX = [int](($SourceWidth - $RoiWidth) / 2)
$roiY = [int](($SourceHeight - $RoiHeight) / 2)
if ($SourceWidth -lt $RoiWidth -or $SourceHeight -lt $RoiHeight -or
    $roiX * 2 + $RoiWidth -ne $SourceWidth -or
    $roiY * 2 + $RoiHeight -ne $SourceHeight) {
    throw "当前正式网络脚本只接受可精确居中的主机 ROI。"
}
if ($EncodedWidth -ne $RoiWidth -or $EncodedHeight -ne $RoiHeight) {
    throw "当前脚本固定 1:1 中心裁剪，encoded 必须等于 ROI 尺寸。"
}

$ReportPrefix = [System.IO.Path]::GetFullPath($ReportPrefix)
$reportDirectory = Split-Path -Parent $ReportPrefix
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
if ([string]::IsNullOrWhiteSpace($ReadyFilePath)) {
    $ReadyFilePath = "$ReportPrefix.ready-$([guid]::NewGuid().ToString('N')).json"
} else {
    $ReadyFilePath = [System.IO.Path]::GetFullPath($ReadyFilePath)
}
$readyDirectory = Split-Path -Parent $ReadyFilePath
if ([string]::IsNullOrWhiteSpace($readyDirectory)) {
    throw "ReadyFilePath 必须解析到有效父目录。"
}
New-Item -ItemType Directory -Path $readyDirectory -Force | Out-Null
if (Test-Path -LiteralPath $ReadyFilePath) {
    throw "ready-file 目标已存在，拒绝覆盖：$ReadyFilePath"
}
$receiverConfig = "$ReportPrefix.receiver.ini"
$networkMarker = "$ReportPrefix.network.json"
$pendingNetworkMarker = "$networkMarker.pending-$([guid]::NewGuid().ToString('N'))"
$profilePath = "$ReportPrefix.provider-profile.json"
$ownedOutputs = @(
    "$ReportPrefix.csv",
    "$ReportPrefix.json",
    "$ReportPrefix.environment.json",
    $profilePath,
    $networkMarker,
    $pendingNetworkMarker,
    $ReadyFilePath
)
$finalOutputs = @(
    "$ReportPrefix.csv",
    "$ReportPrefix.json",
    "$ReportPrefix.environment.json",
    $profilePath,
    $networkMarker
)
$runtimeReportPublished = $false
foreach ($path in $finalOutputs) {
    if (Test-Path -LiteralPath $path) {
        throw "网络基准目标已存在，拒绝覆盖：$path"
    }
}

$ndiMetadata = if ($NdiRequireFrameMetadata -eq "on") { "true" } else { "false" }
$configLines = @(
    "[capture]",
    "backend=$CaptureBackend",
    "udp_url=$ListenUrl",
    "udp_read_timeout_ms=250",
    "udp_disconnect_timeout_ms=2000",
    "udp_frame_layout=center_crop_1_to_1",
    "udp_source_width=$SourceWidth",
    "udp_source_height=$SourceHeight",
    "ndi_source_name=$NdiSourceName",
    "ndi_discovery_timeout_ms=5000",
    "ndi_receive_timeout_ms=50",
    "ndi_disconnect_timeout_ms=2000",
    "ndi_frame_layout=center_crop_1_to_1",
    "ndi_source_width=$SourceWidth",
    "ndi_source_height=$SourceHeight",
    "ndi_require_frame_metadata=$ndiMetadata",
    "roi_width=$RoiWidth",
    "roi_height=$RoiHeight",
    "center_roi=true",
    "roi_x=0",
    "roi_y=0",
    "acquire_timeout_ms=16",
    ""
)
$configText = $configLines -join "`r`n"
if (Test-Path -LiteralPath $receiverConfig) {
    $existingConfig = [System.IO.File]::ReadAllText($receiverConfig)
    if ($existingConfig -ne $configText) {
        throw "既有接收配置与本次参数不一致，拒绝覆盖：$receiverConfig"
    }
} else {
    [System.IO.File]::WriteAllText(
        $receiverConfig, $configText,
        [System.Text.UTF8Encoding]::new($false))
}

if ($PrepareOnly) {
    Write-Host "网络接收配置已准备：$receiverConfig"
    Write-Host "主机 FOV=${SourceWidth}x${SourceHeight}, ROI=($roiX,$roiY,${RoiWidth}x${RoiHeight})"
    return
}

$resolvedFp16 = if ($EnableFp16 -eq "auto") {
    if ($Backend -eq "directml" -or $Backend -eq "cpu") { "off" } else { "on" }
} else { $EnableFp16 }
$resolvedGraph = if ($EnableCudaGraph -eq "auto") {
    if ($Backend -eq "tensorrt") { "on" } else { "off" }
} else { $EnableCudaGraph }
$resolvedGpuPreprocess = if ($EnableGpuPreprocess -eq "auto") {
    if ($Backend -eq "tensorrt") { "on" } else { "off" }
} else { $EnableGpuPreprocess }

$benchmarkScript = Join-Path $PSScriptRoot "benchmark_runtime.ps1"
try {
    & $benchmarkScript `
        -ModelPath $ModelPath `
        -ReportPrefix $ReportPrefix `
        -ConfigPath $receiverConfig `
        -Backend $Backend `
        -OutputFormat $OutputFormat `
        -ExpectedCaptureBackend $CaptureBackend `
        -BuildDirectory $BuildDirectory `
        -PackageManifestPath $PackageManifestPath `
        -Configuration $Configuration `
        -WarmupSamples $WarmupSamples `
        -MinimumSamples $MinimumSamples `
        -MinimumSeconds $MinimumSeconds `
        -MaximumSeconds $MaximumSeconds `
        -ExpectedSourceWidth $SourceWidth `
        -ExpectedSourceHeight $SourceHeight `
        -ExpectedEncodedWidth $EncodedWidth `
        -ExpectedEncodedHeight $EncodedHeight `
        -ExpectedRoiX $roiX `
        -ExpectedRoiY $roiY `
        -ExpectedRoiWidth $RoiWidth `
        -ExpectedRoiHeight $RoiHeight `
        -ExpectedScaleX 1 `
        -ExpectedScaleY 1 `
        -EnableFp16 $resolvedFp16 `
        -EnableCudaGraph $resolvedGraph `
        -EnableGpuPreprocess $resolvedGpuPreprocess `
        -EnablePerformanceProbes $EnablePerformanceProbes `
        -ReadyFilePath $ReadyFilePath

    $reportPath = "$ReportPrefix.json"
    $environmentPath = "$ReportPrefix.environment.json"
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $expectedPerformanceProbes = $EnablePerformanceProbes -eq "on"
    $reportFields = @($report.PSObject.Properties.Name)
    if ([int]$report.schema -ne 7) {
        throw "网络基准 Runtime 报告 schema 不是 7：$($report.schema)"
    }
    if ([bool]$report.performance_probes_enabled -ne
            $expectedPerformanceProbes) {
        throw "网络基准 Runtime 报告性能探针状态不符合请求。"
    }
    if ($reportFields -notcontains "coverage" -or
        $reportFields -notcontains "ndi_video_queue_depth") {
        throw "网络基准 Runtime 报告缺少覆盖分段或 NDI 视频队列深度证据。"
    }
    $snapshot = $report.final_snapshot
    $runtimeReportPublished = $true
    if ([uint64]$snapshot.source_dropped_frames -gt
            $MaximumSourceDroppedFrames -or
        [uint64]$snapshot.transport_dropped_frames -gt
            $MaximumTransportDroppedFrames -or
        [uint64]$snapshot.transport_invalid_packets -gt
            $MaximumTransportInvalidPackets -or
        [uint64]$snapshot.overwritten_frames -gt
            $MaximumRuntimeOverwrittenFrames) {
        throw (("网络门禁未通过：source_dropped_frames={0}/{1}, " +
            "transport_dropped_frames={2}/{3}, " +
            "transport_invalid_packets={4}/{5}, " +
            "runtime_overwritten_frames={6}/{7}。Runtime 报告已保留，" +
            "网络完成标记不发布。") -f
            [uint64]$snapshot.source_dropped_frames,
            $MaximumSourceDroppedFrames,
            [uint64]$snapshot.transport_dropped_frames,
            $MaximumTransportDroppedFrames,
            [uint64]$snapshot.transport_invalid_packets,
            $MaximumTransportInvalidPackets,
            [uint64]$snapshot.overwritten_frames,
            $MaximumRuntimeOverwrittenFrames)
    }
    if (($CaptureBackend -eq "xudp_jpeg" -or
         $CaptureBackend -eq "ndi") -and
        [uint64]$snapshot.source_received_frames -eq 0) {
        throw "版本化网络后端没有发布源端接收计数。"
    }

    $marker = [ordered]@{
        schema = 1
        complete = $true
        role = "receiver"
        capture_backend = $CaptureBackend
        display_resolution_is_not_coordinate_input = $true
        coordination = [ordered]@{
            ready_file_used = $true
            ready_file_absent_after_benchmark =
                -not (Test-Path -LiteralPath $ReadyFilePath)
        }
        geometry = [ordered]@{
            source_width = $SourceWidth
            source_height = $SourceHeight
            encoded_width = $EncodedWidth
            encoded_height = $EncodedHeight
            roi_x = $roiX
            roi_y = $roiY
            roi_width = $RoiWidth
            roi_height = $RoiHeight
            source_pixels_per_pixel_x = 1.0
            source_pixels_per_pixel_y = 1.0
        }
        transport = [ordered]@{
            source_received_frames = [uint64]$snapshot.source_received_frames
            source_dropped_frames = [uint64]$snapshot.source_dropped_frames
            transport_dropped_frames = [uint64]$snapshot.transport_dropped_frames
            transport_invalid_packets = [uint64]$snapshot.transport_invalid_packets
            runtime_overwritten_frames = [uint64]$snapshot.overwritten_frames
            capture_fps = [double]$snapshot.capture_fps
            source_fps = [double]$snapshot.source_fps
            cross_machine_clock_basis = if ($CaptureBackend -eq "xudp_jpeg") {
                "unsynchronized_monotonic_clocks; no strict frame age"
            } else {
                "backend local timing only"
            }
        }
        thresholds = [ordered]@{
            maximum_source_dropped_frames = $MaximumSourceDroppedFrames
            maximum_transport_dropped_frames = $MaximumTransportDroppedFrames
            maximum_transport_invalid_packets = $MaximumTransportInvalidPackets
            maximum_runtime_overwritten_frames = $MaximumRuntimeOverwrittenFrames
        }
        measurement = [ordered]@{
            runtime_report_schema = [int]$report.schema
            performance = [ordered]@{
                probes_enabled = [bool]$report.performance_probes_enabled
                coverage = $report.coverage
                ndi_video_queue_depth = $report.ndi_video_queue_depth
            }
        }
        artifacts = [ordered]@{
            receiver_config = Get-FileEvidence $receiverConfig
            runtime_report = Get-FileEvidence $reportPath
            runtime_environment = Get-FileEvidence $environmentPath
            provider_profile = if (Test-Path -LiteralPath $profilePath) {
                Get-FileEvidence $profilePath
            } else { $null }
        }
    }
    $markerText = $marker | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText(
        $pendingNetworkMarker, $markerText,
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Move($pendingNetworkMarker, $networkMarker)
    Write-Host "网络接收正式基准通过：$networkMarker"
} catch {
    # Runtime 报告已按自身契约原子发布后，即使网络门禁拒绝，也必须保留
    # 完整采样证据供定位具体丢弃来源；缺少 network.json 明确表示未正式通过。
    if (-not $runtimeReportPublished) {
        Remove-OwnedOutputs $ownedOutputs
    }
    throw
} finally {
    Remove-Item -LiteralPath $pendingNetworkMarker -Force `
        -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $ReadyFilePath -Force `
        -ErrorAction SilentlyContinue
}
