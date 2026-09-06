param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,
    [string]$ConfigPath = "",
    [ValidateSet("tensorrt", "cuda", "directml", "openvino", "cpu")]
    [string]$Backend = "tensorrt",
    [ValidateSet("gpu", "cpu", "npu")]
    [string]$OpenVinoDevice = "gpu",
    [ValidateSet("auto", "channel_first", "objectness", "end_to_end")]
    [string]$OutputFormat = "auto",
    [ValidateSet("auto", "desktop_duplication", "udp_mjpeg", "xudp_jpeg", "ndi")]
    [string]$ExpectedCaptureBackend = "auto",
    [string]$BuildDirectory = "",
    [string]$PackageManifestPath = "",
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
    [string]$EnablePerformanceProbes = "off",
    [ValidateSet("on", "off")]
    [string]$EnableD3D11CudaInterop = "off",
    [ValidateSet("on", "off")]
    [string]$EnableD3D11DirectMlInterop = "off",
    [ValidateSet("on", "off")]
    [string]$ExpectedAimPrediction = "off",
    [ValidateRange(1.0, 50.0)]
    [double]$ExpectedAimMaxPredictionLeadPercent = 35.0,
    [ValidateRange(0, 1000000000)]
    [uint64]$MinimumAimPrecomputedCommandFrames = 0,
    [string]$ReadyFilePath = ""
)

$ErrorActionPreference = "Stop"
$reportRetentionCapacity = 100000

$environmentScript = Join-Path $PSScriptRoot "runtime_environment.ps1"
if (-not (Test-Path -LiteralPath $environmentScript -PathType Leaf)) {
    throw "Runtime 环境采集脚本不存在：$environmentScript"
}
. $environmentScript

$aimReportScript = Join-Path $PSScriptRoot "aim_report.ps1"
if (-not (Test-Path -LiteralPath $aimReportScript -PathType Leaf)) {
    throw "Aim 报告校验脚本不存在：$aimReportScript"
}
. $aimReportScript

$sequenceScript = Join-Path $PSScriptRoot "runtime_report_sequence.ps1"
if (-not (Test-Path -LiteralPath $sequenceScript -PathType Leaf)) {
    throw "Runtime sequence 校验脚本不存在：$sequenceScript"
}
. $sequenceScript

if ([string]::IsNullOrWhiteSpace($BuildDirectory) -and
    [string]::IsNullOrWhiteSpace($PackageManifestPath)) {
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

function Get-RelativePackagePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$PackageRoot
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetFullPath($PackageRoot).TrimEnd('\', '/')
    $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "文件不在便携包根目录内：$fullPath"
    }
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Assert-PortablePackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ManifestPath,
        [Parameter(Mandatory = $true)]
        [string]$PackageRoot,
        [Parameter(Mandatory = $true)]
        [string]$RequestedConfiguration,
        [Parameter(Mandatory = $true)]
        [string]$RequestedBackend
    )

    try {
        $document = Get-Content -LiteralPath $ManifestPath -Raw `
            -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "便携包清单不是有效 JSON：$ManifestPath`n$($_.Exception.Message)"
    }
    if ([int]$document.schema -ne 1 -or -not [bool]$document.complete -or
        [string]$document.package_type -ne "xen-dual-machine-receiver") {
        throw "便携包清单 schema、完成状态或类型无效：$ManifestPath"
    }
    if ([string]$document.configuration -ne $RequestedConfiguration) {
        throw "便携包配置不符合请求：$($document.configuration)"
    }
    $gitCommit = [string]$document.source.git_commit
    if ($gitCommit -notmatch '^[0-9a-fA-F]{40}$' -or
        [bool]$document.source.git_dirty) {
        throw "便携包必须来自明确且干净的 Git 提交。"
    }
    $allowedBackends = @($document.allowed_backends | ForEach-Object {
        [string]$_
    })
    if ($allowedBackends -notcontains $RequestedBackend) {
        throw "便携包不允许请求的 Provider：$RequestedBackend"
    }

    $packageRoot = [System.IO.Path]::GetFullPath($PackageRoot).TrimEnd(
        '\', '/')
    $rootPrefix = $packageRoot + [System.IO.Path]::DirectorySeparatorChar
    $reportedPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $snapshots = [ordered]@{}
    foreach ($record in @($document.files)) {
        $relativePath = ([string]$record.relative_path).Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relativePath) -or
            [System.IO.Path]::IsPathRooted($relativePath) -or
            $relativePath -match '(^|/)\.\.(/|$)' -or
            -not $reportedPaths.Add($relativePath)) {
            throw "便携包清单包含非法或重复相对路径：$relativePath"
        }
        $fullPath = [System.IO.Path]::GetFullPath(
            (Join-Path $packageRoot $relativePath))
        if (-not $fullPath.StartsWith(
                $rootPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            throw "便携包文件缺失或越界：$relativePath"
        }
        $snapshot = Get-FileSnapshot $fullPath
        if ([long]$record.length -ne [long]$snapshot.length -or
            [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
            [string]$record.sha256 -ne [string]$snapshot.sha256) {
            throw "便携包文件长度或 SHA-256 不一致：$relativePath"
        }
        $snapshots[$relativePath] = $snapshot
    }
    if ($reportedPaths.Count -eq 0) {
        throw "便携包清单没有文件记录。"
    }

    $requiredPaths = @(
        "$RequestedConfiguration/XenBenchmark.exe",
        "$RequestedConfiguration/xen-runtime-deployment.json",
        "scripts/benchmark_runtime.ps1",
        "scripts/benchmark_network_receiver.ps1",
        "scripts/invoke_dual_machine_receiver.ps1",
        "scripts/aim_report.ps1",
        "scripts/runtime_report_sequence.ps1",
        "scripts/runtime_environment.ps1",
        [string]$document.model.relative_path
    )
    foreach ($relativePath in $requiredPaths) {
        if ([string]::IsNullOrWhiteSpace($relativePath) -or
            -not $reportedPaths.Contains($relativePath.Replace('\', '/'))) {
            throw "便携包缺少正式入口或模型记录：$relativePath"
        }
    }

    $actualPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in @(
            (Join-Path $packageRoot $RequestedConfiguration),
            (Join-Path $packageRoot "scripts"))) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "便携包缺少目录：$directory"
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $directory `
                -Recurse -File)) {
            $relativePath = Get-RelativePackagePath `
                $file.FullName $packageRoot
            $actualPaths.Add($relativePath) | Out-Null
        }
    }
    if (($actualPaths.Count -ne $reportedPaths.Count) -or
        @($actualPaths | Where-Object {
            -not $reportedPaths.Contains($_)
        }).Count -ne 0) {
        throw "便携包实际文件集合与清单不一致。"
    }

    return [ordered]@{
        package_id = [string]$document.package_id
        manifest = Get-FileSnapshot $ManifestPath
        git_commit = $gitCommit
        git_dirty = [bool]$document.source.git_dirty
        allowed_backends = $allowedBackends
        model_relative_path = [string]$document.model.relative_path
        file_count = $reportedPaths.Count
        files = $snapshots
    }
}

function Assert-PortableRuntimeDeployment {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputDirectory,
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$RuntimeFiles,
        [Parameter(Mandatory = $true)]
        [string]$RequestedConfiguration
    )

    $reportPath = Join-Path $OutputDirectory "xen-runtime-deployment.json"
    try {
        $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        throw "便携包部署报告不是有效 JSON：$reportPath`n$($_.Exception.Message)"
    }
    if ([int]$report.schema -ne 1 -or
        [string]$report.configuration -ne $RequestedConfiguration) {
        throw "便携包部署报告 schema 或配置无效。"
    }
    $reportedDlls = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($report.files)) {
        $name = [string]$record.name
        if ([System.IO.Path]::GetFileName($name) -ne $name -or
            [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "便携包部署报告包含非法文件记录：$name"
        }
        $deployedPath = Join-Path $OutputDirectory $name
        if (-not (Test-Path -LiteralPath $deployedPath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $deployedPath -Algorithm SHA256).Hash `
                -ne [string]$record.sha256) {
            throw "便携包运行库与主机构建部署报告不一致：$name"
        }
        if ($name.EndsWith(
                ".dll", [System.StringComparison]::OrdinalIgnoreCase)) {
            $reportedDlls.Add($name) | Out-Null
        }
    }
    $actualDlls = @($RuntimeFiles.Keys | Sort-Object)
    if ($actualDlls.Count -ne $reportedDlls.Count -or
        @($actualDlls | Where-Object {
            -not $reportedDlls.Contains([string]$_)
        }).Count -ne 0) {
        throw "便携包 DLL 集合与主机构建部署报告不一致。"
    }
    return [ordered]@{
        verified = $true
        verification = "packaged deployment report + local SHA-256"
        report = Get-FileSnapshot $reportPath
        declared_output_directory = [string]$report.output_directory
        file_count = @($report.files).Count
    }
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
    if ($RequestedBackend -eq "openvino") {
        $forbidden = @($RuntimeFiles.Keys | Where-Object {
            $_ -match '^(DirectML|cudart|cublas|cudnn|nvinfer|nvonnxparser|onnxruntime_providers_(cuda|tensorrt))'
        })
        if ($forbidden.Count -ne 0) {
            throw "OpenVINO 输出目录混入其他 GPU 后端运行库：$($forbidden -join ', ')"
        }
        $required = @(
            "onnxruntime_providers_openvino.dll", "openvino.dll",
            "openvino_onnx_frontend.dll",
            "openvino_intel_cpu_plugin.dll", "openvino_intel_gpu_plugin.dll",
            "openvino_intel_npu_plugin.dll", "tbb12.dll")
        $missing = @($required | Where-Object {
            -not $RuntimeFiles.Contains($_)
        })
        if ($missing.Count -ne 0) {
            throw "OpenVINO 输出目录缺少运行库：$($missing -join ', ')"
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

    $expectedProvider = switch ($RequestedBackend) {
        "tensorrt" { "TensorrtExecutionProvider" }
        "cuda" { "CUDAExecutionProvider" }
        "openvino" { "OpenVINOExecutionProvider" }
        default { throw "不支持 Provider profile 的后端：$RequestedBackend" }
    }
    $allowedProviders = switch ($RequestedBackend) {
        "tensorrt" {
            @("TensorrtExecutionProvider", "CUDAExecutionProvider",
              "CPUExecutionProvider")
        }
        "cuda" { @("CUDAExecutionProvider", "CPUExecutionProvider") }
        "openvino" { @("OpenVINOExecutionProvider") }
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
if ($Backend -ne "openvino" -and $PSBoundParameters.ContainsKey(
        "OpenVinoDevice")) {
    throw "只有 OpenVINO 后端接受 OpenVinoDevice。"
}
if ($EnableD3D11DirectMlInterop -eq "on" -and
    $Backend -ne "directml") {
    throw "D3D11/DirectML 互操作要求严格 DirectML 后端。"
}
if ($EnableD3D11CudaInterop -eq "on" -and
    $EnableD3D11DirectMlInterop -eq "on") {
    throw "D3D11/CUDA 与 D3D11/DirectML 互操作不能同时启用。"
}

$portablePackage = -not [string]::IsNullOrWhiteSpace($PackageManifestPath)
if ($portablePackage) {
    $PackageManifestPath = Resolve-InputFile `
        $PackageManifestPath "便携包清单"
    $packageRoot = Split-Path -Parent $PackageManifestPath
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
        $BuildDirectory = $packageRoot
    }
    $resolvedBuildDirectory = ([System.IO.Path]::GetFullPath(
        $BuildDirectory)).TrimEnd('\', '/')
    $resolvedPackageRoot = ([System.IO.Path]::GetFullPath(
        $packageRoot)).TrimEnd('\', '/')
    if (-not $resolvedBuildDirectory.Equals(
            $resolvedPackageRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "便携包模式要求 BuildDirectory 等于清单所在目录。"
    }
    $repositoryRoot = $resolvedPackageRoot
} else {
    $repositoryRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot ".."))
}
$ModelPath = Resolve-InputFile $ModelPath "模型文件"
if (-not [string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Resolve-InputFile $ConfigPath "配置文件"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$executable = Resolve-InputFile `
    (Join-Path $BuildDirectory "$Configuration\XenBenchmark.exe") `
    "XenBenchmark"
$outputDirectory = Split-Path -Parent $executable
$cmakeCachePath = if ($portablePackage) {
    ""
} else {
    Resolve-InputFile `
        (Join-Path $BuildDirectory "CMakeCache.txt") "CMake Cache"
}
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
$requiresProviderProfile =
    $Backend -eq "tensorrt" -or $Backend -eq "cuda" -or
    $Backend -eq "openvino"
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
$packageBefore = if ($portablePackage) {
    Assert-PortablePackage `
        $PackageManifestPath $repositoryRoot $Configuration $Backend
} else {
    $null
}
$modelBefore = Get-FileSnapshot $ModelPath
$executableBefore = Get-FileSnapshot $executable
$cmakeCacheBefore = if ($portablePackage) {
    $null
} else {
    Get-FileSnapshot $cmakeCachePath
}
$configBefore = if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $null
} else {
    Get-FileSnapshot $ConfigPath
}
$runtimeBefore = Get-RuntimeSnapshot $outputDirectory
if ($runtimeBefore.Count -eq 0) {
    throw "XenBenchmark 输出目录没有部署任何第三方 DLL：$outputDirectory"
}
$deploymentEvidence = if ($portablePackage) {
    Assert-PortableRuntimeDeployment `
        $outputDirectory $runtimeBefore $Configuration
} else {
    $null
}
$sdkRoots = if ($portablePackage) {
    @()
} else {
    @(Assert-RuntimeDllOrigins $runtimeBefore $cmakeCachePath $Backend)
}
$enableFp16Value = $EnableFp16 -eq "on"
$enableCudaGraphValue = $EnableCudaGraph -eq "on"
$enableGpuPreprocessValue = $EnableGpuPreprocess -eq "on"
$enablePerformanceProbesValue = $EnablePerformanceProbes -eq "on"
$enableD3D11CudaInteropValue = $EnableD3D11CudaInterop -eq "on"
$enableD3D11DirectMlInteropValue =
    $EnableD3D11DirectMlInterop -eq "on"
$expectedAimPredictionValue = $ExpectedAimPrediction -eq "on"

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
    "--performance-probes", $EnablePerformanceProbes,
    "--d3d11-cuda-interop", $EnableD3D11CudaInterop,
    "--d3d11-directml-interop", $EnableD3D11DirectMlInterop
)
if ($requiresProviderProfile) {
    $arguments += @("--provider-profile", $pendingProviderProfile)
}
if ($Backend -eq "openvino") {
    $arguments += @("--openvino-device", $OpenVinoDevice)
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
        throw "需要节点级验证的基准没有生成 Provider profile。"
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
$cmakeCacheAfter = if ($portablePackage) {
    $null
} else {
    Get-FileSnapshot $cmakeCachePath
}
$configAfter = if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $null
} else {
    Get-FileSnapshot $ConfigPath
}
$runtimeAfter = Get-RuntimeSnapshot $outputDirectory
Assert-FileSnapshotUnchanged $modelBefore $modelAfter "模型文件"
Assert-FileSnapshotUnchanged $executableBefore $executableAfter "XenBenchmark"
if (-not $portablePackage) {
    Assert-FileSnapshotUnchanged `
        $cmakeCacheBefore $cmakeCacheAfter "CMake Cache"
}
if ($null -ne $configBefore) {
    Assert-FileSnapshotUnchanged $configBefore $configAfter "配置文件"
}
Assert-RuntimeSnapshotUnchanged $runtimeBefore $runtimeAfter
$packageAfter = if ($portablePackage) {
    Assert-PortablePackage `
        $PackageManifestPath $repositoryRoot $Configuration $Backend
} else {
    $null
}
if ($portablePackage) {
    Assert-FileSnapshotUnchanged `
        $packageBefore.manifest $packageAfter.manifest "便携包清单"
}

$report = Get-Content -LiteralPath $pendingJson -Raw -Encoding UTF8 |
    ConvertFrom-Json
$expectedProviders = @{
    tensorrt = "TensorrtExecutionProvider"
    cuda = "CUDAExecutionProvider"
    directml = "DmlExecutionProvider"
    openvino = "OpenVINOExecutionProvider"
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
if ([int]$report.schema -notin @(17, 18)) {
    throw "报告 schema 不是 17 或 18：$($report.schema)"
}
$retention = Get-XenRuntimeReportRetention `
    -Report $report -RetentionCapacity $reportRetentionCapacity
[uint64]$formalSampleCount = $retention.formal_sample_count
[uint64]$retainedSampleCount = $retention.retained_sample_count
[uint64]$omittedSampleCount = $retention.omitted_sample_count
$csvLines = @(Get-Content -LiteralPath $pendingCsv -Encoding UTF8)
$csvRetention = Get-XenRuntimeCsvRetentionMetadata -Lines $csvLines
if ([uint64]$csvRetention.omitted_sample_count -ne $omittedSampleCount) {
    throw "CSV 与 JSON 的 report_samples_dropped 不一致。"
}
if ([bool]$report.performance_probes_enabled -ne
        $enablePerformanceProbesValue) {
    throw "报告性能探针状态不符合请求。"
}
if (-not [string]::IsNullOrEmpty($expectedCaptureName) -and
    $report.capture_backend -ne $expectedCaptureName) {
    throw "实际 Capture 后端不符合请求：expected=$expectedCaptureName, report=$($report.capture_backend)"
}
if ($report.provider -ne $expectedProvider -or
    $report.final_snapshot.provider -ne $expectedProvider) {
    throw "实际 Provider 不符合请求：expected=$expectedProvider, report=$($report.provider)"
}
if ($formalSampleCount -lt $MinimumSamples -or
    [uint64]$report.successful_samples -ne $retainedSampleCount -or
    $report.failed_samples -ne 0 -or
    $report.runtime_samples_dropped -ne 0) {
    throw "报告 formal/留样数、失败数或 Runtime 丢弃数不符合正式基准门槛。"
}
$reportSamples = @($report.samples)
if ([uint64]$reportSamples.Count -ne $retainedSampleCount) {
    throw "JSON 样本数组长度与 sample_count 不一致。"
}
$aimSummary = Get-XenAimReportSummary `
    -Samples $reportSamples `
    -PredictionEnabled $ExpectedAimPrediction `
    -MaxPredictionLeadPercent $ExpectedAimMaxPredictionLeadPercent `
    -RequireMatchedObservation
if (-not [bool]$aimSummary.contract_valid) {
    throw "Aim 逐帧门禁未通过：$($aimSummary.violation_messages -join '；')"
}
if ([uint64]$aimSummary.precomputed_command_frames -lt
        $MinimumAimPrecomputedCommandFrames) {
    throw ("Aim 预计算命令帧不足：required={0}, actual={1}" -f
        $MinimumAimPrecomputedCommandFrames,
        [uint64]$aimSummary.precomputed_command_frames)
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
foreach ($sample in $reportSamples) {
    if (-not $sample.success -or
        [bool]$sample.performance_probes -ne
            $enablePerformanceProbesValue -or
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
$csvDataLines = @($csvLines | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        -not $_.StartsWith("#", [System.StringComparison]::Ordinal)
    })
if ($csvDataLines.Count -lt 2) {
    throw "CSV 缺少表头或正式样本行。"
}
try {
    $csvRows = @($csvDataLines | ConvertFrom-Csv)
} catch {
    throw "CSV 结构化解析失败：$($_.Exception.Message)"
}
if ([uint64]$csvRows.Count -ne $retainedSampleCount) {
    throw "CSV 正式样本行数与 JSON sample_count 不一致。"
}
[uint64[]]$csvSequences = Get-XenRuntimeSequenceValues `
    -JsonSamples $reportSamples -CsvRows $csvRows
$coverage = $report.coverage
if (-not [bool]$coverage.available -or
    [uint64]$coverage.startup.sample_count -ne 1 -or
    [uint64]$coverage.warmup.sample_count -ne $WarmupSamples -or
    [uint64]$coverage.startup.runtime_overwritten_frames -ne
        [uint64]$coverage.warmup_start_overwritten_frames -or
    [uint64]$coverage.warmup.runtime_overwritten_frames -ne
        ([uint64]$coverage.warmup_end_overwritten_frames -
         [uint64]$coverage.warmup_start_overwritten_frames) -or
    [uint64]$coverage.formal.runtime_overwritten_frames -ne
        ([uint64]$coverage.formal_end_overwritten_frames -
         [uint64]$coverage.warmup_end_overwritten_frames)) {
    throw "startup/warmup/formal 覆盖分段或累计边界不闭合。"
}
$previousFormalSequence = if ($WarmupSamples -gt 0) {
    [uint64]$coverage.warmup.last_sequence
} else {
    [uint64]$coverage.startup.last_sequence
}
$sequenceEvidence = Get-XenRuntimeRetainedSequenceEvidence `
    -Sequences $csvSequences `
    -PreviousFormalSequence $previousFormalSequence `
    -OmittedSampleCount $omittedSampleCount
if ([uint64]$sequenceEvidence.last_sequence -ne
        [uint64]$coverage.formal.last_sequence -or
    ($omittedSampleCount -eq 0 -and
     [uint64]$sequenceEvidence.first_sequence -ne
        [uint64]$coverage.formal.first_sequence) -or
    ($omittedSampleCount -gt 0 -and
     [uint64]$sequenceEvidence.first_sequence -le
        [uint64]$coverage.formal.first_sequence) -or
    [uint64]$sequenceEvidence.formal_sequence_gaps -ne
        [uint64]$coverage.formal.sequence_gaps) {
    throw "正式留样尾窗、sequence 缺口与 coverage 交叉统计不一致。"
}
if ([uint64]$snapshot.source_dropped_frames -eq 0) {
    $formalCounterExpected =
        [uint64]$coverage.formal.sequence_gaps +
        [uint64]$coverage.formal.trailing_runtime_overwritten_frames
    if (-not [bool]$coverage.formal.counter_matches_sequence_gaps -or
        [uint64]$coverage.formal.runtime_overwritten_frames -ne
            $formalCounterExpected) {
        throw "源端零丢帧时，正式 Runtime 覆盖与 CSV sequence 缺口/停止尾差不一致。"
    }
}
$probeSummaryValid = if ($enablePerformanceProbesValue) {
    [uint64]$report.timing.pipeline_complete.sample_count -eq
        [uint64]$report.sample_count -and
    [uint64]$report.timing.runtime_handoff.sample_count -eq
        [uint64]$report.sample_count -and
    ($ExpectedCaptureBackend -ne "ndi" -or
     ([uint64]$report.timing.ndi_receive_call.sample_count -eq
          [uint64]$report.sample_count -and
      [uint64]$report.ndi_video_queue_depth.sample_count -gt 0))
} else {
    [uint64]$report.timing.pipeline_complete.sample_count -eq 0 -and
    [uint64]$report.timing.runtime_handoff.sample_count -eq 0 -and
    [uint64]$report.timing.ndi_receive_call.sample_count -eq 0 -and
    [uint64]$report.ndi_video_queue_depth.sample_count -eq 0
}
if (-not $probeSummaryValid) {
    throw "性能探针分段样本数或 NDI queue 采样数不符合请求。"
}

$gitCommit = if ($portablePackage) {
    $packageAfter.git_commit
} else {
    (& git -C $repositoryRoot rev-parse HEAD).Trim()
}
if (-not $portablePackage -and $LASTEXITCODE -ne 0) {
    throw "读取 Git commit 失败。"
}
$gitStatus = if ($portablePackage) {
    @()
} else {
    @(& git -C $repositoryRoot status --porcelain)
}
if (-not $portablePackage -and $LASTEXITCODE -ne 0) {
    throw "读取 Git 工作树状态失败。"
}
$hardwareInventory = Get-XenRuntimeHardwareInventory
$environment = [ordered]@{
    schema = 1
    complete = $true
    started_utc = $startedUtc.ToString("o")
    finished_utc = $finishedUtc.ToString("o")
    duration_seconds = ($finishedUtc - $startedUtc).TotalSeconds
    git = [ordered]@{
        commit = $gitCommit
        dirty = if ($portablePackage) {
            [bool]$packageAfter.git_dirty
        } else {
            $gitStatus.Count -ne 0
        }
        status = $gitStatus
    }
    build = [ordered]@{
        directory = $BuildDirectory
        configuration = $Configuration
        provenance_mode = if ($portablePackage) {
            "portable_package_manifest"
        } else {
            "local_cmake_sdk_roots"
        }
        cmake_cache = $cmakeCacheAfter
        sdk_roots = $sdkRoots
        package = if ($portablePackage) {
            [ordered]@{
                package_id = $packageAfter.package_id
                manifest = $packageAfter.manifest
                file_count = $packageAfter.file_count
                allowed_backends = $packageAfter.allowed_backends
                model_relative_path = $packageAfter.model_relative_path
            }
        } else {
            $null
        }
        deployment = $deploymentEvidence
        executable = $executableAfter
        runtime_dlls = $runtimeAfter
    }
    inputs = [ordered]@{
        model = $modelAfter
        config = $configAfter
    }
    hardware = $hardwareInventory
    benchmark = [ordered]@{
        backend = $Backend
        openvino_device = if ($Backend -eq "openvino") {
            $OpenVinoDevice
        } else {
            $null
        }
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
            cpu_fallback_disabled =
                $Backend -eq "directml" -or $Backend -eq "openvino"
            node_fallback_policy = switch ($Backend) {
                "tensorrt" { "TensorRT -> CUDA -> CPU" }
                "cuda" { "CUDA -> CPU" }
                "directml" { "DirectML only" }
                "openvino" { "OpenVINO only" }
                "cpu" { "CPU only" }
            }
            verification = switch ($Backend) {
                "tensorrt" { "independent ORT profiling session" }
                "cuda" { "independent ORT profiling session" }
                "directml" { "session.disable_cpu_ep_fallback=1" }
                "openvino" {
                    "independent ORT profiling session + session.disable_cpu_ep_fallback=1"
                }
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
        performance_probes = $enablePerformanceProbesValue
        d3d11_cuda_interop = $enableD3D11CudaInteropValue
        d3d11_directml_interop = $enableD3D11DirectMlInteropValue
        expected_explicit_device_copy = $expectExplicitDeviceCopy
        detector_intra_threads = 0
        detector_inter_threads = 0
        runtime_queue_policy = "latest-only"
        runtime_debug_ring_capacity = 4096
        report_capacity = $reportRetentionCapacity
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
        aim = [ordered]@{
            prediction_enabled = $expectedAimPredictionValue
            max_prediction_lead_percent =
                $ExpectedAimMaxPredictionLeadPercent
            minimum_precomputed_command_frames =
                $MinimumAimPrecomputedCommandFrames
            physical_output_expected = $false
        }
    }
    report = [ordered]@{
        csv_sha256 = (Get-FileHash -LiteralPath $pendingCsv `
            -Algorithm SHA256).Hash
        json_sha256 = (Get-FileHash -LiteralPath $pendingJson `
            -Algorithm SHA256).Hash
        sample_count = [long]$retainedSampleCount
        formal_sample_count = [long]$formalSampleCount
        retained_sample_count = [long]$retainedSampleCount
        omitted_sample_count = [long]$omittedSampleCount
        retention_capacity = [long]$retention.retention_capacity
        retention_policy = [string]$retention.retention_policy
        summary_scope = [string]$retention.summary_scope
        aim_validation_scope = [string]$retention.summary_scope
        total = $report.timing.total
        pipeline_complete = $report.timing.pipeline_complete
        coverage = $report.coverage
        ndi_video_queue_depth = $report.ndi_video_queue_depth
        aim = $aimSummary
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
Write-Host ("  formal_samples={0}, retained_samples={1}, omitted_samples={2}, provider={3}" -f
    $formalSampleCount, $retainedSampleCount, $omittedSampleCount,
    $expectedProvider)
Write-Host "  retained total P50=$($report.timing.total.p50_ms) ms"
Write-Host "  retained total P95=$($report.timing.total.p95_ms) ms"
Write-Host "  retained total P99=$($report.timing.total.p99_ms) ms"
Write-Host (("  Aim：targets={0}, commands={1}, lead_active={2}, " +
    "prediction_outside_box={3}, switches={4}") -f
    $aimSummary.target_frames, $aimSummary.command_frames,
    $aimSummary.lead_active_frames,
    $aimSummary.prediction_point_outside_box_frames,
    $aimSummary.target_switches)
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
