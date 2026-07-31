param(
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,
    [string]$BuildDirectory = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateRange(0, 64)]
    [int]$AdapterIndex = 0,
    [ValidateRange(0, 64)]
    [int]$OutputIndex = 0,
    [ValidateRange(1, 32768)]
    [int]$ExpectedSourceWidth = 2560,
    [ValidateRange(1, 32768)]
    [int]$ExpectedSourceHeight = 1440,
    [ValidateRange(1, 32768)]
    [int]$RoiWidth = 320,
    [ValidateRange(1, 32768)]
    [int]$RoiHeight = 320,
    [int]$RoiX = -1,
    [int]$RoiY = -1,
    [ValidateRange(1, 100)]
    [int]$JpegQuality = 85,
    [ValidateRange(125, 65507)]
    [int]$DatagramBytes = 1400,
    [ValidateRange(1, 1000000)]
    [int]$Fps = 240,
    [ValidateRange(1, 600)]
    [int]$DurationSeconds = 300,
    [ValidateRange(1, 200000)]
    [int]$MinimumFrames = 10000
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build"
}

function Get-FileEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        last_write_utc = $file.LastWriteTimeUtc.ToString("o")
        file_version = $file.VersionInfo.FileVersion
        product_version = $file.VersionInfo.ProductVersion
    }
}

function Get-DllEvidence {
    param([Parameter(Mandatory = $true)][string]$Directory)
    $result = [ordered]@{}
    foreach ($file in @(Get-ChildItem -LiteralPath $Directory -File `
            -Filter "*.dll" | Sort-Object Name)) {
        $result[$file.Name] = Get-FileEvidence $file.FullName
    }
    return $result
}

function Assert-SnapshotUnchanged {
    param(
        [System.Collections.IDictionary]$Before,
        [System.Collections.IDictionary]$After,
        [string]$Description
    )
    if ($Before.sha256 -ne $After.sha256 -or
        $Before.length -ne $After.length) {
        throw "$Description 在发送基准期间发生变化。"
    }
}

if ($Destination.IndexOfAny([char[]]"`r`n") -ge 0 -or
    $Destination -notmatch '^udp://[^:]+:[0-9]+$' -or
    $Destination -match '^udp://(0\.0\.0\.0|\*):') {
    throw "Destination 必须是可发送的 udp://IPv4或主机名:端口。"
}
if (($RoiX -lt 0) -ne ($RoiY -lt 0)) {
    throw "显式 ROI 必须同时提供 RoiX 和 RoiY。"
}
$useExplicitRoi = $RoiX -ge 0
$expectedRoiX = if ($useExplicitRoi) {
    $RoiX
} else {
    [int](($ExpectedSourceWidth - $RoiWidth) / 2)
}
$expectedRoiY = if ($useExplicitRoi) {
    $RoiY
} else {
    [int](($ExpectedSourceHeight - $RoiHeight) / 2)
}
if ($expectedRoiX -lt 0 -or $expectedRoiY -lt 0 -or
    $expectedRoiX + $RoiWidth -gt $ExpectedSourceWidth -or
    $expectedRoiY + $RoiHeight -gt $ExpectedSourceHeight) {
    throw "期望 ROI 超出主机 FOV。"
}
if (-not $useExplicitRoi -and
    ($expectedRoiX * 2 + $RoiWidth -ne $ExpectedSourceWidth -or
     $expectedRoiY * 2 + $RoiHeight -ne $ExpectedSourceHeight)) {
    throw "主机 FOV 与 ROI 不能形成精确整数中心裁剪。"
}
if ([int64]$DurationSeconds * [int64]$Fps -gt 200000) {
    throw "时长与 FPS 上限可能超过发送报告 200000 样本容量。"
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$executable = Join-Path $BuildDirectory "$Configuration\XenSender.exe"
$cmakeCache = Join-Path $BuildDirectory "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf) -or
    -not (Test-Path -LiteralPath $cmakeCache -PathType Leaf)) {
    throw "XenSender 或 CMakeCache.txt 不存在：$BuildDirectory"
}
$outputDirectory = Split-Path -Parent $executable
$ReportPrefix = [System.IO.Path]::GetFullPath($ReportPrefix)
$reportDirectory = Split-Path -Parent $ReportPrefix
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$finalReport = "$ReportPrefix.sender.json"
$finalEnvironment = "$ReportPrefix.sender.environment.json"
$pendingId = [guid]::NewGuid().ToString("N")
$pendingReport = "$finalReport.pending-$pendingId"
$pendingEnvironment = "$finalEnvironment.pending-$pendingId"
foreach ($path in @($finalReport, $finalEnvironment)) {
    if (Test-Path -LiteralPath $path) {
        throw "XUDP 发送基准目标已存在，拒绝覆盖：$path"
    }
}

$executableBefore = Get-FileEvidence $executable
$cacheBefore = Get-FileEvidence $cmakeCache
$dllsBefore = Get-DllEvidence $outputDirectory
$arguments = @(
    "--destination", $Destination,
    "--adapter", [string]$AdapterIndex,
    "--output", [string]$OutputIndex,
    "--roi-width", [string]$RoiWidth,
    "--roi-height", [string]$RoiHeight,
    "--jpeg-quality", [string]$JpegQuality,
    "--fps", [string]$Fps,
    "--datagram-bytes", [string]$DatagramBytes,
    "--max-seconds", [string]$DurationSeconds,
    "--report", $pendingReport
)
if ($useExplicitRoi) {
    $arguments += @("--roi-x", [string]$RoiX, "--roi-y", [string]$RoiY)
}

$startedUtc = [DateTime]::UtcNow
$originalTaskPath = $env:PATH
$exitCode = -1
$published = New-Object System.Collections.Generic.List[string]
try {
    try {
        $env:PATH = @(
            (Join-Path $env:SystemRoot "System32"),
            $env:SystemRoot
        ) -join ";"
        Push-Location $repositoryRoot
        try {
            & $executable @arguments
            $exitCode = $LASTEXITCODE
        } finally {
            Pop-Location
        }
    } finally {
        $env:PATH = $originalTaskPath
    }
    $finishedUtc = [DateTime]::UtcNow
    if ($exitCode -ne 0) {
        throw "XenSender 失败，退出码：$exitCode"
    }
    if (-not (Test-Path -LiteralPath $pendingReport -PathType Leaf)) {
        throw "XenSender 成功退出但没有生成 pending 报告。"
    }

    $executableAfter = Get-FileEvidence $executable
    $cacheAfter = Get-FileEvidence $cmakeCache
    $dllsAfter = Get-DllEvidence $outputDirectory
    Assert-SnapshotUnchanged $executableBefore $executableAfter "XenSender"
    Assert-SnapshotUnchanged $cacheBefore $cacheAfter "CMake Cache"
    if (($dllsBefore.Keys -join "`n") -ne ($dllsAfter.Keys -join "`n")) {
        throw "发送基准期间部署 DLL 集合发生变化。"
    }
    foreach ($name in $dllsBefore.Keys) {
        Assert-SnapshotUnchanged $dllsBefore[$name] $dllsAfter[$name] `
            "部署 DLL $name"
    }

    $report = Get-Content -LiteralPath $pendingReport -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($report.schema -ne 1 -or -not $report.complete -or
        $report.stop_reason -ne "duration" -or
        [double]$report.elapsed_seconds -lt $DurationSeconds -or
        [uint64]$report.stats.frames_sent -lt $MinimumFrames -or
        [uint64]$report.stats.frames_failed -ne 0 -or
        [uint64]$report.stats.samples_dropped -ne 0 -or
        @($report.samples).Count -ne [uint64]$report.stats.frames_sent) {
        throw "XUDP 发送样本、时长、失败或丢弃不符合正式门槛。"
    }
    if ($report.destination_url -ne $Destination -or
        $report.geometry.source_width -ne $ExpectedSourceWidth -or
        $report.geometry.source_height -ne $ExpectedSourceHeight -or
        $report.geometry.encoded_width -ne $RoiWidth -or
        $report.geometry.encoded_height -ne $RoiHeight -or
        $report.geometry.roi_x -ne $expectedRoiX -or
        $report.geometry.roi_y -ne $expectedRoiY -or
        $report.geometry.roi_width -ne $RoiWidth -or
        $report.geometry.roi_height -ne $RoiHeight -or
        [double]$report.geometry.source_pixels_per_pixel_x -ne 1.0 -or
        [double]$report.geometry.source_pixels_per_pixel_y -ne 1.0) {
        throw "XUDP 发送报告的目的地址或主机几何不符合声明。"
    }
    if ([uint64]$report.stats.largest_datagram_bytes -gt $DatagramBytes) {
        throw "XUDP 实际数据报超过声明上限。"
    }

    $gitCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "读取 Git commit 失败。" }
    $gitStatus = @(& git -C $repositoryRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw "读取 Git 工作树状态失败。" }
    $environment = [ordered]@{
        schema = 1
        complete = $true
        role = "sender"
        started_utc = $startedUtc.ToString("o")
        finished_utc = $finishedUtc.ToString("o")
        duration_seconds = ($finishedUtc - $startedUtc).TotalSeconds
        git = [ordered]@{
            commit = $gitCommit
            dirty = $gitStatus.Count -ne 0
            status = $gitStatus
        }
        build = [ordered]@{
            directory = $BuildDirectory
            configuration = $Configuration
            cmake_cache = $cacheAfter
            executable = $executableAfter
            runtime_dlls = $dllsAfter
        }
        network = [ordered]@{
            destination = $Destination
            cross_machine_clock_basis =
                "unsynchronized_monotonic_clocks; no strict frame age"
            auxiliary_display_resolution_is_not_coordinate_input = $true
        }
        geometry = $report.geometry
        sender = [ordered]@{
            jpeg_quality = $JpegQuality
            datagram_bytes = $DatagramBytes
            requested_fps = $Fps
            minimum_frames = $MinimumFrames
            frames_sent = [uint64]$report.stats.frames_sent
            frames_failed = [uint64]$report.stats.frames_failed
            datagrams_sent = [uint64]$report.stats.datagrams_sent
            jpeg_bytes_sent = [uint64]$report.stats.jpeg_bytes_sent
            wire_bytes_sent = [uint64]$report.stats.wire_bytes_sent
            timing = $report.timing
        }
        report = Get-FileEvidence $pendingReport
    }
    $environment.report["path"] = $finalReport
    $environmentText = $environment | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText(
        $pendingEnvironment, $environmentText,
        [System.Text.UTF8Encoding]::new($false))

    [System.IO.File]::Move($pendingReport, $finalReport)
    $published.Add($finalReport)
    [System.IO.File]::Move($pendingEnvironment, $finalEnvironment)
    $published.Add($finalEnvironment)
    Write-Host "XUDP 主机正式发送基准通过："
    Write-Host "  frames=$($report.stats.frames_sent), failed=0"
    Write-Host "  capture-to-send P95=$($report.timing.capture_to_send.p95_ms) ms"
    Write-Host "  报告：$finalReport"
    Write-Host "  环境清单：$finalEnvironment"
} catch {
    foreach ($path in $published) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    throw
} finally {
    Remove-Item -LiteralPath $pendingReport -Force `
        -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $pendingEnvironment -Force `
        -ErrorAction SilentlyContinue
}
