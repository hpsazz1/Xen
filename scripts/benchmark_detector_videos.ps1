param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$VideoDirectory,
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$TensorRtRoot = $env:TENSORRT_ROOT,
    [ValidateRange(0, 99)]
    [int]$TensorRtMajor = 0,
    [string]$CudnnRoot = $env:CUDNN_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build"),
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

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $escapedName = [regex]::Escape($Name)
    $match = Select-String -LiteralPath $CachePath `
        -Pattern "^${escapedName}:[^=]*=(.*)$" | Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }
    return $match.Matches[0].Groups[1].Value
}

function Resolve-CHeaderInteger {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Defines,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $current = $Name
    $visited = @{}
    for ($depth = 0; $depth -lt 16; ++$depth) {
        if ($current -match '^\d+$') {
            return [int]$current
        }
        if ($visited.ContainsKey($current) -or
            -not $Defines.ContainsKey($current)) {
            return $null
        }
        $visited[$current] = $true
        $current = [string]$Defines[$current]
    }
    return $null
}

function Get-TensorRtHeaderVersion {
    param([Parameter(Mandatory = $true)][string]$Root)

    $versionHeader = Join-Path $Root "include\NvInferVersion.h"
    if (-not (Test-Path -LiteralPath $versionHeader -PathType Leaf)) {
        throw "TensorRT 版本头文件不存在：$versionHeader"
    }

    $defines = @{}
    foreach ($line in Get-Content -LiteralPath $versionHeader -Encoding UTF8) {
        if ($line -match '^\s*#define\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)') {
            $defines[$Matches[1]] = $Matches[2]
        }
    }

    $parts = @(
        Resolve-CHeaderInteger $defines "NV_TENSORRT_MAJOR"
        Resolve-CHeaderInteger $defines "NV_TENSORRT_MINOR"
        Resolve-CHeaderInteger $defines "NV_TENSORRT_PATCH"
        Resolve-CHeaderInteger $defines "NV_TENSORRT_BUILD"
    )
    if ($parts.Count -ne 4 -or $parts -contains $null) {
        throw "无法从 TensorRT 版本头文件解析完整版本：$versionHeader"
    }
    return ($parts -join ".")
}

function Get-InventoryFingerprint {
    param([Parameter(Mandatory = $true)][object[]]$Files)

    $inventory = ($Files | ForEach-Object {
        "{0}|{1}|{2}|{3}" -f $_.name, $_.bytes,
            $_.last_write_utc, $_.sha256
    }) -join "`n"
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($inventory)
        return ([BitConverter]::ToString(
            $sha256.ComputeHash($bytes))).Replace("-", "")
    } finally {
        $sha256.Dispose()
    }
}

function Get-FileInventory {
    param([Parameter(Mandatory = $true)][object[]]$Files)

    return @($Files | Sort-Object FullName -Unique | ForEach-Object {
        [ordered]@{
            name = $_.Name
            bytes = $_.Length
            last_write_utc = $_.LastWriteTimeUtc.ToString("o")
            sha256 = (Get-FileHash -LiteralPath $_.FullName `
                -Algorithm SHA256).Hash
        }
    })
}

function Get-TensorRtCacheInfo {
    param([Parameter(Mandatory = $true)][string]$Directory)

    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        return @()
    }
    $files = @(Get-ChildItem -LiteralPath $Directory -File | Where-Object {
        $_.Extension -eq ".engine" -or $_.Extension -eq ".timing"
    })
    if ($files.Count -eq 0) {
        return @()
    }
    return @(Get-FileInventory $files)
}

function Get-DeployedRuntimeInfo {
    param([Parameter(Mandatory = $true)][string]$Directory)

    $patterns = @(
        "onnxruntime*.dll",
        "nvinfer*.dll",
        "nvonnxparser*.dll",
        "cudnn*.dll",
        "cudart64_*.dll",
        "cublas*.dll",
        "cufft*.dll",
        "opencv_world*.dll"
    )
    $files = foreach ($pattern in $patterns) {
        Get-ChildItem -LiteralPath $Directory -File -Filter $pattern
    }
    return @($files | Sort-Object FullName -Unique | ForEach-Object {
        $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
            $_.FullName)
        [ordered]@{
            name = $_.Name
            bytes = $_.Length
            last_write_utc = $_.LastWriteTimeUtc.ToString("o")
            sha256 = (Get-FileHash -LiteralPath $_.FullName `
                -Algorithm SHA256).Hash
            file_version = $version.FileVersion
            product_version = $version.ProductVersion
        }
    })
}

function Get-NvidiaGpuInfo {
    $nvidiaSmi = Get-Command "nvidia-smi.exe" -ErrorAction SilentlyContinue
    if ($null -eq $nvidiaSmi) {
        return @()
    }

    $lines = @(& $nvidiaSmi.Source `
        --query-gpu=name,uuid,driver_version,compute_cap,memory.total `
        --format=csv,noheader,nounits 2>$null)
    if ($LASTEXITCODE -ne 0) {
        return @()
    }
    return @($lines | ForEach-Object {
        $fields = @(([string]$_) -split ',\s*', 5)
        if ($fields.Count -ne 5) {
            return [ordered]@{ raw = ([string]$_).Trim() }
        }
        [ordered]@{
            name = $fields[0]
            uuid = $fields[1]
            driver_version = $fields[2]
            compute_capability = $fields[3]
            memory_mib = [long]$fields[4]
        }
    })
}

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
    throw "TensorRT 基准构建或基础测试失败，退出码：$LASTEXITCODE"
}

$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$testExecutable = Join-Path $BuildDirectory `
    "$Configuration\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "TensorRT 基准程序不存在：$testExecutable"
}

# 基准前固定所有输入与二进制快照；完成后重新计算并逐项对比，避免长时间运行期间
# 模型、视频或部署 DLL 被替换后，JSON 却描述成另一套环境。
$snapshotModelPath = [System.IO.Path]::GetFullPath($ModelPath)
$snapshotVideoDirectory = [System.IO.Path]::GetFullPath($VideoDirectory)
$snapshotOutputDirectory = Split-Path -Parent $testExecutable
$snapshotModelSha256 = (Get-FileHash -LiteralPath $snapshotModelPath `
    -Algorithm SHA256).Hash
$snapshotVideoFiles = @(Get-ChildItem -LiteralPath $snapshotVideoDirectory `
    -File -Filter "*.mp4" | Sort-Object Name)
$snapshotVideoInventory = @(Get-FileInventory $snapshotVideoFiles)
$snapshotVideoFingerprint = Get-InventoryFingerprint $snapshotVideoInventory
$snapshotRuntimeInventory = @(Get-DeployedRuntimeInfo $snapshotOutputDirectory)
$snapshotRuntimeFingerprint = Get-InventoryFingerprint $snapshotRuntimeInventory
$snapshotExecutableSha256 = (Get-FileHash -LiteralPath $testExecutable `
    -Algorithm SHA256).Hash
$snapshotCacheBefore = @(Get-TensorRtCacheInfo `
    ([System.IO.Path]::GetFullPath($CacheDirectory)))
$benchmarkStartedUtc = [DateTime]::UtcNow.ToString("o")

$originalPath = $env:PATH
try {
    # 只允许从可执行文件目录或系统目录加载 DLL，避免开发机 PATH 掩盖部署缺口。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    & $testExecutable $ModelPath "tensorrt" $CacheDirectory `
        $VideoDirectory $ReportPath $InputMode
    if ($LASTEXITCODE -ne 0) {
        throw "Detector 视频基准失败，退出码：$LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}
$benchmarkFinishedUtc = [DateTime]::UtcNow.ToString("o")

Write-Host "Detector 视频基准完成：$ReportPath"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
$VideoDirectory = [System.IO.Path]::GetFullPath($VideoDirectory)
$CacheDirectory = [System.IO.Path]::GetFullPath($CacheDirectory)
$manifestPath = [System.IO.Path]::ChangeExtension($ReportPath, ".json")
$cmakeCache = Join-Path $BuildDirectory "CMakeCache.txt"

$gitCommit = $null
$gitDirty = $null
$gitCommand = Get-Command "git.exe" -ErrorAction SilentlyContinue
if ($null -ne $gitCommand) {
    $commitOutput = @(& $gitCommand.Source -C $repositoryRoot `
        rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $commitOutput.Count -gt 0) {
        $gitCommit = ([string]$commitOutput[0]).Trim()
    }
    $statusOutput = @(& $gitCommand.Source -C $repositoryRoot `
        status --porcelain --untracked-files=normal 2>$null)
    if ($LASTEXITCODE -eq 0) {
        $gitDirty = $statusOutput.Count -gt 0
    }
}

$videoFiles = @(Get-ChildItem -LiteralPath $VideoDirectory `
    -File -Filter "*.mp4" | Sort-Object Name)
$videoInventory = @(Get-FileInventory $videoFiles)
$videoFingerprint = Get-InventoryFingerprint $videoInventory
$modelSha256 = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash
$deployedRuntimes = @(Get-DeployedRuntimeInfo `
    (Split-Path -Parent $testExecutable))
$runtimeFingerprint = Get-InventoryFingerprint $deployedRuntimes
$executableSha256 = (Get-FileHash -LiteralPath $testExecutable `
    -Algorithm SHA256).Hash
if ($modelSha256 -ne $snapshotModelSha256 -or
    $videoFingerprint -ne $snapshotVideoFingerprint -or
    $runtimeFingerprint -ne $snapshotRuntimeFingerprint -or
    $executableSha256 -ne $snapshotExecutableSha256) {
    throw "基准运行期间模型、视频、可执行文件或部署运行库发生变化，报告已拒绝发布。"
}
$reportRows = @(Import-Csv -LiteralPath $ReportPath)
if ($reportRows.Count -eq 0) {
    throw "Detector 视频基准报告没有数据行：$ReportPath"
}
if ($reportRows.Count -ne $videoFiles.Count) {
    throw "Detector 视频基准场景数与视频清单不一致：报告 $($reportRows.Count)，视频 $($videoFiles.Count)"
}
$expectedScenes = @($videoFiles | ForEach-Object { $_.BaseName } |
    Sort-Object)
$actualScenes = @($reportRows | ForEach-Object { $_.scene } |
    Sort-Object)
$sceneDifference = @(Compare-Object `
    -ReferenceObject $expectedScenes -DifferenceObject $actualScenes)
if ($sceneDifference.Count -ne 0) {
    throw "Detector 视频基准场景集合与视频清单不一致：$($sceneDifference -join '; ')"
}
$sampleFrames = 0L
foreach ($row in $reportRows) {
    $frames = [long]$row.frames
    $sampleFrames += $frames
    if ($frames -le 0 -or [long]$row.failed_frames -ne 0 -or
        $row.status -ne "SUCCESS" -or
        [int]$row.explicit_device_copy -ne 1) {
        throw "Detector 视频基准包含失败样本：$($row.scene)"
    }
}
$modelShapeKeys = @($reportRows | ForEach-Object {
    "{0}x{1}" -f $_.model_input_height, $_.model_input_width
} | Sort-Object -Unique)
if ($modelShapeKeys.Count -ne 1) {
    throw "Detector 视频基准出现多个模型输入 shape。"
}
$modelInputShape = @(
    [int]$reportRows[0].model_input_height,
    [int]$reportRows[0].model_input_width
)
$evaluatedShapes = @($reportRows | ForEach-Object {
    [ordered]@{
        scene = $_.scene
        height = [int]$_.evaluated_height
        width = [int]$_.evaluated_width
    }
})

$manifest = [ordered]@{
    schema_version = 2
    generated_utc = [DateTime]::UtcNow.ToString("o")
    report = [ordered]@{
        csv = $ReportPath
        json = $manifestPath
        started_utc = $benchmarkStartedUtc
        finished_utc = $benchmarkFinishedUtc
        scenes = $reportRows.Count
        frames = $sampleFrames
        warmup_frames_per_scene = 50
    }
    source = [ordered]@{
        repository = $repositoryRoot
        git_commit = $gitCommit
        git_dirty = $gitDirty
    }
    model = [ordered]@{
        path = $ModelPath
        bytes = (Get-Item -LiteralPath $ModelPath).Length
        sha256 = $modelSha256
        input_hw = $modelInputShape
        output_format = "AUTO"
    }
    input = [ordered]@{
        video_directory = $VideoDirectory
        mode = $InputMode
        evaluated_shapes = $evaluatedShapes
        inventory_sha256 = $videoFingerprint
        files = $videoInventory
    }
    detector = [ordered]@{
        backend = "TensorrtExecutionProvider"
        node_fallback = @("CUDAExecutionProvider", "CPUExecutionProvider")
        fp16 = $true
        cuda_graph = $true
        output_fingerprint = $false
        intra_threads = 0
        inter_threads = 0
        execution_mode = "ORT_SEQUENTIAL"
        engine_cache = $true
        timing_cache = $true
        cache_directory = $CacheDirectory
    }
    build = [ordered]@{
        configuration = $Configuration
        directory = $BuildDirectory
        executable = $testExecutable
        executable_sha256 = $executableSha256
        cmake_generator = Get-CMakeCacheValue $cmakeCache "CMAKE_GENERATOR"
        onnxruntime_library = Get-CMakeCacheValue `
            $cmakeCache "ONNXRUNTIME_LIB"
        tensorrt_major = Get-CMakeCacheValue `
            $cmakeCache "XEN_TENSORRT_MAJOR"
    }
    requested_dependencies = [ordered]@{
        onnxruntime_root = [System.IO.Path]::GetFullPath($OnnxRuntimeRoot)
        opencv_dir = [System.IO.Path]::GetFullPath($OpenCvDir)
        tensorrt_root = [System.IO.Path]::GetFullPath($TensorRtRoot)
        tensorrt_version = Get-TensorRtHeaderVersion $TensorRtRoot
        cudnn_root = [System.IO.Path]::GetFullPath($CudnnRoot)
        cuda_root = [System.IO.Path]::GetFullPath($CudaRoot)
    }
    host = [ordered]@{
        os = [Environment]::OSVersion.VersionString
        machine = [Environment]::MachineName
        processor = $env:PROCESSOR_IDENTIFIER
        nvidia_gpus = @(Get-NvidiaGpuInfo)
    }
    tensorrt_cache = [ordered]@{
        warm_before_run = $snapshotCacheBefore.Count -gt 0
        before = $snapshotCacheBefore
        after = @(Get-TensorRtCacheInfo $CacheDirectory)
    }
    validation = [ordered]@{
        graph_on_off_output_match = $true
        changing_input_fingerprint = $true
        isolated_runtime_path = $true
        csv_matches_video_inventory = $true
        inputs_unchanged_during_run = $true
    }
    deployed_runtimes = $deployedRuntimes
}

$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Detector 基准环境清单：$manifestPath"
