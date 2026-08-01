param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$ImagePath,
    [string]$BuildDirectory = "",
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$UltralyticsRoot = "",
    [string]$ReferenceDirectory = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

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

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-pose"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$ModelPath = Resolve-InputFile $ModelPath "YOLOv8 兼容姿态 ONNX 模型"
$ImagePath = Resolve-InputFile $ImagePath "包含人物的真实测试图像"
if (-not [string]::IsNullOrWhiteSpace($UltralyticsRoot)) {
    if (-not (Test-Path -LiteralPath `
            (Join-Path $UltralyticsRoot "ultralytics") -PathType Container)) {
        throw "Ultralytics 源码根目录无效：$UltralyticsRoot"
    }
    $UltralyticsRoot = (Resolve-Path -LiteralPath $UltralyticsRoot).ProviderPath
    if ([string]::IsNullOrWhiteSpace($ReferenceDirectory)) {
        $ReferenceDirectory = Join-Path $BuildDirectory "pose-reference"
    }
    $ReferenceDirectory = [System.IO.Path]::GetFullPath($ReferenceDirectory)
}

# 姿态算法和真实模型基线固定使用 CPU EP，隔离本机 TensorRT、CUDA、
# DirectML 与 NDI SDK。权重和图像只作为本地验收输入，不进入 Git。
& (Join-Path $PSScriptRoot "build.ps1") `
    -BuildDirectory $BuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -CudaRoot "" `
    -DirectMlRoot "" `
    -NdiSdkRoot "" `
    -ModelPath "" `
    -SegmentationModelPath "" `
    -SegmentationImagePath "" `
    -PoseModelPath $ModelPath `
    -PoseImagePath $ImagePath `
    -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "姿态构建或 CTest 失败，退出码：$LASTEXITCODE"
}

$testExecutable = Join-Path $BuildDirectory `
    "$Configuration\pose_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "姿态真实模型测试程序不存在：$testExecutable"
}

$originalPath = $env:PATH
try {
    # 单独执行以输出实例、关键点与后处理耗时；PATH 只保留系统目录，
    # 验证程序确实使用输出目录内已部署的 ORT/OpenCV DLL。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    $testArguments = @($ModelPath, $ImagePath)
    if (-not [string]::IsNullOrWhiteSpace($UltralyticsRoot)) {
        $testArguments += $ReferenceDirectory
    }
    & $testExecutable @testArguments
    if ($LASTEXITCODE -ne 0) {
        throw "姿态真实模型复核失败，退出码：$LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

if (-not [string]::IsNullOrWhiteSpace($UltralyticsRoot)) {
    $pythonCommand = (Get-Command python -ErrorAction Stop).Source
    & $pythonCommand `
        (Join-Path $PSScriptRoot "compare_pose_reference.py") `
        --model $ModelPath `
        --image $ImagePath `
        --xen-result $ReferenceDirectory `
        --ultralytics-root $UltralyticsRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Ultralytics 姿态参考对照失败，退出码：$LASTEXITCODE"
    }
}

Write-Host "姿态真实模型验证通过：$testExecutable"
