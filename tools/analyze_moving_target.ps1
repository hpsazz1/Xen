[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [ValidateSet('X', 'Y')][string]$Axis = 'X',
    [double]$TrialGapMs = 150.0,
    [double]$WarmupMs = 200.0,
    [double]$ReversalVelocityThreshold = 60.0,
    [ValidateRange(1, 20)][int]$ReversalConfirmFrames = 3,
    [double]$RecoveryRadiusPx = 8.0,
    [ValidateRange(1, 20)][int]$RecoveryConfirmFrames = 3,
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
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $index = [math]::Floor($Percentile * ($sorted.Count - 1))
    return [double]$sorted[$index]
}

function Split-MovingTrials {
    param(
        [Parameter(Mandatory)][object[]]$Rows,
        [Parameter(Mandatory)][double]$GapMs
    )
    $trials = [System.Collections.Generic.List[object]]::new()
    $current = [System.Collections.Generic.List[object]]::new()
    $previousTimestamp = $null
    foreach ($row in $Rows) {
        $timestamp = [double]$row.Timestamp
        # 每轮采集之间主动松开瞄准并保留空档；按时间切分可兼容不同推理帧率。
        if ($null -ne $previousTimestamp -and ($timestamp - [double]$previousTimestamp) -gt $GapMs) {
            if ($current.Count -gt 0) {
                $trials.Add(@($current))
                $current = [System.Collections.Generic.List[object]]::new()
            }
        }
        $current.Add($row)
        $previousTimestamp = $timestamp
    }
    if ($current.Count -gt 0) {
        $trials.Add(@($current))
    }
    return @($trials)
}

function Get-ReversalMetrics {
    param(
        [Parameter(Mandatory)][object[]]$Rows,
        [Parameter(Mandatory)][string]$VelocityColumn,
        [Parameter(Mandatory)][string]$ErrorColumn,
        [Parameter(Mandatory)][double]$VelocityThreshold,
        [Parameter(Mandatory)][int]$DirectionConfirmFrames,
        [Parameter(Mandatory)][double]$RecoveryRadius,
        [Parameter(Mandatory)][int]$RecoveryFrames
    )
    $direction = 0
    $candidateDirection = 0
    $candidateStart = -1
    $candidateCount = 0
    $reversalStarts = [System.Collections.Generic.List[int]]::new()
    for ($index = 0; $index -lt $Rows.Count; ++$index) {
        $velocity = [double]$Rows[$index].$VelocityColumn
        $sampleDirection = if ($velocity -ge $VelocityThreshold) { 1 } elseif ($velocity -le -$VelocityThreshold) { -1 } else { 0 }
        if ($sampleDirection -eq 0) {
            $candidateDirection = 0
            $candidateStart = -1
            $candidateCount = 0
            continue
        }
        if ($sampleDirection -ne $candidateDirection) {
            $candidateDirection = $sampleDirection
            $candidateStart = $index
            $candidateCount = 1
        }
        else {
            ++$candidateCount
        }
        if ($candidateCount -lt $DirectionConfirmFrames) {
            continue
        }
        if ($direction -eq 0) {
            $direction = $candidateDirection
        }
        elseif ($candidateDirection -ne $direction) {
            $reversalStarts.Add($candidateStart)
            $direction = $candidateDirection
        }
        $candidateCount = 0
        $candidateStart = -1
    }

    $recoveryTimes = [System.Collections.Generic.List[double]]::new()
    foreach ($reversalStart in $reversalStarts) {
        $stableCount = 0
        $stableStart = -1
        for ($index = $reversalStart; $index -lt $Rows.Count; ++$index) {
            if ([math]::Abs([double]$Rows[$index].$ErrorColumn) -le $RecoveryRadius) {
                if ($stableCount -eq 0) {
                    $stableStart = $index
                }
                ++$stableCount
                if ($stableCount -ge $RecoveryFrames) {
                    $recoveryTimes.Add([double]$Rows[$stableStart].Timestamp - [double]$Rows[$reversalStart].Timestamp)
                    break
                }
            }
            else {
                $stableCount = 0
                $stableStart = -1
            }
        }
    }

    return [pscustomobject]@{
        ReversalCount = $reversalStarts.Count
        RecoveredReversals = $recoveryTimes.Count
        RecoveryMeanMs = if ($recoveryTimes.Count -eq 0) { $null } else {
            [math]::Round([double]($recoveryTimes | Measure-Object -Average).Average, 1)
        }
        RecoveryP95Ms = if ($recoveryTimes.Count -eq 0) { $null } else {
            [math]::Round((Get-PercentileValue -Values @($recoveryTimes) -Percentile 0.95), 1)
        }
    }
}

$resolvedRoot = [System.IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "Moving-target data root does not exist: $resolvedRoot"
}
$velocityColumn = if ($Axis -eq 'X') { 'ObservedVelocityX' } else { 'ObservedVelocityY' }
$errorColumn = if ($Axis -eq 'X') { 'ErrorX' } else { 'ErrorY' }
$requiredColumns = @(
    'Timestamp', 'SourceWidth', 'SourceHeight', 'InferenceFPS', 'SourceReceiveFPS',
    'ObservationAgeSec', 'ErrorX', 'ErrorY', 'ErrorDistance', 'FilterResidual',
    'ObservedVelocityX', 'ObservedVelocityY', 'SpeedLimited', 'QueuedMoveCount'
)
$trialMetrics = [System.Collections.Generic.List[object]]::new()
$rootPrefix = $resolvedRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar

foreach ($csvFile in @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Filter '*.csv' | Sort-Object FullName)) {
    if (-not $csvFile.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "CSV escaped the requested data root: $($csvFile.FullName)"
    }
    $relativePath = $csvFile.FullName.Substring($rootPrefix.Length)
    $parts = $relativePath -split '[\\/]'
    $chain = if ($parts.Count -ge 3) { "$($parts[0])+$($parts[1])" } else { $csvFile.Directory.Name }
    $rows = @(Import-Csv -LiteralPath $csvFile.FullName)
    if ($rows.Count -eq 0) {
        continue
    }
    foreach ($column in $requiredColumns) {
        if ($rows[0].PSObject.Properties.Name -notcontains $column) {
            throw "CSV is missing required moving-target column '$column': $($csvFile.FullName)"
        }
    }

    $trials = @(Split-MovingTrials -Rows $rows -GapMs $TrialGapMs)
    for ($trialIndex = 0; $trialIndex -lt $trials.Count; ++$trialIndex) {
        $trial = @($trials[$trialIndex])
        $firstTimestamp = [double]$trial[0].Timestamp
        $samples = @($trial | Where-Object { ([double]$_.Timestamp - $firstTimestamp) -ge $WarmupMs })
        if ($samples.Count -eq 0) {
            $samples = $trial
        }

        $axisAbsErrors = @($samples | ForEach-Object { [math]::Abs([double]$_.$errorColumn) })
        $distanceErrors = @($samples | ForEach-Object { [double]$_.ErrorDistance })
        $axisSpeeds = @($samples | ForEach-Object { [math]::Abs([double]$_.$velocityColumn) })
        $residuals = @($samples | ForEach-Object { [double]$_.FilterResidual })
        $limitedRows = @($samples | Where-Object { [int]$_.SpeedLimited -eq 1 }).Count
        $reversals = Get-ReversalMetrics -Rows $samples -VelocityColumn $velocityColumn -ErrorColumn $errorColumn -VelocityThreshold $ReversalVelocityThreshold -DirectionConfirmFrames $ReversalConfirmFrames -RecoveryRadius $RecoveryRadiusPx -RecoveryFrames $RecoveryConfirmFrames

        $trialMetrics.Add([pscustomobject]@{
            Level = 'Trial'
            Chain = $chain
            Scenario = $csvFile.BaseName
            Trial = $trialIndex + 1
            Rows = $trial.Count
            Samples = $samples.Count
            DurationMs = [math]::Round([double]$trial[-1].Timestamp - $firstTimestamp, 1)
            MeanAbsAxisErrorPx = [math]::Round([double]($axisAbsErrors | Measure-Object -Average).Average, 2)
            P50AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.50), 2)
            P95AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.95), 2)
            P99AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.99), 2)
            P95ErrorDistancePx = [math]::Round((Get-PercentileValue -Values $distanceErrors -Percentile 0.95), 2)
            MaxErrorDistancePx = [math]::Round([double]($distanceErrors | Measure-Object -Maximum).Maximum, 2)
            P50ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.50), 1)
            P95ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.95), 1)
            P95FilterResidualPx = [math]::Round((Get-PercentileValue -Values $residuals -Percentile 0.95), 2)
            SpeedLimitedPct = [math]::Round(100.0 * $limitedRows / [math]::Max(1, $samples.Count), 1)
            InferenceFps = [math]::Round([double]($samples | Measure-Object InferenceFPS -Average).Average, 1)
            SourceReceiveFps = [math]::Round([double]($samples | Measure-Object SourceReceiveFPS -Average).Average, 1)
            ObservationAgeAvgMs = [math]::Round(1000.0 * [double]($samples | Measure-Object ObservationAgeSec -Average).Average, 1)
            ObservationAgeP95Ms = [math]::Round(1000.0 * (Get-PercentileValue -Values @($samples | ForEach-Object { [double]$_.ObservationAgeSec }) -Percentile 0.95), 1)
            MaxQueuedMoves = [int]($samples | Measure-Object QueuedMoveCount -Maximum).Maximum
            ReversalCount = $reversals.ReversalCount
            RecoveredReversals = $reversals.RecoveredReversals
            RecoveryMeanMs = $reversals.RecoveryMeanMs
            RecoveryP95Ms = $reversals.RecoveryP95Ms
            SourceGeometry = "$($trial[0].SourceWidth)x$($trial[0].SourceHeight)"
        })
    }
}

if ($trialMetrics.Count -eq 0) {
    throw "No moving-target CSV trials were found under: $resolvedRoot"
}

$scenarioMetrics = @($trialMetrics | Group-Object Chain, Scenario | ForEach-Object {
    $group = @($_.Group)
    $sampleCount = [int]($group | Measure-Object Samples -Sum).Sum
    $recoveredCount = [int]($group | Measure-Object RecoveredReversals -Sum).Sum
    $weightedRecoveryTotal = [double](($group | Where-Object {
        $_.RecoveredReversals -gt 0 -and $null -ne $_.RecoveryMeanMs
    } | ForEach-Object {
        [double]$_.RecoveryMeanMs * [int]$_.RecoveredReversals
    } | Measure-Object -Sum).Sum)
    [pscustomobject]@{
        Level = 'Scenario'
        Chain = $group[0].Chain
        Scenario = $group[0].Scenario
        Trials = $group.Count
        Samples = $sampleCount
        MeanP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object P95AbsAxisErrorPx -Average).Average, 2)
        WorstP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object P95AbsAxisErrorPx -Maximum).Maximum, 2)
        MeanP95ErrorDistancePx = [math]::Round([double]($group | Measure-Object P95ErrorDistancePx -Average).Average, 2)
        SpeedLimitedPct = [math]::Round([double](($group | ForEach-Object { $_.SpeedLimitedPct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        ReversalCount = [int]($group | Measure-Object ReversalCount -Sum).Sum
        RecoveredReversals = $recoveredCount
        RecoveryMeanMs = if ($recoveredCount -eq 0) { $null } else {
            [math]::Round($weightedRecoveryTotal / $recoveredCount, 1)
        }
        MaxQueuedMoves = [int]($group | Measure-Object MaxQueuedMoves -Maximum).Maximum
        SourceGeometry = $group[0].SourceGeometry
    }
})

$allMetrics = @($trialMetrics) + $scenarioMetrics
if (-not [string]::IsNullOrWhiteSpace($OutputCsv)) {
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputCsv)
    $outputDirectory = Split-Path -Parent $resolvedOutput
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and -not (Test-Path -LiteralPath $outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory | Out-Null
    }
    # 试次与场景汇总字段不同，显式使用联合列，确保机器可读导出不会丢失汇总指标。
    $exportColumns = @(
        'Level', 'Chain', 'Scenario', 'Trial', 'Trials', 'Rows', 'Samples', 'DurationMs',
        'MeanAbsAxisErrorPx', 'P50AbsAxisErrorPx', 'P95AbsAxisErrorPx', 'P99AbsAxisErrorPx',
        'MeanP95AbsAxisErrorPx', 'WorstP95AbsAxisErrorPx', 'P95ErrorDistancePx',
        'MeanP95ErrorDistancePx', 'MaxErrorDistancePx', 'P50ObservedAxisSpeed',
        'P95ObservedAxisSpeed', 'P95FilterResidualPx', 'SpeedLimitedPct', 'InferenceFps',
        'SourceReceiveFps', 'ObservationAgeAvgMs', 'ObservationAgeP95Ms', 'MaxQueuedMoves',
        'ReversalCount', 'RecoveredReversals', 'RecoveryMeanMs', 'RecoveryP95Ms',
        'SourceGeometry'
    )
    $allMetrics | Select-Object -Property $exportColumns |
        Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
}

if ($PassThru) {
    $allMetrics
    return
}

Write-Host '[moving-target] Trial metrics' -ForegroundColor Cyan
$trialMetrics | Format-Table Chain, Scenario, Trial, DurationMs, P95AbsAxisErrorPx, P95ErrorDistancePx, SpeedLimitedPct, ReversalCount, RecoveryMeanMs, MaxQueuedMoves -AutoSize
Write-Host '[moving-target] Scenario summary' -ForegroundColor Cyan
$scenarioMetrics | Format-Table Chain, Scenario, Trials, MeanP95AbsAxisErrorPx, WorstP95AbsAxisErrorPx, SpeedLimitedPct, ReversalCount, RecoveredReversals, RecoveryMeanMs, MaxQueuedMoves -AutoSize
