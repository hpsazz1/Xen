param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$TensorRtCachePath,
    [Parameter(Mandatory = $true)]
    [string]$VideoDirectory,
    [string]$ReportDirectory = "cache/benchmarks/gpu-preprocess",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("center", "full")]
    [string]$InputMode = "center",
    [ValidateSet("auto", "channel_first", "objectness", "end_to_end")]
    [string]$OutputFormat = "auto"
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [ValidateSet("Container", "Leaf")]
        [string]$PathType,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Invoke-Benchmark {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Mode,
        [Parameter(Mandatory = $true)]
        [string]$ReportPath
    )

    & $executable $modelPath tensorrt $cachePath $videoPath $ReportPath `
        $InputMode --output-format $OutputFormat --gpu-preprocess $Mode
    if ($LASTEXITCODE -ne 0) {
        throw "GPU 前处理 $Mode 基准失败，退出码：$LASTEXITCODE"
    }
}

$buildPath = Resolve-RequiredPath $BuildDirectory Container "构建目录"
$modelPath = Resolve-RequiredPath $ModelPath Leaf "ONNX 模型"
$videoPath = Resolve-RequiredPath $VideoDirectory Container "视频目录"
$cachePath = [System.IO.Path]::GetFullPath($TensorRtCachePath)
$reportPath = [System.IO.Path]::GetFullPath($ReportDirectory)
$executable = Join-Path $buildPath "$Configuration\detector_model_test.exe"
$executable = Resolve-RequiredPath $executable Leaf "Detector 真实模型测试程序"
New-Item -ItemType Directory -Force -Path $cachePath | Out-Null
New-Item -ItemType Directory -Force -Path $reportPath | Out-Null

$onReport = Join-Path $reportPath "gpu-preprocess-on.csv"
$offReport = Join-Path $reportPath "gpu-preprocess-off.csv"
$originalPath = $env:PATH
try {
    # 只保留系统目录，确保运行库来自可执行文件输出目录，而不是开发机 SDK PATH。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    Invoke-Benchmark on $onReport
    Invoke-Benchmark off $offReport
} finally {
    $env:PATH = $originalPath
}

$onRows = @(Import-Csv -LiteralPath $onReport)
$offRows = @(Import-Csv -LiteralPath $offReport)
if ($onRows.Count -eq 0 -or $onRows.Count -ne $offRows.Count) {
    throw "GPU 前处理 on/off 报告场景数量不一致。"
}

$results = @()
for ($index = 0; $index -lt $onRows.Count; ++$index) {
    $on = $onRows[$index]
    $off = $offRows[$index]
    foreach ($field in @(
            "scene", "frames", "failed_frames", "status",
            "detected_frames", "detection_frame_rate",
            "longest_empty_sequence", "mean_detection_count",
            "mean_best_confidence")) {
        if ($on.$field -ne $off.$field) {
            throw "场景 $($on.scene) 的正确性字段 $field 在 on/off 间不一致。"
        }
    }
    if ($on.status -ne "SUCCESS" -or
        [int]$on.failed_frames -ne 0 -or
        [int]$off.failed_frames -ne 0 -or
        [int]$on.gpu_preprocess -ne 1 -or
        [int]$off.gpu_preprocess -ne 0) {
        throw "场景 $($on.scene) 的执行状态或 GPU 前处理标记不符合契约。"
    }

    $inputPixels = [uint64]$on.model_input_width *
        [uint64]$on.model_input_height
    $expectedGpuUpload = $inputPixels * 3
    $expectedCpuUpload = $expectedGpuUpload * 4
    if ([uint64]$on.input_upload_bytes -ne $expectedGpuUpload -or
        [uint64]$off.input_upload_bytes -ne $expectedCpuUpload) {
        throw "场景 $($on.scene) 的输入上传字节数不符合 uint8/float32 契约。"
    }

    $onP95 = [double]$on.total_p95_ms
    $offP95 = [double]$off.total_p95_ms
    $onP99 = [double]$on.total_p99_ms
    $offP99 = [double]$off.total_p99_ms
    $results += [pscustomobject]@{
        Scene = $on.scene
        Frames = [int]$on.frames
        UploadOnBytes = [uint64]$on.input_upload_bytes
        UploadOffBytes = [uint64]$off.input_upload_bytes
        OnMeanMs = [double]$on.total_mean_ms
        OffMeanMs = [double]$off.total_mean_ms
        OnP50Ms = [double]$on.total_p50_ms
        OffP50Ms = [double]$off.total_p50_ms
        OnP95Ms = $onP95
        OffP95Ms = $offP95
        P95ChangePercent = if ($offP95 -gt 0) {
            100.0 * ($onP95 - $offP95) / $offP95
        } else { 0.0 }
        OnP99Ms = $onP99
        OffP99Ms = $offP99
        P99ChangePercent = if ($offP99 -gt 0) {
            100.0 * ($onP99 - $offP99) / $offP99
        } else { 0.0 }
    }
}

$results | Format-Table -AutoSize
Write-Host "GPU 前处理报告：$onReport"
Write-Host "CPU 前处理报告：$offReport"
