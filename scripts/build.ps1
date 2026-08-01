param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build"),
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [ValidateRange(0, 99)]
    [int]$TensorRtMajor = 0,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$DirectMlRoot = $env:DIRECTML_ROOT,
    [string]$NdiSdkRoot = $env:NDI_SDK_DIR,
    [string]$ModelPath = "",
    [string]$SegmentationModelPath = "",
    [string]$SegmentationImagePath = "",
    [string]$PoseModelPath = "",
    [string]$PoseImagePath = "",
    [string]$ObbModelPath = "",
    [string]$ObbImagePath = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [ValidateSet("Container", "Leaf")]
        [string]$PathType = "Container"
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Resolve-TensorRtMajor {
    param(
        [string]$Root,
        [int]$RequestedMajor
    )

    if ([string]::IsNullOrWhiteSpace($Root)) {
        if ($RequestedMajor -ne 0) {
            throw "指定了 TensorRT major，但 TensorRT SDK 根目录为空。"
        }
        return ""
    }

    $binDirectory = Join-Path $Root "bin"
    $majorVersions = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $binDirectory `
            -File -Filter "nvinfer_*.dll")) {
        if ($file.Name -match '^nvinfer_([0-9]+)\.dll$') {
            $majorVersions += [int]$Matches[1]
        }
    }
    $majorVersions = @($majorVersions | Sort-Object -Unique)
    if ($RequestedMajor -eq 0) {
        if ($majorVersions.Count -ne 1) {
            throw "无法唯一识别 TensorRT DLL major，请通过 -TensorRtMajor 显式指定：$binDirectory"
        }
        $RequestedMajor = $majorVersions[0]
    } elseif ($majorVersions -notcontains $RequestedMajor) {
        throw "TensorRT SDK 中不存在 nvinfer_$RequestedMajor.dll：$binDirectory"
    }

    $parserDll = Join-Path $binDirectory "nvonnxparser_$RequestedMajor.dll"
    if (-not (Test-Path -LiteralPath $parserDll -PathType Leaf)) {
        throw "TensorRT SDK 缺少与 major 匹配的 ONNX Parser：$parserDll"
    }
    return [string]$RequestedMajor
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
    if (-not $entry) {
        throw "CMakeCache 缺少 $Name：$CachePath"
    }
    return $entry.Matches[0].Groups[1].Value
}

function Assert-PathWithinRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Candidate,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $candidatePath = [System.IO.Path]::GetFullPath($Candidate)
    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $rootPrefix = $rootPath + [System.IO.Path]::DirectorySeparatorChar
    if (-not $candidatePath.Equals(
            $rootPath, [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $candidatePath.StartsWith(
            $rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description 未解析到指定 ONNX Runtime SDK 内：$candidatePath"
    }
}

$ortHeaderFound = $false
if (-not [string]::IsNullOrWhiteSpace($OnnxRuntimeRoot)) {
    $ortHeaderCandidates = @(
        (Join-Path $OnnxRuntimeRoot "include\onnxruntime_cxx_api.h"),
        (Join-Path $OnnxRuntimeRoot "build\native\include\onnxruntime_cxx_api.h")
    )
    $ortHeaderFound = [bool]($ortHeaderCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    })
}
if ([string]::IsNullOrWhiteSpace($OnnxRuntimeRoot) -or -not $ortHeaderFound) {
    throw "ONNX Runtime SDK 无效：请通过 -OnnxRuntimeRoot 或 ONNXRUNTIME_ROOT 指定解压根目录。"
}
$OnnxRuntimeRoot = Resolve-ExistingPath $OnnxRuntimeRoot "ONNX Runtime SDK 目录"

if ([string]::IsNullOrWhiteSpace($OpenCvDir) -or
    -not (Test-Path -LiteralPath (Join-Path $OpenCvDir "OpenCVConfig.cmake"))) {
    throw "OpenCV_DIR 无效：请通过 -OpenCvDir 或 OpenCV_DIR 指定包含 OpenCVConfig.cmake 的目录。"
}
$OpenCvDir = Resolve-ExistingPath $OpenCvDir "OpenCV 配置目录"
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

if (-not [string]::IsNullOrWhiteSpace($TensorRtRoot)) {
    $TensorRtRoot = Resolve-ExistingPath $TensorRtRoot "TensorRT SDK 目录"
}
if (-not [string]::IsNullOrWhiteSpace($CudnnRoot)) {
    $CudnnRoot = Resolve-ExistingPath $CudnnRoot "cuDNN SDK 目录"
}
if (-not [string]::IsNullOrWhiteSpace($CudaRoot)) {
    $CudaRoot = Resolve-ExistingPath $CudaRoot "CUDA Toolkit 目录"
}
if (-not [string]::IsNullOrWhiteSpace($DirectMlRoot)) {
    $DirectMlRoot = Resolve-ExistingPath $DirectMlRoot "DirectML 可再发行目录"
}
if (-not [string]::IsNullOrWhiteSpace($NdiSdkRoot)) {
    $NdiSdkRoot = Resolve-ExistingPath $NdiSdkRoot "NDI 6 SDK 目录"
}
if (-not [string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Resolve-ExistingPath $ModelPath "测试模型" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($SegmentationModelPath)) {
    $SegmentationModelPath = Resolve-ExistingPath `
        $SegmentationModelPath "实例分割测试模型" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($SegmentationImagePath)) {
    $SegmentationImagePath = Resolve-ExistingPath `
        $SegmentationImagePath "实例分割真实测试图像" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($PoseModelPath)) {
    $PoseModelPath = Resolve-ExistingPath `
        $PoseModelPath "姿态测试模型" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($PoseImagePath)) {
    $PoseImagePath = Resolve-ExistingPath `
        $PoseImagePath "姿态真实测试图像" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($ObbModelPath)) {
    $ObbModelPath = Resolve-ExistingPath `
        $ObbModelPath "OBB 测试模型" "Leaf"
}
if (-not [string]::IsNullOrWhiteSpace($ObbImagePath)) {
    $ObbImagePath = Resolve-ExistingPath `
        $ObbImagePath "OBB 真实测试图像" "Leaf"
}
$resolvedTensorRtMajor = Resolve-TensorRtMajor $TensorRtRoot $TensorRtMajor
$cmakeCommand = (Get-Command cmake -ErrorAction Stop).Source
$ctestCommand = (Get-Command ctest -ErrorAction Stop).Source

$configureArguments = @(
    "-S", (Join-Path $PSScriptRoot ".."),
    "-B", $BuildDirectory,
    "-G", "Visual Studio 18 2026",
    "-A", "x64",
    "-DONNXRUNTIME_ROOT=$OnnxRuntimeRoot",
    "-UONNXRUNTIME_INCLUDE_DIR",
    "-UONNXRUNTIME_LIB",
    "-UCUDAToolkit_*",
    "-DOpenCV_DIR=$OpenCvDir",
    "-DBUILD_TESTING=ON",
    "-DXEN_TENSORRT_ROOT=$TensorRtRoot",
    "-DXEN_TENSORRT_MAJOR=$resolvedTensorRtMajor",
    "-DXEN_CUDNN_ROOT=$CudnnRoot",
    "-DXEN_CUDA_ROOT=$CudaRoot",
    "-DXEN_DIRECTML_ROOT=$DirectMlRoot",
    "-DXEN_NDI_SDK_ROOT=$NdiSdkRoot",
    "-DXEN_TEST_MODEL=$ModelPath",
    "-DXEN_TEST_SEGMENTATION_MODEL=$SegmentationModelPath",
    "-DXEN_TEST_SEGMENTATION_IMAGE=$SegmentationImagePath",
    "-DXEN_TEST_POSE_MODEL=$PoseModelPath",
    "-DXEN_TEST_POSE_IMAGE=$PoseImagePath",
    "-DXEN_TEST_OBB_MODEL=$ObbModelPath",
    "-DXEN_TEST_OBB_IMAGE=$ObbImagePath"
)
if (-not [string]::IsNullOrWhiteSpace($DirectMlRoot)) {
    $directMlDll = Join-Path $DirectMlRoot "bin\x64-win\DirectML.dll"
    if (-not (Test-Path -LiteralPath $directMlDll -PathType Leaf)) {
        throw "DirectML 可再发行目录无效：$DirectMlRoot"
    }
}

& $cmakeCommand @configureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码：$LASTEXITCODE" }

$cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
$resolvedInclude = Get-CMakeCacheValue $cachePath "ONNXRUNTIME_INCLUDE_DIR"
$resolvedLibrary = Get-CMakeCacheValue $cachePath "ONNXRUNTIME_LIB"
Assert-PathWithinRoot $resolvedInclude $OnnxRuntimeRoot "ONNX Runtime 头文件"
Assert-PathWithinRoot $resolvedLibrary $OnnxRuntimeRoot "ONNX Runtime 导入库"
if (-not [string]::IsNullOrWhiteSpace($resolvedTensorRtMajor)) {
    $cachedTensorRtMajor = Get-CMakeCacheValue $cachePath "XEN_TENSORRT_MAJOR"
    if ($cachedTensorRtMajor -ne $resolvedTensorRtMajor) {
        throw "CMakeCache 中的 TensorRT major 不一致：$cachedTensorRtMajor"
    }
}
Write-Host "ONNX Runtime 解析校验通过：$resolvedLibrary"

if (-not [string]::IsNullOrWhiteSpace($NdiSdkRoot)) {
    $resolvedNdiInclude = Get-CMakeCacheValue $cachePath "XEN_NDI_INCLUDE_DIR"
    $resolvedNdiLibrary = Get-CMakeCacheValue $cachePath "XEN_NDI_LIB"
    Assert-PathWithinRoot $resolvedNdiInclude $NdiSdkRoot "NDI SDK 头文件"
    Assert-PathWithinRoot $resolvedNdiLibrary $NdiSdkRoot "NDI SDK 导入库"
    Write-Host "NDI SDK 解析校验通过：$resolvedNdiLibrary"
}

& $cmakeCommand --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "构建失败，退出码：$LASTEXITCODE" }

if (-not [string]::IsNullOrWhiteSpace($NdiSdkRoot)) {
    $outputDirectory = Join-Path $BuildDirectory $Configuration
    foreach ($runtimeName in @(
            "Processing.NDI.Lib.x64.dll",
            "Processing.NDI.Lib.Licenses.txt")) {
        $runtimePath = Join-Path $outputDirectory $runtimeName
        if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
            throw "NDI 运行文件未部署到可执行文件目录：$runtimePath"
        }
    }
}

# CMake 的 POST_BUILD 规则必须已把目标所需 DLL 部署到可执行文件旁。
# 测试阶段只保留系统目录，避免调用方已有的 SDK PATH 掩盖缺失复制规则。
$originalPath = $env:PATH
try {
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    & $ctestCommand --test-dir $BuildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "测试失败，退出码：$LASTEXITCODE" }
} finally {
    $env:PATH = $originalPath
}
