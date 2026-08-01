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
    [string]$VisibilityDirectory = "",
    [string]$AimAnnotationDirectory = "",
    [switch]$AimContinuity,
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ($null -eq ("XenDetectorBenchmarkAtomicFile" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class XenDetectorBenchmarkAtomicFile {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileExW(
        string existingName,
        string newName,
        uint flags);
}
'@
}

function Publish-JsonAtomically {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $temporaryPath = "{0}.{1}.tmp" -f $Path, $PID
    if (Test-Path -LiteralPath $temporaryPath) {
        throw "环境清单临时路径已存在：$temporaryPath"
    }
    try {
        $json = $Value | ConvertTo-Json -Depth 8
        $encoding = New-Object System.Text.UTF8Encoding($true)
        [System.IO.File]::WriteAllText($temporaryPath, $json, $encoding)
        $moveFileReplaceExisting = 0x1
        $moveFileWriteThrough = 0x8
        if (-not [XenDetectorBenchmarkAtomicFile]::MoveFileExW(
                $temporaryPath, $Path,
                $moveFileReplaceExisting -bor $moveFileWriteThrough)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "无法原子发布环境清单，Win32 error=$errorCode"
        }
    } finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Publish-ExistingFileAtomically {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,
        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "待发布文件不存在：$Source"
    }
    $moveFileReplaceExisting = 0x1
    $moveFileWriteThrough = 0x8
    if (-not [XenDetectorBenchmarkAtomicFile]::MoveFileExW(
            $Source, $Destination,
            $moveFileReplaceExisting -bor $moveFileWriteThrough)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "无法原子发布文件，Win32 error=$errorCode：$Destination"
    }
}

function Get-VideoFiles {
    param([Parameter(Mandatory = $true)][string]$Directory)

    return @(Get-ChildItem -LiteralPath $Directory -File | Where-Object {
        $_.Extension -ieq ".mp4" -or $_.Extension -ieq ".avi"
    } | Sort-Object Name)
}

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
$useVisibilityAnnotations = -not [string]::IsNullOrWhiteSpace(
    $VisibilityDirectory)
if ($useVisibilityAnnotations) {
    if ($InputMode -ne "center") {
        throw "目标可见性标注只支持 InputMode=center。"
    }
    if (-not (Test-Path -LiteralPath $VisibilityDirectory `
            -PathType Container)) {
        throw "目标可见性标注目录不存在：$VisibilityDirectory"
    }
    $VisibilityDirectory = [System.IO.Path]::GetFullPath(
        $VisibilityDirectory)
}
$useAimAnnotations = -not [string]::IsNullOrWhiteSpace(
    $AimAnnotationDirectory)
if ($useAimAnnotations) {
    if ($InputMode -ne "center") {
        throw "Aim 真值标注只支持 InputMode=center。"
    }
    if (-not (Test-Path -LiteralPath $AimAnnotationDirectory `
            -PathType Container)) {
        throw "Aim 真值标注目录不存在：$AimAnnotationDirectory"
    }
    $AimAnnotationDirectory = [System.IO.Path]::GetFullPath(
        $AimAnnotationDirectory)
}
$useAimContinuity = $AimContinuity.IsPresent -or $useAimAnnotations

$videoFiles = @(Get-VideoFiles $VideoDirectory)
if ($videoFiles.Count -eq 0) {
    throw "视频目录中没有 MP4/AVI：$VideoDirectory"
}
$visibilityFiles = @()
if ($useVisibilityAnnotations) {
    $visibilityFiles = @(Get-ChildItem -LiteralPath $VisibilityDirectory `
        -File | Where-Object { $_.Name -ilike "*.visibility.json" } |
        Sort-Object Name)
    $expectedVisibilityNames = @($videoFiles | ForEach-Object {
        ("{0}.visibility.json" -f $_.Name).ToLowerInvariant()
    } | Sort-Object)
    $actualVisibilityNames = @($visibilityFiles | ForEach-Object {
        $_.Name.ToLowerInvariant()
    } | Sort-Object)
    $visibilityDifference = @(Compare-Object `
        -ReferenceObject $expectedVisibilityNames `
        -DifferenceObject $actualVisibilityNames)
    if ($visibilityDifference.Count -ne 0) {
        throw "视频与可见性标注文件必须一一对应：$($visibilityDifference -join '; ')"
    }
}
$aimAnnotationFiles = @()
if ($useAimAnnotations) {
    $aimAnnotationFiles = @(Get-ChildItem -LiteralPath $AimAnnotationDirectory `
        -File | Where-Object { $_.Name -ilike "*.aim.json" } |
        Sort-Object Name)
    $expectedAimNames = @($videoFiles | ForEach-Object {
        ("{0}.aim.json" -f $_.Name).ToLowerInvariant()
    } | Sort-Object)
    $actualAimNames = @($aimAnnotationFiles | ForEach-Object {
        $_.Name.ToLowerInvariant()
    } | Sort-Object)
    $aimDifference = @(Compare-Object `
        -ReferenceObject $expectedAimNames -DifferenceObject $actualAimNames)
    if ($aimDifference.Count -ne 0) {
        throw "视频与 Aim 真值标注文件必须一一对应：$($aimDifference -join '; ')"
    }
}

$finalReportPath = [System.IO.Path]::GetFullPath($ReportPath)
if ([System.IO.Path]::GetExtension($finalReportPath) -ine ".csv") {
    throw "Detector 视频基准报告必须使用 .csv 扩展名：$finalReportPath"
}
$manifestPath = [System.IO.Path]::ChangeExtension(
    $finalReportPath, ".json")
$pendingReportPath = "{0}.{1}.pending" -f $finalReportPath, $PID
if (Test-Path -LiteralPath $pendingReportPath) {
    throw "待发布 CSV 路径已存在：$pendingReportPath"
}
$protectedInputPaths = @(
    [System.IO.Path]::GetFullPath($ModelPath)
) + @($videoFiles | ForEach-Object { $_.FullName }) +
    @($visibilityFiles | ForEach-Object { $_.FullName }) +
    @($aimAnnotationFiles | ForEach-Object { $_.FullName })
foreach ($candidate in @(
        $finalReportPath, $manifestPath, $pendingReportPath)) {
    foreach ($inputPath in $protectedInputPaths) {
        if ($candidate.Equals(
                [System.IO.Path]::GetFullPath($inputPath),
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "报告路径与只读输入冲突：$candidate"
        }
    }
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
$snapshotVideoFiles = @(Get-VideoFiles $snapshotVideoDirectory)
$snapshotVideoInventory = @(Get-FileInventory $snapshotVideoFiles)
$snapshotVideoFingerprint = Get-InventoryFingerprint $snapshotVideoInventory
$snapshotVisibilityInventory = @()
$snapshotVisibilityFingerprint = $null
if ($useVisibilityAnnotations) {
    $snapshotVisibilityFiles = @(Get-ChildItem `
        -LiteralPath $VisibilityDirectory -File | Where-Object {
            $_.Name -ilike "*.visibility.json"
        } | Sort-Object Name)
    $snapshotVisibilityInventory = @(Get-FileInventory `
        $snapshotVisibilityFiles)
    $snapshotVisibilityFingerprint = Get-InventoryFingerprint `
        $snapshotVisibilityInventory
}
$snapshotAimInventory = @()
$snapshotAimFingerprint = $null
if ($useAimAnnotations) {
    $snapshotAimFiles = @(Get-ChildItem -LiteralPath $AimAnnotationDirectory `
        -File | Where-Object { $_.Name -ilike "*.aim.json" } |
        Sort-Object Name)
    $snapshotAimInventory = @(Get-FileInventory $snapshotAimFiles)
    $snapshotAimFingerprint = Get-InventoryFingerprint $snapshotAimInventory
}
$snapshotRuntimeInventory = @(Get-DeployedRuntimeInfo $snapshotOutputDirectory)
$snapshotRuntimeFingerprint = Get-InventoryFingerprint $snapshotRuntimeInventory
$snapshotExecutableSha256 = (Get-FileHash -LiteralPath $testExecutable `
    -Algorithm SHA256).Hash
$snapshotCacheBefore = @(Get-TensorRtCacheInfo `
    ([System.IO.Path]::GetFullPath($CacheDirectory)))
$benchmarkStartedUtc = [DateTime]::UtcNow.ToString("o")

try {
    $originalPath = $env:PATH
    try {
    # 只允许从可执行文件目录或系统目录加载 DLL，避免开发机 PATH 掩盖部署缺口。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    $benchmarkArguments = @(
        $ModelPath,
        "tensorrt",
        $CacheDirectory,
        $VideoDirectory,
        $pendingReportPath,
        $InputMode
    )
    if ($useVisibilityAnnotations) {
        $benchmarkArguments += @(
            "--visibility-directory",
            $VisibilityDirectory
        )
    }
    if ($useAimAnnotations) {
        $benchmarkArguments += @(
            "--aim-annotation-directory",
            $AimAnnotationDirectory
        )
    }
    if ($useAimContinuity) {
        $benchmarkArguments += "--aim-continuity"
    }
    & $testExecutable @benchmarkArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Detector 视频基准失败，退出码：$LASTEXITCODE"
    }
    } finally {
        $env:PATH = $originalPath
    }
    $benchmarkFinishedUtc = [DateTime]::UtcNow.ToString("o")

    Write-Host "Detector 视频基准执行完成，开始校验待发布 CSV。"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$ModelPath = [System.IO.Path]::GetFullPath($ModelPath)
$VideoDirectory = [System.IO.Path]::GetFullPath($VideoDirectory)
$CacheDirectory = [System.IO.Path]::GetFullPath($CacheDirectory)
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

$videoFiles = @(Get-VideoFiles $VideoDirectory)
$videoInventory = @(Get-FileInventory $videoFiles)
$videoFingerprint = Get-InventoryFingerprint $videoInventory
$visibilityInventory = @()
$visibilityFingerprint = $null
if ($useVisibilityAnnotations) {
    $visibilityFiles = @(Get-ChildItem -LiteralPath $VisibilityDirectory `
        -File | Where-Object { $_.Name -ilike "*.visibility.json" } |
        Sort-Object Name)
    $visibilityInventory = @(Get-FileInventory $visibilityFiles)
    $visibilityFingerprint = Get-InventoryFingerprint $visibilityInventory
}
$aimInventory = @()
$aimFingerprint = $null
if ($useAimAnnotations) {
    $aimFiles = @(Get-ChildItem -LiteralPath $AimAnnotationDirectory `
        -File | Where-Object { $_.Name -ilike "*.aim.json" } |
        Sort-Object Name)
    $aimInventory = @(Get-FileInventory $aimFiles)
    $aimFingerprint = Get-InventoryFingerprint $aimInventory
}
$modelSha256 = (Get-FileHash -LiteralPath $ModelPath -Algorithm SHA256).Hash
$deployedRuntimes = @(Get-DeployedRuntimeInfo `
    (Split-Path -Parent $testExecutable))
$runtimeFingerprint = Get-InventoryFingerprint $deployedRuntimes
$executableSha256 = (Get-FileHash -LiteralPath $testExecutable `
    -Algorithm SHA256).Hash
if ($modelSha256 -ne $snapshotModelSha256 -or
    $videoFingerprint -ne $snapshotVideoFingerprint -or
    $visibilityFingerprint -ne $snapshotVisibilityFingerprint -or
    $aimFingerprint -ne $snapshotAimFingerprint -or
    $runtimeFingerprint -ne $snapshotRuntimeFingerprint -or
    $executableSha256 -ne $snapshotExecutableSha256) {
    throw "基准运行期间模型、视频、标注、可执行文件或部署运行库发生变化，报告已拒绝发布。"
}
$reportRows = @(Import-Csv -LiteralPath $pendingReportPath)
if ($reportRows.Count -eq 0) {
    throw "Detector 视频基准报告没有数据行：$pendingReportPath"
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
$aimVisibleFramesTotal = 0L
foreach ($row in $reportRows) {
    $frames = [long]$row.frames
    $sampleFrames += $frames
    if ($frames -le 0 -or [long]$row.failed_frames -ne 0 -or
        $row.status -ne "SUCCESS" -or
        [int]$row.explicit_device_copy -ne 1) {
        throw "Detector 视频基准包含失败样本：$($row.scene)"
    }
    $annotationsPresent = [int]$row.visibility_annotations
    $recallAvailable = [int]$row.recall_available
    $annotatedFrames = [long]$row.annotated_frames
    $visibleFrames = [long]$row.visible_frames
    $visibleDetectedFrames = [long]$row.visible_detected_frames
    $visibleMissedFrames = [long]$row.visible_missed_frames
    $notVisibleFrames = [long]$row.not_visible_frames
    $notVisibleDetectedFrames = [long]$row.not_visible_detected_frames
    $ignoredFrames = [long]$row.ignored_frames
    $ignoredDetectedFrames = [long]$row.ignored_detected_frames
    $aimContinuityField = [int]$row.aim_continuity
    $aimControlComplete = [int]$row.aim_control_complete
    $aimControlEvaluatedFrames = [long]$row.aim_control_evaluated_frames
    $aimControlInvalidFrames = [long]$row.aim_control_invalid_frames
    $aimAnnotations = [int]$row.aim_annotations
    $aimComplete = [int]$row.aim_complete
    $aimAnnotatedFrames = [long]$row.aim_annotated_frames
    $aimVisibleFrames = [long]$row.aim_visible_frames
    $aimVisibleFramesTotal += $aimVisibleFrames
    $aimMatchedVisibleFrames = [long]$row.aim_matched_visible_frames
    $aimMissedVisibleFrames = [long]$row.aim_missed_visible_frames
    $aimIdSwitches = [long]$row.aim_id_switches
    $aimTrackFragments = [long]$row.aim_track_fragments
    $aimFragmentationEvents = [long]$row.aim_track_fragmentation_events
    $aimUnnecessarySwitches = [long]$row.aim_unnecessary_switches
    $aimNotVisibleFrames = [long]$row.aim_not_visible_frames
    $aimIgnoredFrames = [long]$row.aim_ignored_frames
    $aimOutputTargetFrames = [long]$row.aim_output_target_frames
    $aimInvalidFrames = [long]$row.aim_invalid_frames
    $aimCommandFrames = [long]$row.aim_command_frames
    $aimObservedCommandFrames = [long]$row.aim_observed_command_frames
    $aimPredictedCommandFrames = [long]$row.aim_predicted_command_frames
    $aimTargetWithoutCommandFrames =
        [long]$row.aim_target_without_command_frames
    $aimNoTargetFrames = [long]$row.aim_no_target_frames
    $aimPredictedTargetFrames = [long]$row.aim_predicted_target_frames
    $aimContinuitySegments = [long]$row.aim_continuity_segments
    $aimTargetSwitches = [long]$row.aim_target_switches
    $aimTargetStateChanges = [long]$row.aim_target_state_changes
    $aimPredictionStateChanges = [long]$row.aim_prediction_state_changes
    $aimDirectionReversals = [long]$row.aim_direction_reversals
    $aimLimitBoundaryFrames = [long]$row.aim_limit_boundary_frames
    $aimLimitBoundaryRate = [double]$row.aim_limit_boundary_rate
    $aimDistributionPrefixes = @(
        "abs_dx", "abs_dy", "magnitude", "delta", "acceleration"
    )
    $aimDistributions = @{}
    foreach ($prefix in $aimDistributionPrefixes) {
        $samples = [long]$row.("aim_{0}_samples" -f $prefix)
        $mean = [double]$row.("aim_{0}_mean_counts" -f $prefix)
        $p50 = [double]$row.("aim_{0}_p50_counts" -f $prefix)
        $p95 = [double]$row.("aim_{0}_p95_counts" -f $prefix)
        $p99 = [double]$row.("aim_{0}_p99_counts" -f $prefix)
        $maximum = [double]$row.("aim_{0}_max_counts" -f $prefix)
        $values = @($mean, $p50, $p95, $p99, $maximum)
        $invalidValue = @($values | Where-Object {
            [double]::IsNaN($_) -or [double]::IsInfinity($_) -or $_ -lt 0.0
        }).Count -ne 0
        if ($samples -lt 0 -or $invalidValue -or
            $p50 -gt $p95 -or $p95 -gt $p99 -or $p99 -gt $maximum -or
            $mean -gt ($maximum + 0.0001) -or
            ($samples -eq 0 -and @($values | Where-Object {
                [math]::Abs($_) -gt 0.0001
            }).Count -ne 0)) {
            throw "Aim 控制分布无效：$($row.scene)，$prefix"
        }
        $aimDistributions[$prefix] = [ordered]@{
            samples = $samples
            mean = $mean
            p50 = $p50
            p95 = $p95
            p99 = $p99
            maximum = $maximum
        }
    }
    $aimConfigurationValues = @(
        $row.aim_person_class_ids, $row.aim_head_class_ids,
        $row.aim_high_confidence, $row.aim_low_confidence,
        $row.aim_min_confirmed_hits, $row.aim_max_lost_frames,
        $row.aim_min_iou, $row.aim_max_center_distance,
        $row.aim_switch_margin, $row.aim_switch_confirm_frames,
        $row.aim_switch_cooldown_frames, $row.aim_body_aim_height_ratio,
        $row.aim_deadzone_pixels, $row.aim_smoothing,
        $row.aim_counts_per_pixel_x, $row.aim_counts_per_pixel_y,
        $row.aim_max_counts_per_frame, $row.aim_predicted_gain,
        $row.aim_evaluation_min_iou,
        $row.aim_evaluation_max_center_distance,
        $row.aim_timebase_fps
    )
    $missingAimConfigurationValues = @($aimConfigurationValues |
        Where-Object { [string]::IsNullOrEmpty($_) })
    if ($useAimContinuity) {
        $aimFloatingConfiguration = @(
            [double]$row.aim_high_confidence,
            [double]$row.aim_low_confidence,
            [double]$row.aim_min_iou,
            [double]$row.aim_max_center_distance,
            [double]$row.aim_switch_margin,
            [double]$row.aim_body_aim_height_ratio,
            [double]$row.aim_deadzone_pixels,
            [double]$row.aim_smoothing,
            [double]$row.aim_counts_per_pixel_x,
            [double]$row.aim_counts_per_pixel_y,
            [double]$row.aim_max_counts_per_frame,
            [double]$row.aim_predicted_gain,
            [double]$row.aim_evaluation_min_iou,
            [double]$row.aim_evaluation_max_center_distance,
            [double]$row.aim_timebase_fps
        )
        $invalidAimFloatingConfiguration = @(
            $aimFloatingConfiguration | Where-Object {
                [double]::IsNaN($_) -or [double]::IsInfinity($_)
            }
        ).Count -ne 0
        if ($aimContinuityField -ne 1 -or $aimControlComplete -ne 1 -or
            $aimControlEvaluatedFrames -ne $frames -or
            $aimControlInvalidFrames -ne 0 -or
            $aimCommandFrames -lt 0 -or
            $aimObservedCommandFrames -lt 0 -or
            $aimPredictedCommandFrames -lt 0 -or
            ($aimObservedCommandFrames + $aimPredictedCommandFrames) -ne
                $aimCommandFrames -or
            $aimTargetWithoutCommandFrames -lt 0 -or
            $aimNoTargetFrames -lt 0 -or
            ($aimCommandFrames + $aimTargetWithoutCommandFrames +
                $aimNoTargetFrames) -ne $frames -or
            $aimPredictedTargetFrames -lt 0 -or
            $aimPredictedCommandFrames -gt $aimPredictedTargetFrames -or
            $aimPredictedTargetFrames -gt
                ($aimCommandFrames + $aimTargetWithoutCommandFrames) -or
            $aimContinuitySegments -lt 0 -or
            $aimContinuitySegments -gt $aimCommandFrames -or
            $aimTargetSwitches -lt 0 -or
            $aimTargetStateChanges -lt 0 -or
            $aimPredictionStateChanges -lt 0 -or
            $aimDirectionReversals -lt 0 -or
            $aimLimitBoundaryFrames -lt 0 -or
            $aimLimitBoundaryFrames -gt $aimCommandFrames -or
            [double]::IsNaN($aimLimitBoundaryRate) -or
            [double]::IsInfinity($aimLimitBoundaryRate) -or
            $aimLimitBoundaryRate -lt 0.0 -or
            $aimLimitBoundaryRate -gt 1.0 -or
            ($aimCommandFrames -eq 0 -and
                [math]::Abs($aimLimitBoundaryRate) -gt 0.0001) -or
            ($aimCommandFrames -gt 0 -and
                [math]::Abs($aimLimitBoundaryRate -
                    ($aimLimitBoundaryFrames / $aimCommandFrames)) -gt
                        0.0001) -or
            [long]$aimDistributions.abs_dx.samples -ne $aimCommandFrames -or
            [long]$aimDistributions.abs_dy.samples -ne $aimCommandFrames -or
            [long]$aimDistributions.magnitude.samples -ne $aimCommandFrames -or
            ([long]$aimDistributions.delta.samples +
                $aimContinuitySegments) -ne $aimCommandFrames -or
            [long]$aimDistributions.acceleration.samples -gt
                [long]$aimDistributions.delta.samples -or
            $aimDirectionReversals -gt
                [long]$aimDistributions.delta.samples -or
            $missingAimConfigurationValues.Count -ne 0 -or
            $invalidAimFloatingConfiguration -or
            [double]$row.aim_high_confidence -lt 0.0 -or
            [double]$row.aim_low_confidence -lt 0.0 -or
            [double]$row.aim_low_confidence -gt
                [double]$row.aim_high_confidence -or
            [int]$row.aim_min_confirmed_hits -le 0 -or
            [int]$row.aim_max_lost_frames -lt 0 -or
            [double]$row.aim_min_iou -lt 0.0 -or
            [double]$row.aim_min_iou -gt 1.0 -or
            [double]$row.aim_max_center_distance -le 0.0 -or
            [double]$row.aim_switch_margin -lt 0.0 -or
            [double]$row.aim_switch_margin -ge 1.0 -or
            [int]$row.aim_switch_confirm_frames -le 0 -or
            [int]$row.aim_switch_cooldown_frames -lt 0 -or
            [double]$row.aim_body_aim_height_ratio -lt 0.0 -or
            [double]$row.aim_body_aim_height_ratio -gt 1.0 -or
            [double]$row.aim_deadzone_pixels -lt 0.0 -or
            [double]$row.aim_smoothing -lt 0.0 -or
            [double]$row.aim_smoothing -gt 1.0 -or
            [double]$row.aim_counts_per_pixel_x -le 0.0 -or
            [double]$row.aim_counts_per_pixel_y -le 0.0 -or
            [double]$row.aim_max_counts_per_frame -le 0.0 -or
            [double]$row.aim_predicted_gain -lt 0.0 -or
            [double]$row.aim_predicted_gain -gt 1.0 -or
            [double]$row.aim_evaluation_min_iou -lt 0.0 -or
            [double]$row.aim_evaluation_min_iou -gt 1.0 -or
            [double]$row.aim_evaluation_max_center_distance -le 0.0 -or
            [double]$row.aim_timebase_fps -le 0.0) {
            throw "Aim 控制连续性统计契约不一致：$($row.scene)"
        }
    } elseif ($aimContinuityField -ne 0 -or $aimControlComplete -ne 0 -or
              $aimControlEvaluatedFrames -ne 0 -or
              $aimControlInvalidFrames -ne 0 -or
              $aimCommandFrames -ne 0 -or
              $aimObservedCommandFrames -ne 0 -or
              $aimPredictedCommandFrames -ne 0 -or
              $aimTargetWithoutCommandFrames -ne 0 -or
              $aimNoTargetFrames -ne 0 -or
              $aimPredictedTargetFrames -ne 0 -or
              $aimContinuitySegments -ne 0 -or
              $aimTargetSwitches -ne 0 -or
              $aimTargetStateChanges -ne 0 -or
              $aimPredictionStateChanges -ne 0 -or
              $aimDirectionReversals -ne 0 -or
              $aimLimitBoundaryFrames -ne 0 -or
              [math]::Abs($aimLimitBoundaryRate) -gt 0.0001 -or
              @($aimDistributions.Values | Where-Object {
                  [long]$_.samples -ne 0
              }).Count -ne 0 -or
              $missingAimConfigurationValues.Count -ne
                  $aimConfigurationValues.Count) {
        throw "未启用 Aim 连续性时不得生成控制指标：$($row.scene)"
    }
    if ($useAimAnnotations) {
        if ($aimAnnotations -ne 1 -or $aimComplete -ne 1 -or
            $row.aim_policy -ne "aim_ground_truth_v1" -or
            $aimAnnotatedFrames -ne $frames -or
            $aimVisibleFrames -lt 0 -or
            ($aimVisibleFrames + $aimNotVisibleFrames +
                $aimIgnoredFrames) -ne $frames -or
            $aimMatchedVisibleFrames -lt 0 -or
            $aimMissedVisibleFrames -lt 0 -or
            ($aimMatchedVisibleFrames + $aimMissedVisibleFrames) -ne
                $aimVisibleFrames -or
            $aimMatchedVisibleFrames -gt $aimVisibleFrames -or
            $aimIdSwitches -lt 0 -or $aimTrackFragments -lt 0 -or
            $aimFragmentationEvents -lt 0 -or
            $aimUnnecessarySwitches -lt 0 -or
            $aimOutputTargetFrames -lt 0 -or
            $aimOutputTargetFrames -gt $frames -or
            $aimInvalidFrames -ne 0) {
            throw "Aim 真值统计契约不一致：$($row.scene)"
        }
        if ($aimVisibleFrames -gt 0) {
            $aimRecall = [double]$row.aim_roi_recall
            if ($aimRecall -lt 0.0 -or $aimRecall -gt 1.0) {
                throw "Aim ROI Recall 无效：$($row.scene)"
            }
        } elseif (-not [string]::IsNullOrEmpty($row.aim_roi_recall)) {
            throw "没有 Aim 可见目标时 Recall 必须留空：$($row.scene)"
        }
    } elseif ($aimAnnotations -ne 0 -or $aimComplete -ne 0 -or
              -not [string]::IsNullOrEmpty($row.aim_policy) -or
              $aimAnnotatedFrames -ne 0 -or
              $aimVisibleFrames -ne 0 -or
              $aimMatchedVisibleFrames -ne 0 -or
              $aimMissedVisibleFrames -ne 0 -or
              -not [string]::IsNullOrEmpty($row.aim_roi_recall) -or
              $aimIdSwitches -ne 0 -or $aimTrackFragments -ne 0 -or
              $aimFragmentationEvents -ne 0 -or
              $aimUnnecessarySwitches -ne 0 -or
              $aimNotVisibleFrames -ne 0 -or $aimIgnoredFrames -ne 0 -or
              $aimOutputTargetFrames -ne 0 -or $aimInvalidFrames -ne 0) {
        throw "无 Aim 真值模式不得生成伪追踪指标：$($row.scene)"
    }
    if ($useVisibilityAnnotations) {
        if ($annotationsPresent -ne 1 -or
            $row.visibility_policy -ne "target_frame_visibility_v1" -or
            $annotatedFrames -ne $frames -or
            ($visibleFrames + $notVisibleFrames + $ignoredFrames) -ne
                $frames -or
            ($visibleDetectedFrames + $visibleMissedFrames) -ne
                $visibleFrames -or
            $notVisibleDetectedFrames -gt $notVisibleFrames -or
            $ignoredDetectedFrames -gt $ignoredFrames -or
            [long]$row.longest_visible_miss_sequence -gt
                $visibleMissedFrames) {
            throw "可见性标注统计契约不一致：$($row.scene)"
        }
        if ($visibleFrames -gt 0) {
            $visibleRecall = [double]$row.visible_frame_recall
            if ($recallAvailable -ne 1 -or $visibleRecall -lt 0.0 -or
                $visibleRecall -gt 1.0) {
                throw "可见帧 Recall 无效：$($row.scene)"
            }
        } elseif ($recallAvailable -ne 0 -or
                  -not [string]::IsNullOrEmpty(
                      $row.visible_frame_recall)) {
            throw "没有可见帧时 Recall 必须显式不可用：$($row.scene)"
        }
        $evaluableRate = [double]$row.evaluable_frame_rate
        if ($evaluableRate -lt 0.0 -or $evaluableRate -gt 1.0) {
            throw "可评价帧覆盖率无效：$($row.scene)"
        }
    } elseif ($annotationsPresent -ne 0 -or $recallAvailable -ne 0 -or
              $annotatedFrames -ne 0 -or $visibleFrames -ne 0 -or
              $notVisibleFrames -ne 0 -or $ignoredFrames -ne 0 -or
              -not [string]::IsNullOrEmpty($row.visible_frame_recall) -or
              -not [string]::IsNullOrEmpty($row.evaluable_frame_rate)) {
        throw "无标注模式不得生成伪 Recall：$($row.scene)"
    }
}
if ($useAimAnnotations -and $aimVisibleFramesTotal -eq 0) {
    throw "Aim 真值集合没有可见目标，拒绝发布全 ignore 模板报告。"
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
$aimConfiguration = $null
if ($useAimContinuity) {
    $aimConfigurationKeys = @($reportRows | ForEach-Object {
        @(
            $_.aim_person_class_ids, $_.aim_head_class_ids,
            $_.aim_high_confidence, $_.aim_low_confidence,
            $_.aim_min_confirmed_hits, $_.aim_max_lost_frames,
            $_.aim_min_iou, $_.aim_max_center_distance,
            $_.aim_switch_margin, $_.aim_switch_confirm_frames,
            $_.aim_switch_cooldown_frames, $_.aim_body_aim_height_ratio,
            $_.aim_deadzone_pixels, $_.aim_smoothing,
            $_.aim_counts_per_pixel_x, $_.aim_counts_per_pixel_y,
            $_.aim_max_counts_per_frame, $_.aim_predicted_gain,
            $_.aim_evaluation_min_iou,
            $_.aim_evaluation_max_center_distance,
            $_.aim_timebase_fps
        ) -join "|"
    } | Sort-Object -Unique)
    if ($aimConfigurationKeys.Count -ne 1) {
        throw "Detector 视频基准出现多套 Aim 追踪或评价配置。"
    }
    $aimRow = $reportRows[0]
    $aimConfiguration = [ordered]@{
        person_class_ids = @($aimRow.aim_person_class_ids -split ";" |
            ForEach-Object { [int]$_ })
        head_class_ids = @($aimRow.aim_head_class_ids -split ";" |
            ForEach-Object { [int]$_ })
        high_confidence = [double]$aimRow.aim_high_confidence
        low_confidence = [double]$aimRow.aim_low_confidence
        min_confirmed_hits = [int]$aimRow.aim_min_confirmed_hits
        max_lost_frames = [int]$aimRow.aim_max_lost_frames
        min_iou = [double]$aimRow.aim_min_iou
        max_center_distance = [double]$aimRow.aim_max_center_distance
        switch_margin = [double]$aimRow.aim_switch_margin
        switch_confirm_frames = [int]$aimRow.aim_switch_confirm_frames
        switch_cooldown_frames = [int]$aimRow.aim_switch_cooldown_frames
        body_aim_height_ratio = [double]$aimRow.aim_body_aim_height_ratio
        deadzone_pixels = [double]$aimRow.aim_deadzone_pixels
        smoothing = [double]$aimRow.aim_smoothing
        counts_per_pixel_x = [double]$aimRow.aim_counts_per_pixel_x
        counts_per_pixel_y = [double]$aimRow.aim_counts_per_pixel_y
        max_counts_per_frame = [double]$aimRow.aim_max_counts_per_frame
        predicted_gain = [double]$aimRow.aim_predicted_gain
        evaluation_min_iou = [double]$aimRow.aim_evaluation_min_iou
        evaluation_max_center_distance =
            [double]$aimRow.aim_evaluation_max_center_distance
        synthetic_timebase_fps = [double]$aimRow.aim_timebase_fps
    }
}

$manifest = [ordered]@{
    schema_version = 4
    generated_utc = [DateTime]::UtcNow.ToString("o")
    report = [ordered]@{
        csv = $finalReportPath
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
        visibility = [ordered]@{
            enabled = $useVisibilityAnnotations
            directory = if ($useVisibilityAnnotations) {
                $VisibilityDirectory
            } else { $null }
            schema_version = if ($useVisibilityAnnotations) { 1 } else {
                $null
            }
            policy = if ($useVisibilityAnnotations) {
                "target_frame_visibility_v1"
            } else { $null }
            inventory_sha256 = $visibilityFingerprint
            files = $visibilityInventory
        }
        aim = [ordered]@{
            enabled = $useAimContinuity
            continuity_policy = if ($useAimContinuity) {
                "aim_control_continuity_v1"
            } else { $null }
            ground_truth_enabled = $useAimAnnotations
            directory = if ($useAimAnnotations) {
                $AimAnnotationDirectory
            } else { $null }
            schema_version = if ($useAimAnnotations) { 1 } else {
                $null
            }
            policy = if ($useAimAnnotations) {
                "aim_ground_truth_v1"
            } else { $null }
            inventory_sha256 = $aimFingerprint
            files = $aimInventory
            configuration = $aimConfiguration
        }
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
        annotations_unchanged_during_run = $true
        recall_requires_visibility_annotations = $true
        aim_metrics_require_ground_truth = $true
        aim_continuity_supports_unannotated_video = $true
        aim_command_state_conservation = $true
        aim_continuity_resets_at_semantic_boundaries = $true
    }
    deployed_runtimes = $deployedRuntimes
}

    Publish-ExistingFileAtomically -Source $pendingReportPath `
        -Destination $finalReportPath
    Publish-JsonAtomically -Value $manifest -Path $manifestPath
    Write-Host "Detector 视频基准完成：$finalReportPath"
    Write-Host "Detector 基准环境清单：$manifestPath"
} finally {
    if (Test-Path -LiteralPath $pendingReportPath) {
        Remove-Item -LiteralPath $pendingReportPath -Force
    }
}
