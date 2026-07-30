param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [ValidateRange(0, 99)]
    [int]$TensorRtMajor = 0,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build"),
    [string]$CacheDirectory = (Join-Path $PSScriptRoot "..\cache\tensorrt-test"),
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

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

function Invoke-TensorRtModelTest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $true)]
        [string]$Model,
        [Parameter(Mandatory = $true)]
        [string]$Cache,
        [Parameter(Mandatory = $true)]
        [int]$Run
    )

    Write-Host "TensorRT 模型测试，第 $Run 次加载："
    # Windows PowerShell 会把原生进程重定向后的 stderr 包装成非终止错误；
    # TensorRT 的正常 WARNING 不能触发全局 Stop，最终结果仍以退出码为准。
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $lines = @(& $Executable $Model "tensorrt" $Cache 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    foreach ($line in $lines) {
        Write-Host ([string]$line)
    }
    if ($exitCode -ne 0) {
        throw "TensorRT 模型测试失败，退出码：$exitCode"
    }

    $match = [regex]::Match(
        ($lines -join "`n"),
        'load_ms=([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)')
    if (-not $match.Success) {
        throw "TensorRT 模型测试未输出可解析的 load_ms。"
    }
    return [double]::Parse(
        $match.Groups[1].Value,
        [System.Globalization.CultureInfo]::InvariantCulture)
}

& (Join-Path $PSScriptRoot "build.ps1") `
    -BuildDirectory $BuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -TensorRtRoot $TensorRtRoot `
    -TensorRtMajor $TensorRtMajor `
    -CudnnRoot $CudnnRoot `
    -CudaRoot $CudaRoot `
    -DirectMlRoot "" `
    -ModelPath $ModelPath `
    -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "TensorRT 构建或基础测试失败，退出码：$LASTEXITCODE"
}

$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$testExecutable = Join-Path $BuildDirectory `
    "$Configuration\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "TensorRT 测试程序不存在：$testExecutable"
}

$cmakeCache = Join-Path $BuildDirectory "CMakeCache.txt"
$resolvedTensorRtMajor = Get-CMakeCacheValue `
    $cmakeCache "XEN_TENSORRT_MAJOR"
if ($resolvedTensorRtMajor -notmatch '^[0-9]+$') {
    throw "CMakeCache 中的 TensorRT major 无效：$resolvedTensorRtMajor"
}

$outputDirectory = Split-Path -Parent $testExecutable
$requiredRuntimeFiles = @(
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll",
    "onnxruntime_providers_tensorrt.dll",
    "nvinfer_$resolvedTensorRtMajor.dll",
    "nvonnxparser_$resolvedTensorRtMajor.dll",
    "opencv_world*.dll"
)
foreach ($pattern in $requiredRuntimeFiles) {
    $matches = @(Get-ChildItem -LiteralPath $outputDirectory -File -Filter $pattern)
    if ($matches.Count -eq 0) {
        throw "构建输出缺少运行库 $pattern：$outputDirectory"
    }
}
$cudartDlls = @(Get-ChildItem -LiteralPath $outputDirectory `
    -File -Filter "cudart64_*.dll")
if ($cudartDlls.Count -ne 1) {
    throw "构建输出必须且只能包含一个 cudart64_*.dll：$outputDirectory"
}
if (Test-Path -LiteralPath (Join-Path $outputDirectory "cudart.lib") -PathType Leaf) {
    throw "构建输出不应包含开发用 cudart.lib：$outputDirectory"
}

$cacheRoot = [System.IO.Path]::GetFullPath($CacheDirectory)
New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
$runCacheDirectory = Join-Path $cacheRoot (
    "run-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"),
    [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $runCacheDirectory | Out-Null
Write-Host "本次独立缓存目录：$runCacheDirectory"

$originalPath = $env:PATH
try {
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    $firstLoadMs = Invoke-TensorRtModelTest `
        $testExecutable $ModelPath $runCacheDirectory 1

    $engineFiles = @(Get-ChildItem -LiteralPath $runCacheDirectory `
        -File -Filter "*.engine")
    $timingFiles = @(Get-ChildItem -LiteralPath $runCacheDirectory `
        -File -Filter "*.timing")
    if ($engineFiles.Count -eq 0 -or $timingFiles.Count -eq 0) {
        throw "TensorRT 首次加载后未生成 Engine Cache 或 Timing Cache。"
    }
    $engineHashes = @{}
    foreach ($engineFile in $engineFiles) {
        $engineHashes[$engineFile.Name] =
            (Get-FileHash -LiteralPath $engineFile.FullName -Algorithm SHA256).Hash
    }

    $secondLoadMs = Invoke-TensorRtModelTest `
        $testExecutable $ModelPath $runCacheDirectory 2
} finally {
    $env:PATH = $originalPath
}

$engineFilesAfterReuse = @(Get-ChildItem -LiteralPath $runCacheDirectory `
    -File -Filter "*.engine")
if ($engineFilesAfterReuse.Count -ne $engineHashes.Count) {
    throw "TensorRT 第二次加载改变了 Engine Cache 文件数量。"
}
foreach ($engineFile in $engineFilesAfterReuse) {
    if (-not $engineHashes.ContainsKey($engineFile.Name)) {
        throw "TensorRT 第二次加载生成了新的 Engine Cache：$($engineFile.Name)"
    }
    $hash = (Get-FileHash -LiteralPath $engineFile.FullName -Algorithm SHA256).Hash
    if ($hash -ne $engineHashes[$engineFile.Name]) {
        throw "TensorRT 第二次加载重写了 Engine Cache：$($engineFile.Name)"
    }
}
if ($secondLoadMs -ge $firstLoadMs) {
    throw "TensorRT 第二次加载未体现缓存复用：首次 ${firstLoadMs}ms，第二次 ${secondLoadMs}ms"
}

Write-Host "TensorRT 缓存生成与复用验证通过：首次 ${firstLoadMs}ms，第二次 ${secondLoadMs}ms"
$engineFilesAfterReuse + $timingFiles |
    Select-Object Name, Length, LastWriteTime
