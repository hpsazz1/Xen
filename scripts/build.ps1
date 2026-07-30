param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build"),
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
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

# OpenCV_DIR 通常指向 build/x64/vcXX/lib，测试程序运行时需要同级 bin 中的 DLL。
$openCvPlatformDirectory = Split-Path -Parent $OpenCvDir
$openCvBinDirectory = Join-Path $openCvPlatformDirectory "bin"
$onnxRuntimeBinDirectory = Join-Path $OnnxRuntimeRoot "lib"
$originalPath = $env:PATH
try {
    if (Test-Path -LiteralPath $onnxRuntimeBinDirectory) {
        $env:PATH = "$onnxRuntimeBinDirectory;$env:PATH"
    }
    if (Test-Path -LiteralPath $openCvBinDirectory) {
        $env:PATH = "$openCvBinDirectory;$env:PATH"
    }
    & ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "测试失败，退出码：$LASTEXITCODE" }
} finally {
    $env:PATH = $originalPath
}
