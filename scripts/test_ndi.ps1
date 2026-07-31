param(
    [string]$NdiBuildDirectory = (Join-Path $PSScriptRoot "..\build-ndi"),
    [string]$NoNdiBuildDirectory = (Join-Path $PSScriptRoot "..\build-no-ndi"),
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$NdiSdkRoot = $env:NDI_SDK_DIR,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($NdiSdkRoot)) {
    throw "请通过 -NdiSdkRoot 或 NDI_SDK_DIR 指定 NDI 6 SDK。"
}

$buildScript = Join-Path $PSScriptRoot "build.ps1"

# 真实 SDK 路径会运行生产 ICapture 与本进程 NDI Sender 的 mDNS 回环测试，
# 并在清空 SDK PATH 后核验目标旁的 DLL 与许可证部署。
& $buildScript `
    -BuildDirectory $NdiBuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -NdiSdkRoot $NdiSdkRoot `
    -Configuration $Configuration `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -DirectMlRoot ""
if ($LASTEXITCODE -ne 0) {
    throw "NDI SDK 构建或真实回环测试失败，退出码：$LASTEXITCODE"
}

# 独立构建目录强制禁用 NDI SDK，验证同一源码可编译且选择 NDI 时明确返回
# UNSUPPORTED。CMake POST_BUILD 还会删除目录中可能残留的旧 NDI 运行文件。
& $buildScript `
    -BuildDirectory $NoNdiBuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -NdiSdkRoot "" `
    -Configuration $Configuration `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -DirectMlRoot ""
if ($LASTEXITCODE -ne 0) {
    throw "无 NDI SDK 构建边界测试失败，退出码：$LASTEXITCODE"
}

$noNdiOutput = Join-Path $NoNdiBuildDirectory $Configuration
foreach ($runtimeName in @(
        "Processing.NDI.Lib.x64.dll",
        "Processing.NDI.Lib.Licenses.txt")) {
    $runtimePath = Join-Path $noNdiOutput $runtimeName
    if (Test-Path -LiteralPath $runtimePath -PathType Leaf) {
        throw "无 NDI SDK 构建不应残留运行文件：$runtimePath"
    }
}

Write-Host "NDI 有 SDK/无 SDK 双路径验证通过。"
