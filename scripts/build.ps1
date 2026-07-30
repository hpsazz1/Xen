param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build"),
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$ModelPath = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OnnxRuntimeRoot) -or
    -not (Test-Path -LiteralPath (Join-Path $OnnxRuntimeRoot "include\onnxruntime_cxx_api.h"))) {
    throw "ONNX Runtime SDK 无效：请通过 -OnnxRuntimeRoot 或 ONNXRUNTIME_ROOT 指定解压根目录。"
}

if ([string]::IsNullOrWhiteSpace($OpenCvDir) -or
    -not (Test-Path -LiteralPath (Join-Path $OpenCvDir "OpenCVConfig.cmake"))) {
    throw "OpenCV_DIR 无效：请通过 -OpenCvDir 或 OpenCV_DIR 指定包含 OpenCVConfig.cmake 的目录。"
}

$configureArguments = @(
    "-S", (Join-Path $PSScriptRoot ".."),
    "-B", $BuildDirectory,
    "-G", "Visual Studio 18 2026",
    "-A", "x64",
    "-DONNXRUNTIME_ROOT=$OnnxRuntimeRoot",
    "-DOpenCV_DIR=$OpenCvDir",
    "-DBUILD_TESTING=ON"
)
if (-not [string]::IsNullOrWhiteSpace($TensorRtRoot)) {
    if (-not (Test-Path -LiteralPath $TensorRtRoot -PathType Container)) {
        throw "TensorRT SDK 目录不存在：$TensorRtRoot"
    }
    $configureArguments += "-DXEN_TENSORRT_ROOT=$TensorRtRoot"
}
if (-not [string]::IsNullOrWhiteSpace($CudnnRoot)) {
    if (-not (Test-Path -LiteralPath $CudnnRoot -PathType Container)) {
        throw "cuDNN SDK 目录不存在：$CudnnRoot"
    }
    $configureArguments += "-DXEN_CUDNN_ROOT=$CudnnRoot"
}
if (-not [string]::IsNullOrWhiteSpace($CudaRoot)) {
    if (-not (Test-Path -LiteralPath $CudaRoot -PathType Container)) {
        throw "CUDA Toolkit 目录不存在：$CudaRoot"
    }
    $configureArguments += "-DXEN_CUDA_ROOT=$CudaRoot"
}
if (-not [string]::IsNullOrWhiteSpace($ModelPath)) {
    if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) {
        throw "测试模型不存在：$ModelPath"
    }
    $configureArguments += "-DXEN_TEST_MODEL=$ModelPath"
}

& cmake @configureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码：$LASTEXITCODE" }

& cmake --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "构建失败，退出码：$LASTEXITCODE" }

# CMake 的 POST_BUILD 规则必须已把目标所需 DLL 部署到可执行文件旁。
# 此处故意不添加 SDK bin/lib 到 PATH，防止缺失的复制规则被开发环境掩盖。
& ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "测试失败，退出码：$LASTEXITCODE" }
