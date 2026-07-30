param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$CacheDirectory = (Join-Path $PSScriptRoot "..\cache\tensorrt-test"),
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$requiredRoots = @(
    @{ Name = "ONNXRUNTIME_ROOT"; Path = $OnnxRuntimeRoot },
    @{ Name = "OpenCV_DIR"; Path = $OpenCvDir },
    @{ Name = "TENSORRT_ROOT"; Path = $TensorRtRoot },
    @{ Name = "CUDNN_ROOT"; Path = $CudnnRoot },
    @{ Name = "CUDA_PATH"; Path = $CudaRoot }
)
foreach ($item in $requiredRoots) {
    if ([string]::IsNullOrWhiteSpace($item.Path) -or
        -not (Test-Path -LiteralPath $item.Path -PathType Container)) {
        throw "$($item.Name) 目录不存在：$($item.Path)"
    }
}

$requiredFiles = @(
    @{ Name = "模型"; Path = $ModelPath },
    @{ Name = "ORT 核心 DLL"; Path = (Join-Path $OnnxRuntimeRoot "lib\onnxruntime.dll") },
    @{ Name = "OpenCV 配置"; Path = (Join-Path $OpenCvDir "OpenCVConfig.cmake") },
    @{ Name = "TensorRT DLL"; Path = (Join-Path $TensorRtRoot "bin\nvinfer_10.dll") },
    @{ Name = "CUDA Runtime"; Path = (Join-Path $CudaRoot "bin\x64\cudart64_13.dll") }
)
foreach ($item in $requiredFiles) {
    if ([string]::IsNullOrWhiteSpace($item.Path) -or
        -not (Test-Path -LiteralPath $item.Path -PathType Leaf)) {
        throw "$($item.Name) 不存在：$($item.Path)"
    }
}

$cudnnDlls = @(Get-ChildItem -LiteralPath $CudnnRoot -Recurse -File -Filter "cudnn64_9.dll")
if ($cudnnDlls.Count -ne 1) {
    throw "CUDNN_ROOT 下必须且只能找到一个 cudnn64_9.dll：$CudnnRoot"
}

$testExecutable = Join-Path $PSScriptRoot `
    "..\build\$Configuration\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "测试程序不存在，请先运行 scripts/build.ps1：$testExecutable"
}

$openCvBin = Join-Path (Split-Path -Parent $OpenCvDir) "bin"
$runtimeDirectories = @(
    (Join-Path $TensorRtRoot "bin"),
    $cudnnDlls[0].DirectoryName,
    (Join-Path $CudaRoot "bin\x64"),
    (Join-Path $OnnxRuntimeRoot "lib"),
    $openCvBin
)

$originalPath = $env:PATH
try {
    $env:PATH = ($runtimeDirectories -join ";") + ";" + $originalPath
    for ($run = 1; $run -le 2; ++$run) {
        Write-Host "TensorRT 模型测试，第 $run 次加载："
        & $testExecutable $ModelPath "tensorrt" $CacheDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "TensorRT 模型测试失败，退出码：$LASTEXITCODE"
        }
    }
} finally {
    $env:PATH = $originalPath
}

$engineFiles = @(Get-ChildItem -LiteralPath $CacheDirectory -File -Filter "*.engine")
$timingFiles = @(Get-ChildItem -LiteralPath $CacheDirectory -File -Filter "*.timing")
if ($engineFiles.Count -eq 0 -or $timingFiles.Count -eq 0) {
    throw "TensorRT 测试完成，但 Engine Cache 或 Timing Cache 未生成。"
}

Write-Host "TensorRT 缓存验证通过："
$engineFiles + $timingFiles | Select-Object Name, Length, LastWriteTime
