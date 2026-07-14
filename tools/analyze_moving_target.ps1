[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [ValidateSet('X', 'Y')][string]$Axis = 'X',
    [double]$TrialGapMs = 150.0,
    [double]$WarmupMs = 200.0,
    [ValidateRange(100.0, 10000.0)][double]$SteadyWindowMs = 1000.0,
    [ValidateRange(0.0, 60000.0)][double]$MinTrialDurationMs = 500.0,
    [ValidateRange(1, 10000)][int]$MinTrialSamples = 30,
    [double]$ReversalErrorThresholdPx = 10.0,
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
        [Parameter(Mandatory)][string]$ErrorColumn,
        [Parameter(Mandatory)][double]$ErrorThreshold,
        [Parameter(Mandatory)][int]$DirectionConfirmFrames,
        [Parameter(Mandatory)][double]$RecoveryRadius,
        [Parameter(Mandatory)][int]$RecoveryFrames
    )
    $errorSide = 0
    $candidateDirection = 0
    $candidateStart = -1
    $candidateCount = 0
    $lastConfirmedIndex = 0
    $transitions = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $Rows.Count; ++$index) {
        $error = [double]$Rows[$index].$ErrorColumn
        $sampleDirection = if ($error -ge $ErrorThreshold) { 1 } elseif ($error -le -$ErrorThreshold) { -1 } else { 0 }
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
        if ($errorSide -eq 0) {
            $errorSide = $candidateDirection
            $lastConfirmedIndex = $candidateStart
        }
        elseif ($candidateDirection -ne $errorSide) {
            # 目标反转后，闭环误差会从旧方向的稳态极值穿过中心并进入另一侧。
            # 从上一个确认方向到新方向起点寻找极值，作为反转响应开始的可重复近似。
            $extremeIndex = $lastConfirmedIndex
            for ($searchIndex = $lastConfirmedIndex; $searchIndex -le $candidateStart; ++$searchIndex) {
                $searchError = [double]$Rows[$searchIndex].$ErrorColumn
                $extremeError = [double]$Rows[$extremeIndex].$ErrorColumn
                if (($errorSide -gt 0 -and $searchError -gt $extremeError) -or
                    ($errorSide -lt 0 -and $searchError -lt $extremeError)) {
                    $extremeIndex = $searchIndex
                }
            }
            $transitions.Add([pscustomobject]@{
                StartIndex = $extremeIndex
                EndIndex = $candidateStart
            })
            $errorSide = $candidateDirection
            $lastConfirmedIndex = $candidateStart
        }
        else {
            $lastConfirmedIndex = $candidateStart
        }
        $candidateCount = 0
        # 确认一次方向后必须清空候选方向；否则下一帧会沿用旧方向并让
        # candidateStart=-1 参与索引，导致反转起点回跳到文件末尾。
        $candidateDirection = 0
        $candidateStart = -1
    }

    $recoveryTimes = [System.Collections.Generic.List[double]]::new()
    foreach ($transition in $transitions) {
        $stableCount = 0
        $stableStart = -1
        for ($index = $transition.StartIndex; $index -le $transition.EndIndex; ++$index) {
            if ([math]::Abs([double]$Rows[$index].$ErrorColumn) -le $RecoveryRadius) {
                if ($stableCount -eq 0) {
                    $stableStart = $index
                }
                ++$stableCount
                if ($stableCount -ge $RecoveryFrames) {
                    $recoveryTimes.Add([double]$Rows[$stableStart].Timestamp - [double]$Rows[$transition.StartIndex].Timestamp)
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
        ReversalCount = $transitions.Count
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
$finalMoveColumn = if ($Axis -eq 'X') { 'FinalMx' } else { 'FinalMy' }
$requestedPixelColumn = if ($Axis -eq 'X') { 'RequestedPixelX' } else { 'RequestedPixelY' }
$requestedCountsColumn = if ($Axis -eq 'X') { 'RequestedCountsX' } else { 'RequestedCountsY' }
$requiredColumns = @(
    'Timestamp', 'SourceWidth', 'SourceHeight', 'InferenceFPS', 'SourceReceiveFPS',
    'ObservationAgeSec', 'ErrorX', 'ErrorY', 'ErrorDistance', 'FilterResidual',
    'ObservedVelocityX', 'ObservedVelocityY', 'RequestedPixelX', 'RequestedPixelY',
    'RequestedCountsX', 'RequestedCountsY', 'FinalMx', 'FinalMy', 'SpeedLimited',
    'QueuedMoveCount'
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
    # 根目录下的 moving_summary.csv 是分析产物，不是原始试次；只接受
    # <backend>/<transport>/<scenario>.csv 三层数据，保证重复执行不会导入自身输出。
    if ($parts.Count -lt 3) {
        continue
    }
    if ($parts[0] -notin @('CUDA', 'DML') -or $parts[1] -notin @('ndi', 'udp')) {
        continue
    }
    $chain = "$($parts[0])+$($parts[1])"
    $rows = @(Import-Csv -LiteralPath $csvFile.FullName)
    if ($rows.Count -eq 0) {
        continue
    }
    foreach ($column in $requiredColumns) {
        if ($rows[0].PSObject.Properties.Name -notcontains $column) {
            throw "CSV is missing required moving-target column '$column': $($csvFile.FullName)"
        }
    }
    $hasBuildIdentity = $rows[0].PSObject.Properties.Name -contains 'BuildRevision' -and
        $rows[0].PSObject.Properties.Name -contains 'ControllerRevision'
    if (-not $hasBuildIdentity) {
        Write-Warning "Legacy moving CSV has no build/controller identity and cannot prove the tested executable: $($csvFile.FullName)"
    }
    $buildBackend = if ($hasBuildIdentity) { [string]$rows[0].BuildBackend } else { 'legacy' }
    $buildRevision = if ($hasBuildIdentity) { [string]$rows[0].BuildRevision } else { 'legacy' }
    $buildTimestampUtc = if ($hasBuildIdentity) { [string]$rows[0].BuildTimestampUtc } else { 'legacy' }
    $controllerRevision = if ($hasBuildIdentity) { [int]$rows[0].ControllerRevision } else { 0 }

    $trials = @(Split-MovingTrials -Rows $rows -GapMs $TrialGapMs)
    for ($trialIndex = 0; $trialIndex -lt $trials.Count; ++$trialIndex) {
        $trial = @($trials[$trialIndex])
        $firstTimestamp = [double]$trial[0].Timestamp
        $trialDurationMs = [double]$trial[-1].Timestamp - $firstTimestamp
        if ($trialDurationMs -lt $MinTrialDurationMs) {
            Write-Warning (
                "Skipping short moving trial: chain=$chain scenario=$($csvFile.BaseName) " +
                "trial=$($trialIndex + 1) duration_ms=$([math]::Round($trialDurationMs, 1)) " +
                "rows=$($trial.Count)")
            continue
        }
        $samples = @($trial | Where-Object { ([double]$_.Timestamp - $firstTimestamp) -ge $WarmupMs })
        if ($samples.Count -eq 0) {
            $samples = $trial
        }
        if ($samples.Count -lt $MinTrialSamples) {
            Write-Warning (
                "Skipping sparse moving trial: chain=$chain scenario=$($csvFile.BaseName) " +
                "trial=$($trialIndex + 1) effective_samples=$($samples.Count) " +
                "required_samples=$MinTrialSamples")
            continue
        }

        $axisAbsErrors = @($samples | ForEach-Object { [math]::Abs([double]$_.$errorColumn) })
        $steadyWindowStart = [double]$samples[-1].Timestamp - $SteadyWindowMs
        $steadySamples = @($samples | Where-Object { [double]$_.Timestamp -ge $steadyWindowStart })
        $steadyAxisAbsErrors = @($steadySamples | ForEach-Object { [math]::Abs([double]$_.$errorColumn) })
        $distanceErrors = @($samples | ForEach-Object { [double]$_.ErrorDistance })
        $axisSpeeds = @($samples | ForEach-Object { [math]::Abs([double]$_.$velocityColumn) })
        $residuals = @($samples | ForEach-Object { [double]$_.FilterResidual })
        $limitedRows = @($samples | Where-Object { [int]$_.SpeedLimited -eq 1 }).Count
        $hasOutsidePrediction =
            $rows[0].PSObject.Properties.Name -contains 'PredictionOutsideApplied' -and
            $rows[0].PSObject.Properties.Name -contains 'PredictionOffsetX' -and
            $rows[0].PSObject.Properties.Name -contains 'TargetBoxWidth'
        $outsideRows = if ($hasOutsidePrediction) {
            @($samples | Where-Object { [int]$_.PredictionOutsideApplied -eq 1 }).Count
        }
        else { 0 }
        $outsideLeadDistances = if ($hasOutsidePrediction) {
            @($samples | Where-Object { [int]$_.PredictionOutsideApplied -eq 1 } | ForEach-Object {
                if ($Axis -eq 'X') {
                    $offset = [double]$_.PredictionOffsetX
                    $viewMotion = [double]$_.ViewMotionX
                    if ($offset -gt 0.0) {
                        [math]::Max(0.0, [double]$_.PredictedX -
                            ([double]$_.TargetBoxX - $viewMotion + [double]$_.TargetBoxWidth))
                    }
                    elseif ($offset -lt 0.0) {
                        [math]::Max(0.0, [double]$_.TargetBoxX - $viewMotion - [double]$_.PredictedX)
                    }
                }
                else {
                    $offset = [double]$_.PredictionOffsetY
                    $viewMotion = [double]$_.ViewMotionY
                    if ($offset -gt 0.0) {
                        [math]::Max(0.0, [double]$_.PredictedY -
                            ([double]$_.TargetBoxY - $viewMotion + [double]$_.TargetBoxHeight))
                    }
                    elseif ($offset -lt 0.0) {
                        [math]::Max(0.0, [double]$_.TargetBoxY - $viewMotion - [double]$_.PredictedY)
                    }
                }
            } | Where-Object { $null -ne $_ })
        }
        else { @() }
        $outsideLeadP50 = if (@($outsideLeadDistances).Count -gt 0) {
            Get-PercentileValue -Values @($outsideLeadDistances) -Percentile 0.50
        }
        else { 0.0 }
        $outsideLeadP95 = if (@($outsideLeadDistances).Count -gt 0) {
            Get-PercentileValue -Values @($outsideLeadDistances) -Percentile 0.95
        }
        else { 0.0 }
        $predictionSideFlipCount = 0
        $lastPredictionSide = 0
        if ($hasOutsidePrediction) {
            foreach ($sample in $samples) {
                if ([int]$sample.PredictionOutsideApplied -ne 1) {
                    continue
                }
                $offset = if ($Axis -eq 'X') {
                    [double]$sample.PredictionOffsetX
                }
                else {
                    [double]$sample.PredictionOffsetY
                }
                $side = if ($offset -gt 0.0) { 1 } elseif ($offset -lt 0.0) { -1 } else { 0 }
                if ($side -ne 0 -and $lastPredictionSide -ne 0 -and $side -ne $lastPredictionSide) {
                    ++$predictionSideFlipCount
                }
                if ($side -ne 0) {
                    $lastPredictionSide = $side
                }
            }
        }
        $movingInsideSettleRows = if ($rows[0].PSObject.Properties.Name -contains 'MovingInsideSettle') {
            @($samples | Where-Object { [int]$_.MovingInsideSettle -eq 1 }).Count
        }
        else { 0 }
        $sampleDurationSeconds = (
            [double]$samples[-1].Timestamp - [double]$samples[0].Timestamp) / 1000.0
        $signedOutputCounts = [double]($samples | Measure-Object $finalMoveColumn -Sum).Sum
        $absoluteOutputCounts = [double](($samples | ForEach-Object {
            [math]::Abs([double]$_.$finalMoveColumn)
        } | Measure-Object -Sum).Sum)
        $countsPerPixelSamples = @($samples | Where-Object {
            [math]::Abs([double]$_.$requestedPixelColumn) -gt 0.000001
        } | ForEach-Object {
            [math]::Abs([double]$_.$requestedCountsColumn / [double]$_.$requestedPixelColumn)
        })
        $estimatedCountsPerPixel = Get-PercentileValue -Values $countsPerPixelSamples -Percentile 0.50
        $signedOutputCps = $signedOutputCounts / [math]::Max(0.001, $sampleDurationSeconds)
        $absoluteOutputCps = $absoluteOutputCounts / [math]::Max(0.001, $sampleDurationSeconds)
        # 匀速单向场景中，平均误差乘 counts/px 后除以实际输出 counts/s，
        # 可得到闭环滞后的近似时间；该值用于辨别响应时间而非速度上限瓶颈。
        $approxClosedLoopLagMs = 1000.0 * [double]($axisAbsErrors | Measure-Object -Average).Average *
            $estimatedCountsPerPixel / [math]::Max(1.0, $absoluteOutputCps)
        # 单向文件的起停、目标切换或短暂过冲不属于测试脚本反转。只有文件名明确标记
        # reverse/reversal 的往返场景才计算恢复指标，避免把单向质量数据误报为反转。
        $isReversalScenario = $csvFile.BaseName -match '(?i)reverse|reversal'
        $reversals = if ($isReversalScenario) {
            Get-ReversalMetrics -Rows $samples -ErrorColumn $errorColumn -ErrorThreshold $ReversalErrorThresholdPx -DirectionConfirmFrames $ReversalConfirmFrames -RecoveryRadius $RecoveryRadiusPx -RecoveryFrames $RecoveryConfirmFrames
        }
        else {
            [pscustomobject]@{
                ReversalCount = 0
                RecoveredReversals = 0
                RecoveryMeanMs = $null
                RecoveryP95Ms = $null
            }
        }

        $trialMetrics.Add([pscustomobject]@{
            Level = 'Trial'
            Chain = $chain
            Scenario = $csvFile.BaseName
            Trial = $trialIndex + 1
            Rows = $trial.Count
            Samples = $samples.Count
            DurationMs = [math]::Round($trialDurationMs, 1)
            StartAbsAxisErrorPx = [math]::Round([math]::Abs([double]$trial[0].$errorColumn), 2)
            MeanAxisErrorPx = [math]::Round([double]($samples | Measure-Object $errorColumn -Average).Average, 2)
            MeanAbsAxisErrorPx = [math]::Round([double]($axisAbsErrors | Measure-Object -Average).Average, 2)
            P50AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.50), 2)
            P95AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.95), 2)
            P99AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $axisAbsErrors -Percentile 0.99), 2)
            SteadySamples = $steadySamples.Count
            SteadyMeanAbsAxisErrorPx = [math]::Round([double]($steadyAxisAbsErrors | Measure-Object -Average).Average, 2)
            SteadyP95AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $steadyAxisAbsErrors -Percentile 0.95), 2)
            P95ErrorDistancePx = [math]::Round((Get-PercentileValue -Values $distanceErrors -Percentile 0.95), 2)
            MaxErrorDistancePx = [math]::Round([double]($distanceErrors | Measure-Object -Maximum).Maximum, 2)
            P50ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.50), 1)
            P95ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.95), 1)
            P95FilterResidualPx = [math]::Round((Get-PercentileValue -Values $residuals -Percentile 0.95), 2)
            SignedOutputCountsPerSecond = [math]::Round($signedOutputCps, 1)
            MeanAbsOutputCountsPerSecond = [math]::Round($absoluteOutputCps, 1)
            EstimatedCountsPerPixel = [math]::Round($estimatedCountsPerPixel, 4)
            ApproxClosedLoopLagMs = [math]::Round($approxClosedLoopLagMs, 1)
            SpeedLimitedPct = [math]::Round(100.0 * $limitedRows / [math]::Max(1, $samples.Count), 1)
            PredictionOutsideAppliedPct = [math]::Round(
                100.0 * $outsideRows / [math]::Max(1, $samples.Count), 1)
            P50OutsideLeadPx = [math]::Round($outsideLeadP50, 2)
            P95OutsideLeadPx = [math]::Round($outsideLeadP95, 2)
            PredictionSideFlipCount = $predictionSideFlipCount
            MovingInsideSettlePct = [math]::Round(100.0 * $movingInsideSettleRows / [math]::Max(1, $samples.Count), 1)
            InferenceFps = [math]::Round([double]($samples | Measure-Object InferenceFPS -Average).Average, 1)
            SourceReceiveFps = [math]::Round([double]($samples | Measure-Object SourceReceiveFPS -Average).Average, 1)
            ObservationAgeAvgMs = [math]::Round(1000.0 * [double]($samples | Measure-Object ObservationAgeSec -Average).Average, 1)
            ObservationAgeP95Ms = [math]::Round(1000.0 * (Get-PercentileValue -Values @($samples | ForEach-Object { [double]$_.ObservationAgeSec }) -Percentile 0.95), 1)
            MaxQueuedMoves = [int]($samples | Measure-Object QueuedMoveCount -Maximum).Maximum
            ReversalCount = $reversals.ReversalCount
            RecoveredReversals = $reversals.RecoveredReversals
            RecoveryMeanMs = $reversals.RecoveryMeanMs
            RecoveryP95Ms = $reversals.RecoveryP95Ms
            ReversalRateHz = [math]::Round($reversals.ReversalCount / [math]::Max(0.001, $sampleDurationSeconds), 2)
            SourceGeometry = "$($trial[0].SourceWidth)x$($trial[0].SourceHeight)"
            BuildBackend = $buildBackend
            BuildRevision = $buildRevision
            BuildTimestampUtc = $buildTimestampUtc
            ControllerRevision = $controllerRevision
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
        MeanStartAbsAxisErrorPx = [math]::Round([double]($group | Measure-Object StartAbsAxisErrorPx -Average).Average, 2)
        MinStartAbsAxisErrorPx = [math]::Round([double]($group | Measure-Object StartAbsAxisErrorPx -Minimum).Minimum, 2)
        MaxStartAbsAxisErrorPx = [math]::Round([double]($group | Measure-Object StartAbsAxisErrorPx -Maximum).Maximum, 2)
        MeanP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object P95AbsAxisErrorPx -Average).Average, 2)
        WorstP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object P95AbsAxisErrorPx -Maximum).Maximum, 2)
        MeanSteadyMeanAbsAxisErrorPx = [math]::Round([double]($group | Measure-Object SteadyMeanAbsAxisErrorPx -Average).Average, 2)
        MeanSteadyP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object SteadyP95AbsAxisErrorPx -Average).Average, 2)
        MeanP95ErrorDistancePx = [math]::Round([double]($group | Measure-Object P95ErrorDistancePx -Average).Average, 2)
        MeanAbsOutputCountsPerSecond = [math]::Round([double]($group | Measure-Object MeanAbsOutputCountsPerSecond -Average).Average, 1)
        MeanApproxClosedLoopLagMs = [math]::Round([double]($group | Measure-Object ApproxClosedLoopLagMs -Average).Average, 1)
        SpeedLimitedPct = [math]::Round([double](($group | ForEach-Object { $_.SpeedLimitedPct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionOutsideAppliedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionOutsideAppliedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        MeanP50OutsideLeadPx = [math]::Round(
            [double]($group | Measure-Object P50OutsideLeadPx -Average).Average, 2)
        MeanP95OutsideLeadPx = [math]::Round(
            [double]($group | Measure-Object P95OutsideLeadPx -Average).Average, 2)
        PredictionSideFlipCount = [int]($group | Measure-Object PredictionSideFlipCount -Sum).Sum
        MovingInsideSettlePct = [math]::Round([double](($group | ForEach-Object { $_.MovingInsideSettlePct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        ReversalCount = [int]($group | Measure-Object ReversalCount -Sum).Sum
        ReversalRateHz = [math]::Round(
            [double]($group | Measure-Object ReversalCount -Sum).Sum /
            [math]::Max(0.001, [double]($group | Measure-Object DurationMs -Sum).Sum / 1000.0), 2)
        RecoveredReversals = $recoveredCount
        RecoveryMeanMs = if ($recoveredCount -eq 0) { $null } else {
            [math]::Round($weightedRecoveryTotal / $recoveredCount, 1)
        }
        RecoveryP95Ms = if ($recoveredCount -eq 0) { $null } else {
            # 多试次场景取各试次恢复P95的最大值，避免把高频反转的最差试次平均掉。
            [math]::Round([double](($group | Measure-Object RecoveryP95Ms -Maximum).Maximum), 1)
        }
        MaxQueuedMoves = [int]($group | Measure-Object MaxQueuedMoves -Maximum).Maximum
        SourceGeometry = $group[0].SourceGeometry
        BuildBackend = (@($group.BuildBackend | Select-Object -Unique) -join ';')
        BuildRevision = (@($group.BuildRevision | Select-Object -Unique) -join ';')
        BuildTimestampUtc = (@($group.BuildTimestampUtc | Select-Object -Unique) -join ';')
        ControllerRevision = (@($group.ControllerRevision | Select-Object -Unique) -join ';')
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
        'StartAbsAxisErrorPx', 'MeanStartAbsAxisErrorPx', 'MinStartAbsAxisErrorPx', 'MaxStartAbsAxisErrorPx',
        'MeanAxisErrorPx', 'MeanAbsAxisErrorPx', 'P50AbsAxisErrorPx', 'P95AbsAxisErrorPx', 'P99AbsAxisErrorPx',
        'SteadySamples', 'SteadyMeanAbsAxisErrorPx', 'SteadyP95AbsAxisErrorPx',
        'MeanP95AbsAxisErrorPx', 'WorstP95AbsAxisErrorPx', 'P95ErrorDistancePx',
        'MeanSteadyMeanAbsAxisErrorPx', 'MeanSteadyP95AbsAxisErrorPx',
        'MeanP95ErrorDistancePx', 'MaxErrorDistancePx', 'P50ObservedAxisSpeed',
        'P95ObservedAxisSpeed', 'P95FilterResidualPx', 'SignedOutputCountsPerSecond',
        'MeanAbsOutputCountsPerSecond', 'EstimatedCountsPerPixel', 'ApproxClosedLoopLagMs',
        'MeanApproxClosedLoopLagMs', 'SpeedLimitedPct', 'PredictionOutsideAppliedPct',
        'P50OutsideLeadPx', 'P95OutsideLeadPx', 'MeanP50OutsideLeadPx', 'MeanP95OutsideLeadPx',
        'PredictionSideFlipCount',
        'MovingInsideSettlePct', 'InferenceFps',
        'SourceReceiveFps', 'ObservationAgeAvgMs', 'ObservationAgeP95Ms', 'MaxQueuedMoves',
        'ReversalCount', 'ReversalRateHz', 'RecoveredReversals', 'RecoveryMeanMs', 'RecoveryP95Ms',
        'SourceGeometry', 'BuildBackend', 'BuildRevision', 'BuildTimestampUtc', 'ControllerRevision'
    )
    $allMetrics | Select-Object -Property $exportColumns |
        Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
}

if ($PassThru) {
    $allMetrics
    return
}

Write-Host '[moving-target] Trial metrics' -ForegroundColor Cyan
$trialMetrics | Format-Table Chain, Scenario, Trial, DurationMs, P95AbsAxisErrorPx, PredictionOutsideAppliedPct, P50OutsideLeadPx, P95OutsideLeadPx, PredictionSideFlipCount, ReversalCount, SpeedLimitedPct -AutoSize
Write-Host '[moving-target] Scenario summary' -ForegroundColor Cyan
$scenarioMetrics | Format-Table Chain, Scenario, Trials, MeanP95AbsAxisErrorPx, PredictionOutsideAppliedPct, MeanP50OutsideLeadPx, MeanP95OutsideLeadPx, PredictionSideFlipCount, ReversalCount, SpeedLimitedPct -AutoSize
