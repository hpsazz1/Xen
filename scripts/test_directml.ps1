param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [string]$BuildDirectory = "",
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_DML_ROOT,
    [string]$DirectMlRoot = $env:DIRECTML_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-dml"
}

if ([string]::IsNullOrWhiteSpace($OnnxRuntimeRoot)) {
    throw "请通过 -OnnxRuntimeRoot 或 ONNXRUNTIME_DML_ROOT 指定 DirectML 版 ORT。"
}
if ([string]::IsNullOrWhiteSpace($DirectMlRoot)) {
    throw "请通过 -DirectMlRoot 或 DIRECTML_ROOT 指定 DirectML 可再发行包。"
}

# DML 使用独立构建目录，禁止与 CUDA/TensorRT 版 onnxruntime.dll 混用。
& (Join-Path $PSScriptRoot "build.ps1") `
    -BuildDirectory $BuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -DirectMlRoot $DirectMlRoot `
    -OpenCvDir $OpenCvDir `
    -ModelPath $ModelPath `
    -Configuration $Configuration `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -CudaRoot ""
if ($LASTEXITCODE -ne 0) {
    throw "DirectML 构建或基础测试失败，退出码：$LASTEXITCODE"
}

$testExecutable = Join-Path $BuildDirectory "$Configuration\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "DirectML 测试程序不存在：$testExecutable"
}

# 测试程序会严格校验实际 Provider，若回退 CPU 会直接返回失败。
$originalPath = $env:PATH
try {
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    & $testExecutable $ModelPath "directml"
    if ($LASTEXITCODE -ne 0) {
        throw "DirectML 真实模型推理失败，退出码：$LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

Write-Host "DirectML 真实模型推理验证通过：$testExecutable"
