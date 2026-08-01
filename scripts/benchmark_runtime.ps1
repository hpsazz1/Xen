param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,
    [string]$ConfigPath = "",
    [ValidateSet("tensorrt", "cuda", "directml", "cpu")]
    [string]$Backend = "tensorrt",
    [ValidateSet("auto", "channel_first", "objectness", "end_to_end")]
    [string]$OutputFormat = "auto",
    [ValidateSet("auto", "desktop_duplication", "udp_mjpeg", "xudp_jpeg", "ndi")]
    [string]$ExpectedCaptureBackend = "auto",
    [string]$BuildDirectory = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateRange(0, 100000)]
    [int]$WarmupSamples = 100,
    [ValidateRange(1, 100000)]
    [int]$MinimumSamples = 10000,
    [ValidateRange(0, 86400)]
    [int]$MinimumSeconds = 300,
    [ValidateRange(1, 86400)]
    [int]$MaximumSeconds = 600,
    [ValidateRange(1, 32768)]
    [int]$ExpectedSourceWidth = 2560,
    [ValidateRange(1, 32768)]
    [int]$ExpectedSourceHeight = 1440,
    [ValidateRange(1, 32768)]
    [int]$ExpectedEncodedWidth = 2560,
    [ValidateRange(1, 32768)]
    [int]$ExpectedEncodedHeight = 1440,
    [ValidateRange(0, 32767)]
    [int]$ExpectedRoiX = 1120,
    [ValidateRange(0, 32767)]
    [int]$ExpectedRoiY = 560,
    [ValidateRange(1, 32768)]
    [int]$ExpectedRoiWidth = 320,
    [ValidateRange(1, 32768)]
    [int]$ExpectedRoiHeight = 320,
    [ValidateRange(0.000001, 32768.0)]
    [double]$ExpectedScaleX = 1.0,
    [ValidateRange(0.000001, 32768.0)]
    [double]$ExpectedScaleY = 1.0,
    [ValidateSet("on", "off")]
    [string]$EnableFp16 = "on",
    [ValidateSet("on", "off")]
    [string]$EnableCudaGraph = "on",
    [ValidateSet("on", "off")]
    [string]$EnableGpuPreprocess = "on",
    [ValidateSet("on", "off")]
    [string]$EnableD3D11CudaInterop = "off",
    [ValidateSet("on", "off")]
    [string]$EnableD3D11DirectMlInterop = "off",
    [string]$ReadyFilePath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build"
}

function Resolve-InputFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Get-FileSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        last_write_utc = $file.LastWriteTimeUtc.ToString("o")
        file_version = $file.VersionInfo.FileVersion
        product_version = $file.VersionInfo.ProductVersion
    }
}

function Get-RuntimeSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputDirectory
    )

    $result = [ordered]@{}
    foreach ($file in @(Get-ChildItem -LiteralPath $OutputDirectory `
            -File -Filter "*.dll" | Sort-Object Name)) {
        $result[$file.Name] = Get-FileSnapshot $file.FullName
    }
    return $result
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $pattern = '^' + [regex]::Escape($Name) + ':[^=]*=(.*)$'
    $entry = Select-String -LiteralPath $CachePath -Pattern $pattern |
        Select-Object -First 1
    if ($null -eq $entry) { return "" }
    return $entry.Matches[0].Groups[1].Value
}

function Assert-RuntimeDllOrigins {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$RuntimeFiles,
        [Parameter(Mandatory = $true)]
        [string]$CMakeCachePath,
        [Parameter(Mandatory = $true)]
        [string]$RequestedBackend
    )

    $ortRoot = Get-CMakeCacheValue $CMakeCachePath "ONNXRUNTIME_ROOT"
    $openCvDir = Get-CMakeCacheValue $CMakeCachePath "OpenCV_DIR"
    $tensorRtRoot = Get-CMakeCacheValue $CMakeCachePath "XEN_TENSORRT_ROOT"
    $cudnnRoot = Get-CMakeCacheValue $CMakeCachePath "XEN_CUDNN_ROOT"
    $cudaRoot = Get-CMakeCacheValue $CMakeCachePath "XEN_CUDA_ROOT"
    $directMlRoot = Get-CMakeCacheValue $CMakeCachePath "XEN_DIRECTML_ROOT"
    $ndiRoot = Get-CMakeCacheValue $CMakeCachePath "XEN_NDI_SDK_ROOT"

    if ([string]::IsNullOrWhiteSpace($ortRoot) -or
        [string]::IsNullOrWhiteSpace($openCvDir)) {
        throw "CMake Cache 缺少 ONNX Runtime 或 OpenCV 根目录。"
    }
    if ($RequestedBackend -eq "tensorrt" -and
        ([string]::IsNullOrWhiteSpace($tensorRtRoot) -or
         [string]::IsNullOrWhiteSpace($cudnnRoot) -or
         [string]::IsNullOrWhiteSpace($cudaRoot))) {
        throw "TensorRT 正式基准的 CMake Cache 缺少 TensorRT/cuDNN/CUDA 根目录。"
    }
    if ($RequestedBackend -eq "cuda" -and
        ([string]::IsNullOrWhiteSpace($cudnnRoot) -or
         [string]::IsNullOrWhiteSpace($cudaRoot))) {
        throw "CUDA 正式基准的 CMake Cache 缺少 cuDNN/CUDA 根目录。"
    }
    if ($RequestedBackend -eq "directml" -and
        [string]::IsNullOrWhiteSpace($directMlRoot)) {
        throw "DirectML 正式基准的 CMake Cache 缺少 DirectML 根目录。"
    }

    $openCvRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $openCvDir "..\..\.."))
    $candidateRoots = @(
        $ortRoot, $openCvRoot, $tensorRtRoot, $cudnnRoot, $cudaRoot,
        $directMlRoot, $ndiRoot
    )
    $resolvedRoots = @($candidateRoots |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            (Test-Path -LiteralPath $_ -PathType Container)
        } |
        ForEach-Object {
            (Resolve-Path -LiteralPath $_).ProviderPath
        } |
        Sort-Object -Unique)

    $targetNames = New-Object 'System.Collections.Generic.HashSet[string]' `
        ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $RuntimeFiles.Keys) {
        $targetNames.Add([string]$name) | Out-Null
    }
    $sourceHashes = @{}
    foreach ($root in $resolvedRoots) {
        foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File `
                -Filter "*.dll" -ErrorAction SilentlyContinue)) {
            if (-not $targetNames.Contains($file.Name)) { continue }
            $key = $file.Name.ToLowerInvariant()
            if (-not $sourceHashes.ContainsKey($key)) {
                $sourceHashes[$key] = New-Object `
                    'System.Collections.Generic.HashSet[string]' `
                    ([System.StringComparer]::OrdinalIgnoreCase)
            }
            $hash = (Get-FileHash -LiteralPath $file.FullName `
                -Algorithm SHA256).Hash
            $sourceHashes[$key].Add($hash) | Out-Null
        }
    }
    foreach ($name in $RuntimeFiles.Keys) {
        $key = ([string]$name).ToLowerInvariant()
        if (-not $sourceHashes.ContainsKey($key) -or
            -not $sourceHashes[$key].Contains($RuntimeFiles[$name].sha256)) {
            throw "部署 DLL 无法匹配本次 CMake 配置的 SDK 来源：$name"
        }
    }

    if ($RequestedBackend -eq "directml") {
        $forbidden = @($RuntimeFiles.Keys | Where-Object {
            $_ -match '^(cudart|cublas|cudnn|nvinfer|nvonnxparser|onnxruntime_providers_(cuda|tensorrt))'
        })
        if ($forbidden.Count -ne 0) {
            throw "DirectML 输出目录混入 NVIDIA 运行库：$($forbidden -join ', ')"
        }
    }
    return $resolvedRoots
}

function Assert-FileSnapshotUnchanged {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Before,
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$After,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ($Before.sha256 -ne $After.sha256 -or
        $Before.length -ne $After.length) {
        throw "$Description 在基准运行期间发生变化：$($Before.path)"
    }
}

function Assert-RuntimeSnapshotUnchanged {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Before,
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$After
    )

    $beforeNames = @($Before.Keys | Sort-Object)
    $afterNames = @($After.Keys | Sort-Object)
    if (($beforeNames -join "`n") -ne ($afterNames -join "`n")) {
        throw "可执行文件目录中的部署 DLL 集合在基准运行期间发生变化。"
    }
    foreach ($name in $beforeNames) {
        Assert-FileSnapshotUnchanged $Before[$name] $After[$name] `
            "部署 DLL $name"
    }
}

function Assert-Near {
    param(
        [double]$Actual,
        [double]$Expected,
        [string]$Description
    )
    if ([double]::IsNaN($Actual) -or
        [double]::IsInfinity($Actual) -or
        [Math]::Abs($Actual - $Expected) -gt 0.000001) {
        throw "$Description 不符合预期：expected=$Expected, actual=$Actual"
    }
}

function Get-OrtProviderProfileSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$RequestedBackend,
        [Parameter(Mandatory = $true)]
        [string]$PublishedPath
    )

    # Windows PowerShell 5 会把 ConvertFrom-Json 返回的顶层数组作为单个
    # pipeline 对象传给 @()。先赋给变量再展开，确保 foreach 收到逐个事件。
    $document = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $events = @($document)
    if ($events.Count -eq 0) {
        throw "ORT profile 没有任何事件：$Path"
    }

    $providerCounts = [ordered]@{}
    $nodeEventCount = 0
    $providerEventCount = 0
    foreach ($event in $events) {
        if ([string]$event.cat -ne "Node") { continue }
        ++$nodeEventCount
        if ($null -eq $event.args) { continue }
        $property = $event.args.PSObject.Properties["provider"]
        if ($null -eq $property -or
            [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            continue
        }
        $provider = [string]$property.Value
        if (-not $providerCounts.Contains($provider)) {
            $providerCounts[$provider] = 0
        }
        $providerCounts[$provider] = [long]$providerCounts[$provider] + 1
        ++$providerEventCount
    }
    if ($nodeEventCount -eq 0 -or $providerEventCount -eq 0) {
        throw "ORT profile 缺少可归属 Provider 的 Node 事件。"
    }

    $expectedProvider = if ($RequestedBackend -eq "tensorrt") {
        "TensorrtExecutionProvider"
    } else {
        "CUDAExecutionProvider"
    }
    $allowedProviders = if ($RequestedBackend -eq "tensorrt") {
        @("TensorrtExecutionProvider", "CUDAExecutionProvider",
          "CPUExecutionProvider")
    } else {
        @("CUDAExecutionProvider", "CPUExecutionProvider")
    }
    if (-not $providerCounts.Contains($expectedProvider) -or
        [long]$providerCounts[$expectedProvider] -le 0) {
        throw "ORT profile 没有预期 Provider 节点：$expectedProvider"
    }
    $unexpected = @($providerCounts.Keys | Where-Object {
        $_ -notin $allowedProviders
    })
    if ($unexpected.Count -ne 0) {
        throw "ORT profile 出现请求链之外的 Provider：$($unexpected -join ', ')"
    }

    $file = Get-FileSnapshot $Path
    # pending 文件通过校验后会原子移动；环境清单必须引用发布后的稳定路径。
    $file["path"] = [System.IO.Path]::GetFullPath($PublishedPath)
    return [ordered]@{
        verified = $true
        expected_provider = $expectedProvider
        event_count = $events.Count
        node_event_count = $nodeEventCount
        provider_event_count = $providerEventCount
        unassigned_node_events = $nodeEventCount - $providerEventCount
        provider_counts = $providerCounts
        file = $file
    }
}

if ($MaximumSeconds -lt $MinimumSeconds) {
    throw "MaximumSeconds 不能小于 MinimumSeconds。"
}
if ($ExpectedRoiX + $ExpectedRoiWidth -gt $ExpectedSourceWidth -or
    $ExpectedRoiY + $ExpectedRoiHeight -gt $ExpectedSourceHeight) {
    throw "期望 ROI 超出主机 FOV。"
}
if ($EnableD3D11CudaInterop -eq "on" -and
    ($Backend -ne "tensorrt" -or $EnableCudaGraph -ne "on" -or
     $EnableGpuPreprocess -ne "on")) {
    throw "D3D11/CUDA 互操作要求 TensorRT、CUDA Graph 和 GPU 前处理全部启用。"
}
if ($EnableD3D11DirectMlInterop -eq "on" -and
    $Backend -ne "directml") {
    throw "D3D11/DirectML 互操作要求严格 DirectML 后端。"
}
if ($EnableD3D11CudaInterop -eq "on" -and
    $EnableD3D11DirectMlInterop -eq "on") {
    throw "D3D11/CUDA 与 D3D11/DirectML 互操作不能同时启用。"
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$ModelPath = Resolve-InputFile $ModelPath "模型文件"
if (-not [string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Resolve-InputFile $ConfigPath "配置文件"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$executable = Resolve-InputFile `
    (Join-Path $BuildDirectory "$Configuration\XenBenchmark.exe") `
    "XenBenchmark"
$outputDirectory = Split-Path -Parent $executable
$cmakeCachePath = Resolve-InputFile `
    (Join-Path $BuildDirectory "CMakeCache.txt") "CMake Cache"
$ReportPrefix = [System.IO.Path]::GetFullPath($ReportPrefix)
$reportDirectory = Split-Path -Parent $ReportPrefix
if ([string]::IsNullOrWhiteSpace($reportDirectory)) {
    throw "ReportPrefix 必须解析到有效父目录。"
}
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
if (-not [string]::IsNullOrWhiteSpace($ReadyFilePath)) {
    $ReadyFilePath = [System.IO.Path]::GetFullPath($ReadyFilePath)
    $readyDirectory = Split-Path -Parent $ReadyFilePath
    if ([string]::IsNullOrWhiteSpace($readyDirectory) -or
        -not (Test-Path -LiteralPath $readyDirectory -PathType Container)) {
        throw "ReadyFilePath 的父目录不存在：$readyDirectory"
    }
    if (Test-Path -LiteralPath $ReadyFilePath) {
        throw "ready-file 目标已存在，拒绝覆盖：$ReadyFilePath"
    }
}

$finalCsv = "$ReportPrefix.csv"
$finalJson = "$ReportPrefix.json"
$finalEnvironment = "$ReportPrefix.environment.json"
$requiresProviderProfile = $Backend -eq "tensorrt" -or $Backend -eq "cuda"
$finalProviderProfile = if ($requiresProviderProfile) {
    "$ReportPrefix.provider-profile.json"
} else {
    ""
}
$finalTargets = @($finalCsv, $finalJson, $finalEnvironment)
if ($requiresProviderProfile) { $finalTargets += $finalProviderProfile }
foreach ($target in $finalTargets) {
    if (Test-Path -LiteralPath $target) {
        throw "正式报告目标已存在，拒绝覆盖：$target"
    }
}

$pendingId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"),
    [guid]::NewGuid().ToString("N")
$pendingPrefix = Join-Path $reportDirectory ".pending-$pendingId"
$pendingCsv = "$pendingPrefix.csv"
$pendingJson = "$pendingPrefix.json"
$pendingEnvironment = "$pendingPrefix.environment.json"
$pendingProviderProfile = if ($requiresProviderProfile) {
    "$pendingPrefix.provider-profile.json"
} else {
    ""
}
$pendingArtifacts = @($pendingCsv, $pendingJson, $pendingEnvironment)
if ($requiresProviderProfile) { $pendingArtifacts += $pendingProviderProfile }
if (-not [string]::IsNullOrWhiteSpace($ReadyFilePath)) {
    $pendingArtifacts += $ReadyFilePath
}

try {
$modelBefore = Get-FileSnapshot $ModelPath
$executableBefore = Get-FileSnapshot $executable
$cmakeCacheBefore = Get-FileSnapshot $cmakeCachePath
$configBefore = if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $null
} else {
    Get-FileSnapshot $ConfigPath
}
$runtimeBefore = Get-RuntimeSnapshot $outputDirectory
if ($runtimeBefore.Count -eq 0) {
    throw "XenBenchmark 输出目录没有部署任何第三方 DLL：$outputDirectory"
}
$sdkRoots = @(Assert-RuntimeDllOrigins `
    $runtimeBefore $cmakeCachePath $Backend)
$enableFp16Value = $EnableFp16 -eq "on"
$enableCudaGraphValue = $EnableCudaGraph -eq "on"
$enableGpuPreprocessValue = $EnableGpuPreprocess -eq "on"
$enableD3D11CudaInteropValue = $EnableD3D11CudaInterop -eq "on"
$enableD3D11DirectMlInteropValue =
    $EnableD3D11DirectMlInterop -eq "on"

$arguments = @(
    "--model", $ModelPath,
    "--backend", $Backend,
    "--output-format", $OutputFormat,
    "--report-prefix", $pendingPrefix,
    "--warmup-samples", [string]$WarmupSamples,
    "--minimum-samples", [string]$MinimumSamples,
    "--minimum-seconds", [string]$MinimumSeconds,
    "--maximum-seconds", [string]$MaximumSeconds,
    "--expect-source", "${ExpectedSourceWidth}x${ExpectedSourceHeight}",
    "--expect-encoded", "${ExpectedEncodedWidth}x${ExpectedEncodedHeight}",
    "--expect-roi", "$ExpectedRoiX,$ExpectedRoiY,$ExpectedRoiWidth,$ExpectedRoiHeight",
    "--expect-scale", "$ExpectedScaleX,$ExpectedScaleY",
    "--fp16", $EnableFp16,
    "--cuda-graph", $EnableCudaGraph,
    "--gpu-preprocess", $EnableGpuPreprocess,
    "--d3d11-cuda-interop", $EnableD3D11CudaInterop,
    "--d3d11-directml-interop", $EnableD3D11DirectMlInterop
)
if ($requiresProviderProfile) {
    $arguments += @("--provider-profile", $pendingProviderProfile)
}
if (-not [string]::IsNullOrWhiteSpace($ConfigPath)) {
    $arguments += @("--config", $ConfigPath)
}
if (-not [string]::IsNullOrWhiteSpace($ReadyFilePath)) {
    $arguments += @("--ready-file", $ReadyFilePath)
}

$startedUtc = [DateTime]::UtcNow
$originalTaskPath = $env:PATH
$exitCode = -1
try {
    # 只保留 Windows 系统目录，确保测试不能借用 SDK PATH；第三方 DLL 必须
    # 已由 CMake 部署到 XenBenchmark.exe 同目录。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    Push-Location $repositoryRoot
    try {
        & $executable @arguments
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
} finally {
    $env:PATH = $originalTaskPath
}
$finishedUtc = [DateTime]::UtcNow
if ($exitCode -ne 0) {
    throw "XenBenchmark 失败，退出码：$exitCode"
}
if (-not [string]::IsNullOrWhiteSpace($ReadyFilePath) -and
    (Test-Path -LiteralPath $ReadyFilePath)) {
    throw "XenBenchmark 退出后仍残留 ready-file，生命周期契约被破坏。"
}
if (-not (Test-Path -LiteralPath $pendingCsv -PathType Leaf) -or
    -not (Test-Path -LiteralPath $pendingJson -PathType Leaf)) {
    throw "XenBenchmark 成功退出但没有生成完整 pending 报告。"
}
$providerProfileSummary = if ($requiresProviderProfile) {
    if (-not (Test-Path -LiteralPath $pendingProviderProfile -PathType Leaf)) {
        throw "GPU 基准成功退出但没有生成 Provider profile。"
    }
    Get-OrtProviderProfileSummary `
        $pendingProviderProfile $Backend $finalProviderProfile
} else {
    [ordered]@{
        verified = $true
        expected_provider = if ($Backend -eq "directml") {
            "DmlExecutionProvider"
        } else {
            "CPUExecutionProvider"
        }
        verification = if ($Backend -eq "directml") {
            "session.disable_cpu_ep_fallback=1"
        } else {
            "CPUExecutionProvider"
        }
    }
}

$modelAfter = Get-FileSnapshot $ModelPath
$executableAfter = Get-FileSnapshot $executable
$cmakeCacheAfter = Get-FileSnapshot $cmakeCachePath
$configAfter = if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $null
} else {
    Get-FileSnapshot $ConfigPath
}
$runtimeAfter = Get-RuntimeSnapshot $outputDirectory
Assert-FileSnapshotUnchanged $modelBefore $modelAfter "模型文件"
Assert-FileSnapshotUnchanged $executableBefore $executableAfter "XenBenchmark"
Assert-FileSnapshotUnchanged $cmakeCacheBefore $cmakeCacheAfter "CMake Cache"
if ($null -ne $configBefore) {
    Assert-FileSnapshotUnchanged $configBefore $configAfter "配置文件"
}
Assert-RuntimeSnapshotUnchanged $runtimeBefore $runtimeAfter

$report = Get-Content -LiteralPath $pendingJson -Raw -Encoding UTF8 |
    ConvertFrom-Json
$expectedProviders = @{
    tensorrt = "TensorrtExecutionProvider"
    cuda = "CUDAExecutionProvider"
    directml = "DmlExecutionProvider"
    cpu = "CPUExecutionProvider"
}
$expectedProvider = $expectedProviders[$Backend]
$captureBackendNames = @{
    desktop_duplication = "DESKTOP_DUPLICATION"
    udp_mjpeg = "UDP_MJPEG"
    xudp_jpeg = "XUDP_JPEG"
    ndi = "NDI"
}
$expectedCaptureName = if ($ExpectedCaptureBackend -eq "auto") {
    ""
} else {
    $captureBackendNames[$ExpectedCaptureBackend]
}
if ($report.schema -ne 6) {
    throw "报告 schema 不是 6：$($report.schema)"
}
if (-not [string]::IsNullOrEmpty($expectedCaptureName) -and
    $report.capture_backend -ne $expectedCaptureName) {
    throw "实际 Capture 后端不符合请求：expected=$expectedCaptureName, report=$($report.capture_backend)"
}
if ($report.provider -ne $expectedProvider -or
    $report.final_snapshot.provider -ne $expectedProvider) {
    throw "实际 Provider 不符合请求：expected=$expectedProvider, report=$($report.provider)"
}
if ($report.sample_count -lt $MinimumSamples -or
    $report.successful_samples -ne $report.sample_count -or
    $report.failed_samples -ne 0 -or
    $report.report_samples_dropped -ne 0 -or
    $report.runtime_samples_dropped -ne 0) {
    throw "报告样本数、失败数或丢弃数不符合正式基准门槛。"
}
$snapshot = $report.final_snapshot
if ($snapshot.runtime_state -ne "STOPPED" -or
    $snapshot.failed_frames -ne 0 -or
    $snapshot.debug_samples_dropped -ne 0 -or
    $snapshot.mouse_commands -ne 0 -or
    $snapshot.output_allowed_by_config -or
    $snapshot.output_armed -or
    -not [string]::IsNullOrEmpty([string]$snapshot.last_error)) {
    throw "最终 Runtime 快照不符合零失败、零物理输入契约。"
}
if ([bool]$snapshot.d3d11_cuda_interop -ne
    $enableD3D11CudaInteropValue) {
    throw "最终 Runtime 互操作状态不符合请求。"
}
if ([bool]$snapshot.d3d11_directml_interop -ne
    $enableD3D11DirectMlInteropValue) {
    throw "最终 Runtime DirectML 互操作状态不符合请求。"
}
if ($snapshot.source_width -ne $ExpectedSourceWidth -or
    $snapshot.source_height -ne $ExpectedSourceHeight -or
    $snapshot.encoded_width -ne $ExpectedEncodedWidth -or
    $snapshot.encoded_height -ne $ExpectedEncodedHeight -or
    $snapshot.roi_x -ne $ExpectedRoiX -or
    $snapshot.roi_y -ne $ExpectedRoiY -or
    $snapshot.roi_width -ne $ExpectedRoiWidth -or
    $snapshot.roi_height -ne $ExpectedRoiHeight) {
    throw "最终 Runtime 几何不符合声明契约。"
}
Assert-Near $snapshot.source_pixels_per_pixel_x $ExpectedScaleX `
    "最终 X 比例"
Assert-Near $snapshot.source_pixels_per_pixel_y $ExpectedScaleY `
    "最终 Y 比例"

$sampleIndex = 0
$expectExplicitDeviceCopy =
    $Backend -eq "tensorrt" -and $enableCudaGraphValue
$expectGpuPreprocess = (
    ($expectExplicitDeviceCopy -and $enableGpuPreprocessValue) -or
    $enableD3D11DirectMlInteropValue)
foreach ($sample in @($report.samples)) {
    if (-not $sample.success -or
        $sample.detection_status -ne "SUCCESS" -or
        $sample.aim_status -ne "SUCCESS" -or
        $sample.mouse_sent -or
        $sample.source_width -ne $ExpectedSourceWidth -or
        $sample.source_height -ne $ExpectedSourceHeight -or
        $sample.encoded_width -ne $ExpectedEncodedWidth -or
        $sample.encoded_height -ne $ExpectedEncodedHeight -or
        $sample.roi_x -ne $ExpectedRoiX -or
        $sample.roi_y -ne $ExpectedRoiY -or
        $sample.roi_width -ne $ExpectedRoiWidth -or
        $sample.roi_height -ne $ExpectedRoiHeight) {
        throw "第 $sampleIndex 个正式样本违反状态、物理输入或几何契约。"
    }
    if ([bool]$sample.explicit_device_copy -ne $expectExplicitDeviceCopy) {
        throw "第 $sampleIndex 个样本的显式设备复制语义不符合 CUDA Graph 配置。"
    }
    if ([bool]$sample.gpu_preprocess -ne $expectGpuPreprocess) {
        throw "第 $sampleIndex 个样本的 GPU 前处理语义不符合请求。"
    }
    if ([bool]$sample.d3d11_cuda_interop -ne
        $enableD3D11CudaInteropValue) {
        throw "第 $sampleIndex 个样本的 D3D11/CUDA 互操作语义不符合请求。"
    }
    if ([bool]$sample.d3d11_directml_interop -ne
        $enableD3D11DirectMlInteropValue) {
        throw "第 $sampleIndex 个样本的 D3D11/DirectML 互操作语义不符合请求。"
    }
    if ($enableD3D11CudaInteropValue) {
        $expectedDeviceCopyBytes = [uint64]$ExpectedRoiWidth *
            [uint64]$ExpectedRoiHeight * 4
        if ([uint64]$sample.input_upload_bytes -ne 0 -or
            [uint64]$sample.input_device_copy_bytes -ne
                $expectedDeviceCopyBytes) {
            throw "第 $sampleIndex 个互操作样本违反零 host upload 或设备复制字节契约。"
        }
    } elseif ($enableD3D11DirectMlInteropValue) {
        if ([uint64]$sample.input_upload_bytes -ne 0 -or
            [uint64]$sample.input_device_copy_bytes -ne 0) {
            throw "第 $sampleIndex 个 DirectML 互操作样本违反零 host upload/零中间设备复制契约。"
        }
    } elseif ($expectGpuPreprocess -and $sample.input_upload_bytes -le 0) {
        throw "第 $sampleIndex 个 CPU BGR GPU 前处理样本缺少 H2D 上传字节。"
    }
    Assert-Near $sample.source_pixels_per_pixel_x $ExpectedScaleX `
        "第 $sampleIndex 个样本 X 比例"
    Assert-Near $sample.source_pixels_per_pixel_y $ExpectedScaleY `
        "第 $sampleIndex 个样本 Y 比例"
    ++$sampleIndex
}
if ($sampleIndex -ne $report.sample_count) {
    throw "JSON 样本数组长度与 sample_count 不一致。"
}

$gitCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "读取 Git commit 失败。" }
$gitStatus = @(& git -C $repositoryRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw "读取 Git 工作树状态失败。" }
$gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{
        name = $_.Name
        driver_version = $_.DriverVersion
        adapter_ram = [long]$_.AdapterRAM
    }
})
$environment = [ordered]@{
    schema = 1
    complete = $true
    started_utc = $startedUtc.ToString("o")
    finished_utc = $finishedUtc.ToString("o")
    duration_seconds = ($finishedUtc - $startedUtc).TotalSeconds
    git = [ordered]@{
        commit = $gitCommit
        dirty = $gitStatus.Count -ne 0
        status = $gitStatus
    }
    build = [ordered]@{
        directory = $BuildDirectory
        configuration = $Configuration
        cmake_cache = $cmakeCacheAfter
        sdk_roots = $sdkRoots
        executable = $executableAfter
        runtime_dlls = $runtimeAfter
    }
    inputs = [ordered]@{
        model = $modelAfter
        config = $configAfter
    }
    hardware = [ordered]@{
        computer_name = $env:COMPUTERNAME
        os = (Get-CimInstance Win32_OperatingSystem).Caption
        gpu = $gpu
    }
    benchmark = [ordered]@{
        backend = $Backend
        provider = $expectedProvider
        coordination = [ordered]@{
            ready_file_enabled = -not [string]::IsNullOrWhiteSpace(
                $ReadyFilePath)
            ready_file_absent_after_benchmark =
                [string]::IsNullOrWhiteSpace($ReadyFilePath) -or
                -not (Test-Path -LiteralPath $ReadyFilePath)
        }
        capture = [ordered]@{
            requested = $ExpectedCaptureBackend
            actual = [string]$report.capture_backend
            source_dropped_frames = [long]$snapshot.source_dropped_frames
            duplication_recoveries = [long]$snapshot.duplication_recoveries
            transport_dropped_frames = [long]$snapshot.transport_dropped_frames
            transport_invalid_packets = [long]$snapshot.transport_invalid_packets
            source_received_frames = [long]$snapshot.source_received_frames
            overwritten_frames = [long]$snapshot.overwritten_frames
            capture_fps = [double]$snapshot.capture_fps
            source_fps = [double]$snapshot.source_fps
        }
        provider_execution = [ordered]@{
            node_assignment_verified = [bool]$providerProfileSummary.verified
            cpu_fallback_disabled = $Backend -eq "directml"
            node_fallback_policy = switch ($Backend) {
                "tensorrt" { "TensorRT -> CUDA -> CPU" }
                "cuda" { "CUDA -> CPU" }
                "directml" { "DirectML only" }
                "cpu" { "CPU only" }
            }
            verification = switch ($Backend) {
                "tensorrt" { "independent ORT profiling session" }
                "cuda" { "independent ORT profiling session" }
                "directml" { "session.disable_cpu_ep_fallback=1" }
                "cpu" { "CPUExecutionProvider" }
            }
            profile = $providerProfileSummary
        }
        output_format = $OutputFormat
        warmup_samples = $WarmupSamples
        minimum_samples = $MinimumSamples
        minimum_seconds = $MinimumSeconds
        maximum_seconds = $MaximumSeconds
        fp16 = $enableFp16Value
        cuda_graph = $enableCudaGraphValue
        gpu_preprocess = $enableGpuPreprocessValue
        d3d11_cuda_interop = $enableD3D11CudaInteropValue
        d3d11_directml_interop = $enableD3D11DirectMlInteropValue
        expected_explicit_device_copy = $expectExplicitDeviceCopy
        detector_intra_threads = 0
        detector_inter_threads = 0
        runtime_queue_policy = "latest-only"
        runtime_debug_ring_capacity = 4096
        report_capacity = 100000
        expected_geometry = [ordered]@{
            source_width = $ExpectedSourceWidth
            source_height = $ExpectedSourceHeight
            encoded_width = $ExpectedEncodedWidth
            encoded_height = $ExpectedEncodedHeight
            roi_x = $ExpectedRoiX
            roi_y = $ExpectedRoiY
            roi_width = $ExpectedRoiWidth
            roi_height = $ExpectedRoiHeight
            source_pixels_per_pixel_x = $ExpectedScaleX
            source_pixels_per_pixel_y = $ExpectedScaleY
        }
    }
    report = [ordered]@{
        csv_sha256 = (Get-FileHash -LiteralPath $pendingCsv `
            -Algorithm SHA256).Hash
        json_sha256 = (Get-FileHash -LiteralPath $pendingJson `
            -Algorithm SHA256).Hash
        sample_count = [long]$report.sample_count
        total = $report.timing.total
    }
}
$environmentText = $environment | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText(
    $pendingEnvironment, $environmentText,
    [System.Text.UTF8Encoding]::new($false))

$published = New-Object System.Collections.Generic.List[string]
try {
    [System.IO.File]::Move($pendingCsv, $finalCsv)
    $published.Add($finalCsv)
    [System.IO.File]::Move($pendingJson, $finalJson)
    $published.Add($finalJson)
    if ($requiresProviderProfile) {
        [System.IO.File]::Move(
            $pendingProviderProfile, $finalProviderProfile)
        $published.Add($finalProviderProfile)
    }
    # 环境清单最后发布；只有它存在且 complete=true，整组报告才可视为有效。
    [System.IO.File]::Move($pendingEnvironment, $finalEnvironment)
    $published.Add($finalEnvironment)
} catch {
    foreach ($path in $published) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    throw
} finally {
    foreach ($path in $pendingArtifacts) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Runtime 正式基准通过："
Write-Host "  samples=$($report.sample_count), provider=$expectedProvider"
Write-Host "  total P50=$($report.timing.total.p50_ms) ms"
Write-Host "  total P95=$($report.timing.total.p95_ms) ms"
Write-Host "  total P99=$($report.timing.total.p99_ms) ms"
Write-Host "  CSV：$finalCsv"
Write-Host "  JSON：$finalJson"
if ($requiresProviderProfile) {
    Write-Host "  Provider profile：$finalProviderProfile"
}
Write-Host "  环境清单：$finalEnvironment"
} finally {
    # 任一校验或 XenBenchmark 失败都只能留下日志；pending 报告不构成
    # 有效结果，必须在最外层统一清理。
    foreach ($path in $pendingArtifacts) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}
