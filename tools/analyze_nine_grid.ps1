[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [double]$SegmentGapMs = 100.0,
    [string]$OutputCsv = '',
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PercentileValue {
    param(
        [Parameter(Mandatory)][double[]]$Values,
        [Parameter(Mandatory)][ValidateRange(0.0, 1.0)][double]$Percentile
    )

    if ($Values.Count -eq 0) {
        return 0.0
    }

    $sorted = @($Values | Sort-Object)
    $index = [math]::Floor($Percentile * ($sorted.Count - 1))
    return [double]$sorted[$index]
}

function Split-NineGridSegments {
    param(
        [Parameter(Mandatory)][object[]]$Rows,
        [Parameter(Mandatory)][double]$GapMs
    )

    $segments = [System.Collections.Generic.List[object]]::new()
    $current = [System.Collections.Generic.List[object]]::new()
    $previousTimestamp = $null

    foreach ($row in $Rows) {
        $timestamp = [double]$row.Timestamp
        # 人工切换九宫格位置时会形成明显空档；使用时间边界而非固定行数，兼容不同后端 FPS。
        if ($null -ne $previousTimestamp -and ($timestamp - [double]$previousTimestamp) -gt $GapMs) {
            if ($current.Count -gt 0) {
                $segments.Add(@($current))
                $current = [System.Collections.Generic.List[object]]::new()
            }
        }
        $current.Add($row)
        $previousTimestamp = $timestamp
    }

    if ($current.Count -gt 0) {
        $segments.Add(@($current))
    }
    return @($segments)
}

function Get-GridPositionName {
    param(
        [Parameter(Mandatory)][string]$Direction,
        [Parameter(Mandatory)][int]$SegmentIndex
    )

    # Windows PowerShell 5 会把无 BOM 的 UTF-8 脚本按本地代码页解析，因此方位字符使用
    # Unicode 码点构造，保证脚本在 PowerShell 5 与 PowerShell 7 下得到相同文件名和标签。
    $left = [string][char]0x5DE6
    $middle = [string][char]0x4E2D
    $right = [string][char]0x53F3
    $up = [string][char]0x4E0A
    $down = [string][char]0x4E0B
    $suffixes = if ($Direction -eq $up) { @($left, $middle, $right) } else { @($up, $middle, $down) }
    if ($SegmentIndex -lt $suffixes.Count) {
        return "$Direction$($suffixes[$SegmentIndex])"
    }
    return "$Direction#$($SegmentIndex + 1)"
}

function Get-SegmentMetric {
    param(
        [Parameter(Mandatory)][object[]]$Rows,
        [Parameter(Mandatory)][string]$Backend,
        [Parameter(Mandatory)][string]$Transport,
        [Parameter(Mandatory)][string]$Direction,
        [Parameter(Mandatory)][int]$SegmentIndex
    )

    $first = $Rows[0]
    $settleIndex = -1
    for ($index = 0; $index -lt $Rows.Count; ++$index) {
        if ([int]$Rows[$index].Settled -eq 1) {
            $settleIndex = $index
            break
        }
    }

    $settleMs = $null
    $postSettleMaxError = $null
    $settleExitCount = 0
    if ($settleIndex -ge 0) {
        $settleMs = [double]$Rows[$settleIndex].Timestamp - [double]$first.Timestamp
        $postSettleRows = @($Rows[$settleIndex..($Rows.Count - 1)])
        $postSettleMaxError = [double]($postSettleRows | Measure-Object ErrorDistance -Maximum).Maximum
        for ($index = $settleIndex + 1; $index -lt $Rows.Count; ++$index) {
            if ([int]$Rows[$index - 1].Settled -eq 1 -and [int]$Rows[$index].Settled -eq 0) {
                ++$settleExitCount
            }
        }
    }

    $up = [string][char]0x4E0A
    $mainAxisColumn = if ($Direction -eq $up) { 'ErrorY' } else { 'ErrorX' }
    $centerCrossings = 0
    for ($index = 1; $index -lt $Rows.Count; ++$index) {
        $previousError = [double]$Rows[$index - 1].$mainAxisColumn
        $currentError = [double]$Rows[$index].$mainAxisColumn
        if (($previousError -lt 0.0 -and $currentError -gt 0.0) -or
            ($previousError -gt 0.0 -and $currentError -lt 0.0)) {
            ++$centerCrossings
        }
    }

    $activeRows = @($Rows | Where-Object { [int]$_.Settled -eq 0 })
    $farRows = @($Rows | Where-Object { [double]$_.ErrorDistance -gt 50.0 })
    $limitedActive = @($activeRows | Where-Object { [int]$_.SpeedLimited -eq 1 }).Count
    $limitedFar = @($farRows | Where-Object { [int]$_.SpeedLimited -eq 1 }).Count

    return [pscustomobject]@{
        Level = 'Segment'
        Chain = "$Backend+$Transport"
        Backend = $Backend
        Transport = $Transport
        Position = Get-GridPositionName -Direction $Direction -SegmentIndex $SegmentIndex
        Rows = $Rows.Count
        DurationMs = [math]::Round([double]$Rows[-1].Timestamp - [double]$first.Timestamp, 1)
        StartErrorPx = [math]::Round([double]$first.ErrorDistance, 2)
        SettleMs = if ($null -eq $settleMs) { $null } else { [math]::Round($settleMs, 1) }
        PostSettleMaxErrorPx = if ($null -eq $postSettleMaxError) { $null } else { [math]::Round($postSettleMaxError, 2) }
        SettleExitCount = $settleExitCount
        CenterCrossings = $centerCrossings
        ActiveRows = $activeRows.Count
        LimitedActiveRows = $limitedActive
        FarRows = $farRows.Count
        LimitedFarRows = $limitedFar
        LimitedActivePct = [math]::Round(100.0 * $limitedActive / [math]::Max(1, $activeRows.Count), 1)
        LimitedFarPct = [math]::Round(100.0 * $limitedFar / [math]::Max(1, $farRows.Count), 1)
        CaptureFps = [math]::Round([double]($Rows | Measure-Object FPS -Average).Average, 1)
        InferenceFps = [math]::Round([double]($Rows | Measure-Object InferenceFPS -Average).Average, 1)
        SourceReceiveFps = [math]::Round([double]($Rows | Measure-Object SourceReceiveFPS -Average).Average, 1)
        ObservationAgeAvgMs = [math]::Round(1000.0 * [double]($Rows | Measure-Object ObservationAgeSec -Average).Average, 1)
        MaxQueuedMoves = [int]($Rows | Measure-Object QueuedMoveCount -Maximum).Maximum
        SourceGeometry = "$($first.SourceWidth)x$($first.SourceHeight)"
    }
}

$resolvedRoot = [System.IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "Nine-grid data root does not exist: $resolvedRoot"
}

$requiredColumns = @(
    'Timestamp', 'SourceWidth', 'SourceHeight', 'FPS', 'InferenceFPS', 'SourceReceiveFPS',
    'ObservationAgeSec', 'ErrorX', 'ErrorY', 'ErrorDistance', 'SpeedLimited', 'Settled', 'QueuedMoveCount'
)
$segmentMetrics = [System.Collections.Generic.List[object]]::new()
$csvFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Filter '*.csv' | Sort-Object FullName)
if ($csvFiles.Count -eq 0) {
    throw "No CSV files were found under: $resolvedRoot"
}

foreach ($csvFile in $csvFiles) {
    # .NET Framework（Windows PowerShell 5）没有 Path.GetRelativePath；文件由根目录递归枚举，
    # 因此可安全去掉已验证的绝对根前缀，同时兼容 PowerShell 7。
    $rootPrefix = $resolvedRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $csvFile.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "CSV escaped the requested data root: $($csvFile.FullName)"
    }
    $relativePath = $csvFile.FullName.Substring($rootPrefix.Length)
    $parts = $relativePath -split '[\\/]'
    $validDirections = @([string][char]0x5DE6, [string][char]0x4E0A, [string][char]0x53F3)
    if ($parts.Count -ne 3 -or $validDirections -notcontains $csvFile.BaseName) {
        continue
    }

    $rows = @(Import-Csv -LiteralPath $csvFile.FullName)
    if ($rows.Count -eq 0) {
        throw "CSV has no data rows: $($csvFile.FullName)"
    }
    foreach ($column in $requiredColumns) {
        if ($rows[0].PSObject.Properties.Name -notcontains $column) {
            throw "CSV is missing required column '$column': $($csvFile.FullName)"
        }
    }

    $segments = @(Split-NineGridSegments -Rows $rows -GapMs $SegmentGapMs)
    for ($segmentIndex = 0; $segmentIndex -lt $segments.Count; ++$segmentIndex) {
        $metric = Get-SegmentMetric -Rows @($segments[$segmentIndex]) -Backend $parts[0] `
            -Transport $parts[1] -Direction $csvFile.BaseName -SegmentIndex $segmentIndex
        $segmentMetrics.Add($metric)
    }
}

$chainMetrics = @($segmentMetrics | Group-Object Chain | ForEach-Object {
    $group = @($_.Group)
    $totalRows = [int]($group | Measure-Object Rows -Sum).Sum
    $activeRows = [int]($group | Measure-Object ActiveRows -Sum).Sum
    $limitedActiveRows = [int]($group | Measure-Object LimitedActiveRows -Sum).Sum
    $farRows = [int]($group | Measure-Object FarRows -Sum).Sum
    $limitedFarRows = [int]($group | Measure-Object LimitedFarRows -Sum).Sum
    $settleValues = @($group | Where-Object { $null -ne $_.SettleMs } | ForEach-Object { [double]$_.SettleMs })
    [pscustomobject]@{
        Level = 'Chain'
        Chain = $_.Name
        Segments = $group.Count
        MeanSettleMs = [math]::Round([double]($settleValues | Measure-Object -Average).Average, 1)
        P95SettleMs = [math]::Round((Get-PercentileValue -Values $settleValues -Percentile 0.95), 1)
        WorstPostSettleErrorPx = [math]::Round([double]($group | Measure-Object PostSettleMaxErrorPx -Maximum).Maximum, 2)
        SettleExitCount = [int]($group | Measure-Object SettleExitCount -Sum).Sum
        CenterCrossings = [int]($group | Measure-Object CenterCrossings -Sum).Sum
        ActiveRows = $activeRows
        LimitedActiveRows = $limitedActiveRows
        FarRows = $farRows
        LimitedFarRows = $limitedFarRows
        LimitedActivePct = [math]::Round(100.0 * $limitedActiveRows / [math]::Max(1, $activeRows), 1)
        LimitedFarPct = [math]::Round(100.0 * $limitedFarRows / [math]::Max(1, $farRows), 1)
        CaptureFps = [math]::Round([double](($group | ForEach-Object { $_.CaptureFps * $_.Rows } | Measure-Object -Sum).Sum) / [math]::Max(1, $totalRows), 1)
        InferenceFps = [math]::Round([double](($group | ForEach-Object { $_.InferenceFps * $_.Rows } | Measure-Object -Sum).Sum) / [math]::Max(1, $totalRows), 1)
        SourceReceiveFps = [math]::Round([double](($group | ForEach-Object { $_.SourceReceiveFps * $_.Rows } | Measure-Object -Sum).Sum) / [math]::Max(1, $totalRows), 1)
        ObservationAgeAvgMs = [math]::Round([double](($group | ForEach-Object { $_.ObservationAgeAvgMs * $_.Rows } | Measure-Object -Sum).Sum) / [math]::Max(1, $totalRows), 1)
        MaxQueuedMoves = [int]($group | Measure-Object MaxQueuedMoves -Maximum).Maximum
        SourceGeometry = ($group | Select-Object -First 1).SourceGeometry
    }
})

$allMetrics = @($segmentMetrics) + $chainMetrics
if (-not [string]::IsNullOrWhiteSpace($OutputCsv)) {
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputCsv)
    $outputDirectory = Split-Path -Parent $resolvedOutput
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and -not (Test-Path -LiteralPath $outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory | Out-Null
    }
    # 明细与汇总对象字段不同，显式声明联合列，避免 Export-Csv 只采用首个对象的属性集合。
    $exportColumns = @(
        'Level', 'Chain', 'Backend', 'Transport', 'Position', 'Rows', 'DurationMs',
        'StartErrorPx', 'SettleMs', 'PostSettleMaxErrorPx', 'Segments', 'MeanSettleMs',
        'P95SettleMs', 'WorstPostSettleErrorPx', 'SettleExitCount', 'CenterCrossings',
        'ActiveRows', 'LimitedActiveRows', 'FarRows', 'LimitedFarRows', 'LimitedActivePct',
        'LimitedFarPct', 'CaptureFps', 'InferenceFps', 'SourceReceiveFps',
        'ObservationAgeAvgMs', 'MaxQueuedMoves', 'SourceGeometry'
    )
    $allMetrics | Select-Object -Property $exportColumns |
        Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
}

if ($PassThru) {
    $allMetrics
    return
}

Write-Host '[nine-grid] Segment metrics' -ForegroundColor Cyan
$segmentMetrics | Format-Table Chain, Position, Rows, StartErrorPx, SettleMs, PostSettleMaxErrorPx, `
    SettleExitCount, LimitedFarPct -AutoSize
Write-Host '[nine-grid] Chain summary' -ForegroundColor Cyan
$chainMetrics | Format-Table Chain, Segments, MeanSettleMs, P95SettleMs, WorstPostSettleErrorPx, `
    SettleExitCount, LimitedFarPct, InferenceFps, SourceReceiveFps, ObservationAgeAvgMs, MaxQueuedMoves -AutoSize
