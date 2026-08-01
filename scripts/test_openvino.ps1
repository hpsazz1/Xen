param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [string]$BuildDirectory = "",
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_OPENVINO_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [ValidateSet("CPU", "GPU", "NPU")]
    [string]$Device = "CPU",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-openvino"
}
if ([string]::IsNullOrWhiteSpace($OnnxRuntimeRoot)) {
    throw "请通过 -OnnxRuntimeRoot 或 ONNXRUNTIME_OPENVINO_ROOT 指定 OpenVINO 版 ORT。"
}

$requiredSdkFiles = @(
    "runtimes\win-x64\native\onnxruntime.dll",
    "runtimes\win-x64\native\onnxruntime_providers_openvino.dll",
    "runtimes\win-x64\native\openvino.dll",
    "runtimes\win-x64\native\openvino_onnx_frontend.dll",
    "runtimes\win-x64\native\openvino_intel_cpu_plugin.dll",
    "runtimes\win-x64\native\openvino_intel_gpu_plugin.dll",
    "runtimes\win-x64\native\openvino_intel_npu_plugin.dll",
    "runtimes\win-x64\native\tbb12.dll",
    "build\native\include\openvino_provider_factory.h"
)
foreach ($relativePath in $requiredSdkFiles) {
    $path = Join-Path $OnnxRuntimeRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "OpenVINO ORT SDK 不完整，缺少：$path"
    }
}

# OpenVINO 使用独立 ORT 包和构建目录，显式清空其他 Provider SDK，防止旧
# CMake Cache 或环境变量把 CUDA/TensorRT/DirectML 运行库带入输出目录。
& (Join-Path $PSScriptRoot "build.ps1") `
    -BuildDirectory $BuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -ModelPath $ModelPath `
    -OpenVinoTestDevice $Device `
    -Configuration $Configuration `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -CudaRoot "" `
    -DirectMlRoot "" `
    -NdiSdkRoot ""
if ($LASTEXITCODE -ne 0) {
    throw "OpenVINO 构建或基础测试失败，退出码：$LASTEXITCODE"
}

$outputDirectory = Join-Path $BuildDirectory $Configuration
$testExecutable = Join-Path $outputDirectory "openvino_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "OpenVINO 测试程序不存在：$testExecutable"
}

$requiredRuntimeDlls = @(
    "onnxruntime.dll", "onnxruntime_providers_shared.dll",
    "onnxruntime_providers_openvino.dll", "openvino.dll",
    "openvino_onnx_frontend.dll",
    "openvino_intel_cpu_plugin.dll", "openvino_intel_gpu_plugin.dll",
    "openvino_intel_npu_plugin.dll", "tbb12.dll")
foreach ($name in $requiredRuntimeDlls) {
    $path = Join-Path $outputDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "OpenVINO 测试输出缺少运行库：$path"
    }
}
$forbiddenRuntimeDlls = @(Get-ChildItem -LiteralPath $outputDirectory `
    -File -Filter "*.dll" | Where-Object {
        $_.Name -match '^(DirectML|cudart|cublas|cudnn|nvinfer|nvonnxparser|onnxruntime_providers_(cuda|tensorrt))'
    })
if ($forbiddenRuntimeDlls.Count -ne 0) {
    throw "OpenVINO 输出混入其他 Provider 运行库：$($forbiddenRuntimeDlls.Name -join ', ')"
}

$profileDirectory = Join-Path $BuildDirectory "cache\openvino"
New-Item -ItemType Directory -Path $profileDirectory -Force | Out-Null
$profilePrefix = Join-Path $profileDirectory (
    "manual-{0}-{1}" -f $Device.ToLowerInvariant(),
    [guid]::NewGuid().ToString("N"))
$deviceArgument = $Device.ToLowerInvariant()

$originalPath = $env:PATH
try {
    # 只保留 Windows 系统目录，验证可执行文件完全依赖本轮部署到同目录的 DLL。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    $testOutput = @(& $testExecutable `
        $ModelPath $deviceArgument $profilePrefix 2>&1)
    $exitCode = $LASTEXITCODE
    $testOutput | ForEach-Object { Write-Host $_ }
    if ($exitCode -ne 0) {
        throw "OpenVINO 真实模型推理失败，退出码：$exitCode"
    }
} finally {
    $env:PATH = $originalPath
}

$profileLine = @($testOutput | ForEach-Object { [string]$_ } |
    Where-Object { $_.StartsWith("provider_profile=") } |
    Select-Object -Last 1)
if ($profileLine.Count -ne 1) {
    throw "测试输出没有唯一的 Provider profile 路径。"
}
$profilePath = $profileLine[0].Substring("provider_profile=".Length)
if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    throw "Provider profile 不存在：$profilePath"
}

$document = Get-Content -LiteralPath $profilePath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$events = @($document)
$providerCounts = [ordered]@{}
$nodeEventCount = 0
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
}
if ($nodeEventCount -eq 0 -or
    -not $providerCounts.Contains("OpenVINOExecutionProvider") -or
    [long]$providerCounts["OpenVINOExecutionProvider"] -le 0) {
    throw "ORT profile 没有 OpenVINOExecutionProvider 节点。"
}
$unexpectedProviders = @($providerCounts.Keys | Where-Object {
    $_ -ne "OpenVINOExecutionProvider"
})
if ($unexpectedProviders.Count -ne 0) {
    throw "严格 OpenVINO Session 出现后备 Provider：$($unexpectedProviders -join ', ')"
}

Write-Host "OpenVINO 真实模型验证通过："
Write-Host "  device=$Device, provider=OpenVINOExecutionProvider"
Write-Host "  node_events=$nodeEventCount"
Write-Host "  profile=$profilePath"
Write-Host "  executable=$testExecutable"
