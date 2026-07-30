param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$VideoDirectory,
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$CacheDirectory = (Join-Path $PSScriptRoot "..\cache\tensorrt-test"),
    [string]$ReportPath = (Join-Path $PSScriptRoot `
        ("..\cache\benchmarks\detector-videos-{0}.csv" -f `
            (Get-Date -Format "yyyyMMdd-HHmmss"))),
    [ValidateSet("center", "full")]
    [string]$InputMode = "center",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$requiredDirectories = @(
    @{ Name = "视频"; Path = $VideoDirectory },
    @{ Name = "ONNX Runtime"; Path = $OnnxRuntimeRoot },
    @{ Name = "OpenCV"; Path = $OpenCvDir },
    @{ Name = "TensorRT"; Path = $TensorRtRoot },
    @{ Name = "cuDNN"; Path = $CudnnRoot },
    @{ Name = "CUDA"; Path = $CudaRoot }
)
foreach ($item in $requiredDirectories) {
    if ([string]::IsNullOrWhiteSpace($item.Path) -or
        -not (Test-Path -LiteralPath $item.Path -PathType Container)) {
        throw "$($item.Name) 目录不存在：$($item.Path)"
    }
}
if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) {
    throw "模型不存在：$ModelPath"
}

$videoFiles = @(Get-ChildItem -LiteralPath $VideoDirectory -File -Filter "*.mp4")
if ($videoFiles.Count -eq 0) {
    throw "视频目录中没有 MP4：$VideoDirectory"
}

$testExecutable = Join-Path $PSScriptRoot `
    "..\build\$Configuration\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "测试程序不存在，请先运行 scripts/build.ps1：$testExecutable"
}

$cudnnDlls = @(Get-ChildItem -LiteralPath $CudnnRoot `
    -Recurse -File -Filter "cudnn64_9.dll")
if ($cudnnDlls.Count -ne 1) {
    throw "cuDNN 目录下必须且只能找到一个 cudnn64_9.dll：$CudnnRoot"
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
    & $testExecutable $ModelPath "tensorrt" $CacheDirectory `
        $VideoDirectory $ReportPath $InputMode
    if ($LASTEXITCODE -ne 0) {
        throw "Detector 视频基准失败，退出码：$LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

Write-Host "Detector 视频基准完成：$ReportPath"
