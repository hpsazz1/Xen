param(
    [Parameter(Mandatory = $true)]
    [string]$VideoDirectory,
    [Parameter(Mandatory = $true)]
    [string]$BenchmarkReport,
    [string]$OutputDirectory = (Join-Path $VideoDirectory "visibility")
)

$ErrorActionPreference = "Stop"

function Get-VideoFiles {
    param([Parameter(Mandatory = $true)][string]$Directory)

    return @(Get-ChildItem -LiteralPath $Directory -File | Where-Object {
        $_.Extension -ieq ".mp4" -or $_.Extension -ieq ".avi"
    } | Sort-Object Name)
}

if (-not (Test-Path -LiteralPath $VideoDirectory -PathType Container)) {
    throw "视频目录不存在：$VideoDirectory"
}
if (-not (Test-Path -LiteralPath $BenchmarkReport -PathType Leaf)) {
    throw "Detector 视频基准 CSV 不存在：$BenchmarkReport"
}

$VideoDirectory = [System.IO.Path]::GetFullPath($VideoDirectory)
$BenchmarkReport = [System.IO.Path]::GetFullPath($BenchmarkReport)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$videos = @(Get-VideoFiles $VideoDirectory)
$rows = @(Import-Csv -LiteralPath $BenchmarkReport)
if ($videos.Count -eq 0 -or $rows.Count -ne $videos.Count) {
    throw "视频集合与基准 CSV 场景数不一致。"
}

$rowsByScene = @{}
foreach ($row in $rows) {
    if ($rowsByScene.ContainsKey($row.scene)) {
        throw "基准 CSV 包含重复场景：$($row.scene)"
    }
    $rowsByScene[$row.scene] = $row
}

if (-not (Test-Path -LiteralPath $OutputDirectory)) {
    New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
}
if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
    throw "标注输出路径不是目录：$OutputDirectory"
}

$pending = @()
foreach ($video in $videos) {
    if (-not $rowsByScene.ContainsKey($video.BaseName)) {
        throw "基准 CSV 缺少视频场景：$($video.BaseName)"
    }
    $row = $rowsByScene[$video.BaseName]
    $sourceWidth = [int]$row.source_width
    $sourceHeight = [int]$row.source_height
    $evaluatedWidth = [int]$row.evaluated_width
    $evaluatedHeight = [int]$row.evaluated_height
    $modelWidth = [int]$row.model_input_width
    $modelHeight = [int]$row.model_input_height
    $frames = [int]$row.frames
    if ($row.status -ne "SUCCESS" -or [int]$row.failed_frames -ne 0 -or
        $frames -le 0 -or $sourceWidth -le 0 -or $sourceHeight -le 0 -or
        $evaluatedWidth -ne $modelWidth -or
        $evaluatedHeight -ne $modelHeight -or
        $evaluatedWidth -gt $sourceWidth -or
        $evaluatedHeight -gt $sourceHeight) {
        throw "场景不是可用于 center ROI 标注的成功基准：$($row.scene)"
    }

    $targetPath = Join-Path $OutputDirectory `
        ("{0}.visibility.json" -f $video.Name)
    if (Test-Path -LiteralPath $targetPath) {
        throw "拒绝覆盖已有标注：$targetPath"
    }
    $pending += [ordered]@{
        path = $targetPath
        value = [ordered]@{
            schema_version = 1
            video_file = $video.Name
            video_sha256 = (Get-FileHash -LiteralPath $video.FullName `
                -Algorithm SHA256).Hash
            source_width = $sourceWidth
            source_height = $sourceHeight
            frame_count = $frames
            input_mode = "center"
            roi_x = [int](($sourceWidth - $evaluatedWidth) / 2)
            roi_y = [int](($sourceHeight - $evaluatedHeight) / 2)
            roi_width = $evaluatedWidth
            roi_height = $evaluatedHeight
            policy = "target_frame_visibility_v1"
            intervals = @(
                [ordered]@{
                    start_frame = 0
                    end_frame = $frames - 1
                    state = "ignore"
                }
            )
        }
    }
}

$encoding = New-Object System.Text.UTF8Encoding($true)
$created = @()
try {
    foreach ($entry in $pending) {
        $temporaryPath = "{0}.{1}.tmp" -f $entry.path, $PID
        try {
            $json = $entry.value | ConvertTo-Json -Depth 5
            [System.IO.File]::WriteAllText($temporaryPath, $json, $encoding)
            Move-Item -LiteralPath $temporaryPath -Destination $entry.path
            $created += $entry.path
        } finally {
            if (Test-Path -LiteralPath $temporaryPath) {
                Remove-Item -LiteralPath $temporaryPath -Force
            }
        }
    }
} catch {
    # 本次调用只创建新文件，失败时删除已创建项，避免留下场景集合不完整的半套模板。
    foreach ($path in $created) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    throw
}

Write-Host "已生成 $($pending.Count) 份标注模板：$OutputDirectory"
Write-Host "模板区间全部是 ignore，不是真值；人工复核后再改为 visible/not_visible/ignore。"
