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
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-segmentation"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$ModelPath = Resolve-InputFile $ModelPath "YOLOv8 兼容分割 ONNX 模型"
$ImagePath = Resolve-InputFile $ImagePath "包含实例的真实测试图像"
if (-not [string]::IsNullOrWhiteSpace($UltralyticsRoot)) {
    if (-not (Test-Path -LiteralPath `
            (Join-Path $UltralyticsRoot "ultralytics") -PathType Container)) {
        throw "Ultralytics 源码根目录无效：$UltralyticsRoot"
    }
    $UltralyticsRoot = (Resolve-Path -LiteralPath $UltralyticsRoot).ProviderPath
    if ([string]::IsNullOrWhiteSpace($ReferenceDirectory)) {
        $ReferenceDirectory = Join-Path $BuildDirectory `
            "segmentation-reference"
    }
    $ReferenceDirectory = [System.IO.Path]::GetFullPath($ReferenceDirectory)
}

# 分割算法和真实模型基线固定使用 CPU EP，隔离本机 TensorRT、CUDA、DirectML
# 与 NDI SDK。权重和图像只作为本地验收输入，不复制进输出目录或 Git。
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
    -SegmentationModelPath $ModelPath `
    -SegmentationImagePath $ImagePath `
    -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "实例分割构建或 CTest 失败，退出码：$LASTEXITCODE"
}

$testExecutable = Join-Path $BuildDirectory `
    "$Configuration\segmentation_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "实例分割真实模型测试程序不存在：$testExecutable"
}

$originalPath = $env:PATH
try {
    # 再次单独执行以输出掩码数量、字节数和后处理耗时；PATH 只保留系统目录，
    # 验证可执行文件确实使用输出目录内已部署的 ORT/OpenCV DLL。
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
        throw "实例分割真实模型复核失败，退出码：$LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

if (-not [string]::IsNullOrWhiteSpace($UltralyticsRoot)) {
    $pythonCommand = (Get-Command python -ErrorAction Stop).Source
    & $pythonCommand `
        (Join-Path $PSScriptRoot "compare_segmentation_reference.py") `
        --model $ModelPath `
        --image $ImagePath `
        --xen-result $ReferenceDirectory `
        --ultralytics-root $UltralyticsRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Ultralytics 实例分割参考对照失败，退出码：$LASTEXITCODE"
    }
}

Write-Host "实例分割真实模型验证通过：$testExecutable"
