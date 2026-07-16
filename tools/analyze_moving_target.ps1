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
$rawPivotColumn = if ($Axis -eq 'X') { 'RawPivotX' } else { 'RawPivotY' }
$predictedColumn = if ($Axis -eq 'X') { 'PredictedX' } else { 'PredictedY' }
$finalMoveColumn = if ($Axis -eq 'X') { 'FinalMx' } else { 'FinalMy' }
$requestedPixelColumn = if ($Axis -eq 'X') { 'RequestedPixelX' } else { 'RequestedPixelY' }
$requestedCountsColumn = if ($Axis -eq 'X') { 'RequestedCountsX' } else { 'RequestedCountsY' }
$integralCountsColumn = if ($Axis -eq 'X') { 'IntegralCountsX' } else { 'IntegralCountsY' }
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
        # ErrorX/ErrorY 是“预测后控制目标 - 准星中心”，包含有意加入的常速度前瞻，
        # 不能直接代表准星相对真实检测目标的屏幕误差。新CSV用
        # Predicted-Error 反推出每帧准星中心，再与原始pivot比较；旧CSV继续可分析，
        # 但通过 ObservedTrackingAvailable=0 明确标记缺少真实跟踪口径。
        $hasObservedTracking =
            $rows[0].PSObject.Properties.Name -contains $rawPivotColumn -and
            $rows[0].PSObject.Properties.Name -contains $predictedColumn
        $observedAxisErrors = if ($hasObservedTracking) {
            @($samples | ForEach-Object {
                $aimCenter = [double]$_.$predictedColumn - [double]$_.$errorColumn
                [double]$_.$rawPivotColumn - $aimCenter
            })
        }
        else { @() }
        $observedAxisAbsErrors = @($observedAxisErrors | ForEach-Object { [math]::Abs($_) })
        $hasObservedBoxTracking = $hasObservedTracking -and
            $rows[0].PSObject.Properties.Name -contains 'RawPivotX' -and
            $rows[0].PSObject.Properties.Name -contains 'RawPivotY' -and
            $rows[0].PSObject.Properties.Name -contains 'PredictedX' -and
            $rows[0].PSObject.Properties.Name -contains 'PredictedY' -and
            $rows[0].PSObject.Properties.Name -contains 'TargetBoxX' -and
            $rows[0].PSObject.Properties.Name -contains 'TargetBoxY' -and
            $rows[0].PSObject.Properties.Name -contains 'TargetBoxWidth' -and
            $rows[0].PSObject.Properties.Name -contains 'TargetBoxHeight'
        $observedDistances = if ($hasObservedBoxTracking) {
            @($samples | ForEach-Object {
                $aimCenterX = [double]$_.PredictedX - [double]$_.ErrorX
                $aimCenterY = [double]$_.PredictedY - [double]$_.ErrorY
                $observedErrorX = [double]$_.RawPivotX - $aimCenterX
                $observedErrorY = [double]$_.RawPivotY - $aimCenterY
                [math]::Sqrt($observedErrorX * $observedErrorX +
                    $observedErrorY * $observedErrorY)
            })
        }
        else { @() }
        $insideTargetRows = if ($hasObservedBoxTracking) {
            @($samples | Where-Object {
                $aimCenterX = [double]$_.PredictedX - [double]$_.ErrorX
                $aimCenterY = [double]$_.PredictedY - [double]$_.ErrorY
                $aimCenterX -ge [double]$_.TargetBoxX -and
                $aimCenterX -le [double]$_.TargetBoxX + [double]$_.TargetBoxWidth -and
                $aimCenterY -ge [double]$_.TargetBoxY -and
                $aimCenterY -le [double]$_.TargetBoxY + [double]$_.TargetBoxHeight
            }).Count
        }
        else { 0 }
        $steadyWindowStart = [double]$samples[-1].Timestamp - $SteadyWindowMs
        $steadySamples = @($samples | Where-Object { [double]$_.Timestamp -ge $steadyWindowStart })
        $steadyAxisAbsErrors = @($steadySamples | ForEach-Object { [math]::Abs([double]$_.$errorColumn) })
        $observedSteadyAxisAbsErrors = if ($hasObservedTracking) {
            @($steadySamples | ForEach-Object {
                $aimCenter = [double]$_.$predictedColumn - [double]$_.$errorColumn
                [math]::Abs([double]$_.$rawPivotColumn - $aimCenter)
            })
        }
        else { @() }
        $distanceErrors = @($samples | ForEach-Object { [double]$_.ErrorDistance })
        $axisSpeeds = @($samples | ForEach-Object { [math]::Abs([double]$_.$velocityColumn) })
        $residuals = @($samples | ForEach-Object { [double]$_.FilterResidual })
        $limitedRows = @($samples | Where-Object { [int]$_.SpeedLimited -eq 1 }).Count
        $hasKinematicPrediction =
            $rows[0].PSObject.Properties.Name -contains 'PredictionDirectionLocked' -and
            $rows[0].PSObject.Properties.Name -contains 'PredictionOffsetX'
        $predictionRows = if ($hasKinematicPrediction) {
            @($samples | Where-Object {
                [int]$_.PredictionDirectionLocked -eq 1 -and
                [math]::Abs([double]$(if ($Axis -eq 'X') { $_.PredictionOffsetX } else { $_.PredictionOffsetY })) -gt 0.001
            })
        }
        else { @() }
        $predictionRows = @($predictionRows)
        $selfMotionSuppressedRows = if (
            $rows[0].PSObject.Properties.Name -contains 'PredictionSelfMotionSuppressed') {
            @($samples | Where-Object { [int]$_.PredictionSelfMotionSuppressed -eq 1 }).Count
        }
        else { 0 }
        $oscillationSuppressedRows = if (
            $rows[0].PSObject.Properties.Name -contains 'PredictionOscillationSuppressed') {
            @($samples | Where-Object { [int]$_.PredictionOscillationSuppressed -eq 1 }).Count
        }
        else { 0 }
        $highSpeedSuppressedRows = if (
            $rows[0].PSObject.Properties.Name -contains 'PredictionHighSpeedSuppressed') {
            @($samples | Where-Object { [int]$_.PredictionHighSpeedSuppressed -eq 1 }).Count
        }
        else { 0 }
        $stationarySuppressedRows = if (
            $rows[0].PSObject.Properties.Name -contains 'PredictionStationarySuppressed') {
            @($samples | Where-Object { [int]$_.PredictionStationarySuppressed -eq 1 }).Count
        }
        else { 0 }
        $motionEvidenceSuppressedRows = if (
            $rows[0].PSObject.Properties.Name -contains 'PredictionMotionEvidenceSuppressed') {
            @($samples | Where-Object { [int]$_.PredictionMotionEvidenceSuppressed -eq 1 }).Count
        }
        else { 0 }
        $predictionLeadDistances = @($predictionRows | ForEach-Object {
            if ($Axis -eq 'X') { [math]::Abs([double]$_.PredictionOffsetX) }
            else { [math]::Abs([double]$_.PredictionOffsetY) }
        })
        $predictionLeadP50 = if ($predictionLeadDistances.Count -gt 0) {
            Get-PercentileValue -Values $predictionLeadDistances -Percentile 0.50
        }
        else { 0.0 }
        $predictionLeadP95 = if ($predictionLeadDistances.Count -gt 0) {
            Get-PercentileValue -Values $predictionLeadDistances -Percentile 0.95
        }
        else { 0.0 }
        $predictionSpan = if (
            $rows[0].PSObject.Properties.Name -contains 'CaptureRoiWidth' -and
            [double]$samples[0].CaptureRoiWidth -gt 0.0) {
            [double]$samples[0].CaptureRoiWidth
        }
        elseif (
            $rows[0].PSObject.Properties.Name -contains 'DmlInputWidth' -and
            [double]$samples[0].DmlInputWidth -gt 0.0) {
            [double]$samples[0].DmlInputWidth
        }
        else { 0.0 }
        $predictionDistanceCap = if ($predictionSpan -gt 0.0) {
            [math]::Max(12.0, $predictionSpan * 0.075)
        }
        else { 0.0 }
        $predictionCappedRows = if ($predictionDistanceCap -gt 0.0) {
            @($predictionRows | Where-Object {
                $offset = if ($Axis -eq 'X') {
                    [math]::Abs([double]$_.PredictionOffsetX)
                }
                else { [math]::Abs([double]$_.PredictionOffsetY) }
                $offset -ge $predictionDistanceCap - 0.01
            }).Count
        }
        else { 0 }
        $predictionSideFlipCount = 0
        $predictionInterruptionCount = 0
        $predictionActiveRunFrames = [System.Collections.Generic.List[double]]::new()
        $predictionLeadDeltas = [System.Collections.Generic.List[double]]::new()
        $predictionLeadJerks = [System.Collections.Generic.List[double]]::new()
        $activeRunFrames = 0
        $wasPredictionActive = $false
        $lastPredictionSide = 0
        $previousActiveLead = $null
        $previousActiveDelta = $null
        if ($hasKinematicPrediction) {
            foreach ($sample in $samples) {
                $offset = if ($Axis -eq 'X') {
                    [double]$sample.PredictionOffsetX
                }
                else {
                    [double]$sample.PredictionOffsetY
                }
                $isPredictionActive =
                    [int]$sample.PredictionDirectionLocked -eq 1 -and
                    [math]::Abs($offset) -gt 0.001
                if ($isPredictionActive) {
                    ++$activeRunFrames
                    $lead = [math]::Abs($offset)
                    if ($null -ne $previousActiveLead) {
                        $delta = $lead - [double]$previousActiveLead
                        $predictionLeadDeltas.Add([math]::Abs($delta))
                        if ($null -ne $previousActiveDelta) {
                            $predictionLeadJerks.Add(
                                [math]::Abs($delta - [double]$previousActiveDelta))
                        }
                        $previousActiveDelta = $delta
                    }
                    else {
                        $previousActiveDelta = $null
                    }
                    $previousActiveLead = $lead
                }
                elseif ($wasPredictionActive) {
                    ++$predictionInterruptionCount
                    $predictionActiveRunFrames.Add($activeRunFrames)
                    $activeRunFrames = 0
                }
                if (-not $isPredictionActive) {
                    $previousActiveLead = $null
                    $previousActiveDelta = $null
                }
                $wasPredictionActive = $isPredictionActive
                if (-not $isPredictionActive) {
                    continue
                }
                $side = if ($offset -gt 0.0) { 1 } elseif ($offset -lt 0.0) { -1 } else { 0 }
                if ($side -ne 0 -and $lastPredictionSide -ne 0 -and $side -ne $lastPredictionSide) {
                    ++$predictionSideFlipCount
                }
                if ($side -ne 0) {
                    $lastPredictionSide = $side
                }
            }
            if ($activeRunFrames -gt 0) {
                $predictionActiveRunFrames.Add($activeRunFrames)
            }
        }
        $predictionActiveRunP50Frames = if ($predictionActiveRunFrames.Count -gt 0) {
            Get-PercentileValue -Values @($predictionActiveRunFrames) -Percentile 0.50
        }
        else { 0.0 }
        $predictionLeadDeltaP95 = if ($predictionLeadDeltas.Count -gt 0) {
            Get-PercentileValue -Values @($predictionLeadDeltas) -Percentile 0.95
        }
        else { 0.0 }
        $predictionLeadJerkP95 = if ($predictionLeadJerks.Count -gt 0) {
            Get-PercentileValue -Values @($predictionLeadJerks) -Percentile 0.95
        }
        else { 0.0 }
        $movingInsideSettleRows = if ($rows[0].PSObject.Properties.Name -contains 'MovingInsideSettle') {
            @($samples | Where-Object { [int]$_.MovingInsideSettle -eq 1 }).Count
        }
        else { 0 }
        $axisMovingInsideSettleColumn = if ($Axis -eq 'X') {
            'MovingInsideSettleX'
        } else { 'MovingInsideSettleY' }
        $axisMovingInsideSettleRows = if (
            $rows[0].PSObject.Properties.Name -contains $axisMovingInsideSettleColumn) {
            @($samples | Where-Object { [int]$_.$axisMovingInsideSettleColumn -eq 1 }).Count
        }
        else { $movingInsideSettleRows }
        $axisSettledColumn = if ($Axis -eq 'X') { 'SettledX' } else { 'SettledY' }
        $axisSettledRows = if ($rows[0].PSObject.Properties.Name -contains $axisSettledColumn) {
            @($samples | Where-Object { [int]$_.$axisSettledColumn -eq 1 }).Count
        }
        elseif ($rows[0].PSObject.Properties.Name -contains 'Settled') {
            @($samples | Where-Object { [int]$_.Settled -eq 1 }).Count
        }
        else { 0 }
        $verticalCatchUpRows = if ($rows[0].PSObject.Properties.Name -contains 'VerticalCatchUp') {
            @($samples | Where-Object { [int]$_.VerticalCatchUp -eq 1 }).Count
        }
        else { 0 }
        $horizontalCatchUpRows = if ($rows[0].PSObject.Properties.Name -contains 'HorizontalCatchUp') {
            @($samples | Where-Object { [int]$_.HorizontalCatchUp -eq 1 }).Count
        }
        else { 0 }
        $sampleDurationSeconds = (
            [double]$samples[-1].Timestamp - [double]$samples[0].Timestamp) / 1000.0
        $signedOutputCounts = [double]($samples | Measure-Object $finalMoveColumn -Sum).Sum
        $absoluteOutputCounts = [double](($samples | ForEach-Object {
            [math]::Abs([double]$_.$finalMoveColumn)
        } | Measure-Object -Sum).Sum)
        $outputSideFlipCount = 0
        $outputSideFlipAbsCounts = [System.Collections.Generic.List[double]]::new()
        $sameSideOutputSteps = [System.Collections.Generic.List[double]]::new()
        $sameSideOutputMagnitudes = [System.Collections.Generic.List[double]]::new()
        $sameSideOutputSides = [System.Collections.Generic.List[int]]::new()
        $lastOutputSide = 0
        $lastOutputMagnitude = 0.0
        foreach ($sample in $samples) {
            $move = [double]$sample.$finalMoveColumn
            $side = if ($move -gt 0.0) { 1 } elseif ($move -lt 0.0) { -1 } else { 0 }
            if ($side -eq 0) {
                continue
            }
            if ($lastOutputSide -ne 0 -and $side -ne $lastOutputSide) {
                ++$outputSideFlipCount
                $outputSideFlipAbsCounts.Add([math]::Abs($move))
            }
            elseif ($side -eq $lastOutputSide) {
                $sameSideOutputSteps.Add([math]::Abs([math]::Abs($move) - $lastOutputMagnitude))
            }
            $sameSideOutputMagnitudes.Add([math]::Abs($move))
            $sameSideOutputSides.Add($side)
            $lastOutputSide = $side
            $lastOutputMagnitude = [math]::Abs($move)
        }
        $sameSideOutputPulseCount = 0
        for ($index = 1; $index + 1 -lt $sameSideOutputMagnitudes.Count; ++$index) {
            if ($sameSideOutputSides[$index - 1] -ne $sameSideOutputSides[$index] -or
                $sameSideOutputSides[$index] -ne $sameSideOutputSides[$index + 1]) {
                continue
            }
            $leftStep = [math]::Abs(
                $sameSideOutputMagnitudes[$index] - $sameSideOutputMagnitudes[$index - 1])
            $rightStep = [math]::Abs(
                $sameSideOutputMagnitudes[$index + 1] - $sameSideOutputMagnitudes[$index])
            $isLocalExtreme =
                ($sameSideOutputMagnitudes[$index] -gt $sameSideOutputMagnitudes[$index - 1] -and
                 $sameSideOutputMagnitudes[$index] -gt $sameSideOutputMagnitudes[$index + 1]) -or
                ($sameSideOutputMagnitudes[$index] -lt $sameSideOutputMagnitudes[$index - 1] -and
                 $sameSideOutputMagnitudes[$index] -lt $sameSideOutputMagnitudes[$index + 1])
            # 3 counts 高于常见的整数余数抖动，可识别 -1/-8/-1 一类同方向强弱脉冲。
            if ($isLocalExtreme -and $leftStep -ge 3.0 -and $rightStep -ge 3.0) {
                ++$sameSideOutputPulseCount
            }
        }
        $requestedOutputSteps = [System.Collections.Generic.List[double]]::new()
        $integralOutputSteps = [System.Collections.Generic.List[double]]::new()
        $controllerUpdateIntervalsMs = [System.Collections.Generic.List[double]]::new()
        $observationIntervalsMs = [System.Collections.Generic.List[double]]::new()
        $hasIntegralCounts = $rows[0].PSObject.Properties.Name -contains $integralCountsColumn
        $hasControllerInterval = $rows[0].PSObject.Properties.Name -contains 'ControllerUpdateIntervalMs'
        $hasControlTime = $rows[0].PSObject.Properties.Name -contains 'ControlTimeNs'
        $hasBackendReceiveTime = $rows[0].PSObject.Properties.Name -contains 'BackendReceiveNs'
        for ($index = 1; $index -lt $samples.Count; ++$index) {
            $currentRequestedCounts = [double](
                $samples[$index].PSObject.Properties[$requestedCountsColumn].Value)
            $previousRequestedCounts = [double](
                $samples[$index - 1].PSObject.Properties[$requestedCountsColumn].Value)
            $requestedOutputSteps.Add([math]::Abs(
                $currentRequestedCounts - $previousRequestedCounts))
            if ($hasIntegralCounts) {
                $currentIntegralCounts = [double](
                    $samples[$index].PSObject.Properties[$integralCountsColumn].Value)
                $previousIntegralCounts = [double](
                    $samples[$index - 1].PSObject.Properties[$integralCountsColumn].Value)
                $integralOutputSteps.Add([math]::Abs(
                    $currentIntegralCounts - $previousIntegralCounts))
            }
            if ($hasControllerInterval) {
                $intervalMs = [double]$samples[$index].ControllerUpdateIntervalMs
                if ($intervalMs -gt 0.0) {
                    $controllerUpdateIntervalsMs.Add($intervalMs)
                }
            }
            elseif ($hasControlTime) {
                $intervalMs = ([double]$samples[$index].ControlTimeNs -
                    [double]$samples[$index - 1].ControlTimeNs) / 1000000.0
                if ($intervalMs -gt 0.0) {
                    $controllerUpdateIntervalsMs.Add($intervalMs)
                }
            }
            if ($hasBackendReceiveTime) {
                $intervalMs = ([double]$samples[$index].BackendReceiveNs -
                    [double]$samples[$index - 1].BackendReceiveNs) / 1000000.0
                if ($intervalMs -gt 0.0) {
                    $observationIntervalsMs.Add($intervalMs)
                }
            }
        }
        $countsPerPixelSamples = @($samples | Where-Object {
            [math]::Abs([double]$_.$requestedPixelColumn) -gt 0.000001
        } | ForEach-Object {
            [math]::Abs([double]$_.$requestedCountsColumn / [double]$_.$requestedPixelColumn)
        })
        # static 九段可能出现某一轴完全无需输出的有效试次。此时没有可用于反推
        # counts/px 的样本，使用 0 表示“本试次不可估算”，避免把正常零位移误报为分析失败。
        $estimatedCountsPerPixel = if ($countsPerPixelSamples.Count -gt 0) {
            Get-PercentileValue -Values $countsPerPixelSamples -Percentile 0.50
        }
        else { 0.0 }
        $signedOutputCps = $signedOutputCounts / [math]::Max(0.001, $sampleDurationSeconds)
        $absoluteOutputCps = $absoluteOutputCounts / [math]::Max(0.001, $sampleDurationSeconds)
        # 匀速单向场景中，平均误差乘 counts/px 后除以实际输出 counts/s，
        # 可得到闭环滞后的近似时间；该值用于辨别响应时间而非速度上限瓶颈。
        $approxClosedLoopLagMs = 1000.0 * [double]($axisAbsErrors | Measure-Object -Average).Average *
            $estimatedCountsPerPixel / [math]::Max(1.0, $absoluteOutputCps)
        $observedApproxClosedLoopLagMs = if ($hasObservedTracking) {
            1000.0 * [double]($observedAxisAbsErrors | Measure-Object -Average).Average *
                $estimatedCountsPerPixel / [math]::Max(1.0, $absoluteOutputCps)
        }
        else { 0.0 }
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
            ObservedTrackingAvailable = if ($hasObservedTracking) { 1 } else { 0 }
            ObservedMeanAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round([double]($observedAxisErrors | Measure-Object -Average).Average, 2)
            } else { 0.0 }
            ObservedMeanAbsAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round([double]($observedAxisAbsErrors | Measure-Object -Average).Average, 2)
            } else { 0.0 }
            ObservedP95AbsAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round((Get-PercentileValue -Values $observedAxisAbsErrors -Percentile 0.95), 2)
            } else { 0.0 }
            ObservedP99AbsAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round((Get-PercentileValue -Values $observedAxisAbsErrors -Percentile 0.99), 2)
            } else { 0.0 }
            ObservedP95DistancePx = if ($hasObservedBoxTracking) {
                [math]::Round((Get-PercentileValue -Values $observedDistances -Percentile 0.95), 2)
            } else { 0.0 }
            ObservedInsideTargetPct = if ($hasObservedBoxTracking) {
                [math]::Round(100.0 * $insideTargetRows / [math]::Max(1, $samples.Count), 1)
            } else { 0.0 }
            SteadySamples = $steadySamples.Count
            SteadyMeanAbsAxisErrorPx = [math]::Round([double]($steadyAxisAbsErrors | Measure-Object -Average).Average, 2)
            SteadyP95AbsAxisErrorPx = [math]::Round((Get-PercentileValue -Values $steadyAxisAbsErrors -Percentile 0.95), 2)
            ObservedSteadyMeanAbsAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round([double]($observedSteadyAxisAbsErrors | Measure-Object -Average).Average, 2)
            } else { 0.0 }
            ObservedSteadyP95AbsAxisErrorPx = if ($hasObservedTracking) {
                [math]::Round((Get-PercentileValue -Values $observedSteadyAxisAbsErrors -Percentile 0.95), 2)
            } else { 0.0 }
            P95ErrorDistancePx = [math]::Round((Get-PercentileValue -Values $distanceErrors -Percentile 0.95), 2)
            MaxErrorDistancePx = [math]::Round([double]($distanceErrors | Measure-Object -Maximum).Maximum, 2)
            P50ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.50), 1)
            P95ObservedAxisSpeed = [math]::Round((Get-PercentileValue -Values $axisSpeeds -Percentile 0.95), 1)
            P95FilterResidualPx = [math]::Round((Get-PercentileValue -Values $residuals -Percentile 0.95), 2)
            SignedOutputCountsPerSecond = [math]::Round($signedOutputCps, 1)
            MeanAbsOutputCountsPerSecond = [math]::Round($absoluteOutputCps, 1)
            EstimatedCountsPerPixel = [math]::Round($estimatedCountsPerPixel, 4)
            ApproxClosedLoopLagMs = [math]::Round($approxClosedLoopLagMs, 1)
            ObservedApproxClosedLoopLagMs = [math]::Round($observedApproxClosedLoopLagMs, 1)
            OutputSideFlipCount = $outputSideFlipCount
            OutputSideFlipRateHz = [math]::Round(
                $outputSideFlipCount / [math]::Max(0.001, $sampleDurationSeconds), 2)
            OutputSideFlipMeanAbsCounts = if ($outputSideFlipAbsCounts.Count -gt 0) {
                [math]::Round([double]($outputSideFlipAbsCounts | Measure-Object -Average).Average, 2)
            } else { 0.0 }
            OutputSideFlipMaxAbsCounts = if ($outputSideFlipAbsCounts.Count -gt 0) {
                [math]::Round([double]($outputSideFlipAbsCounts | Measure-Object -Maximum).Maximum, 2)
            } else { 0.0 }
            OutputSameSideStepP95Counts = if ($sameSideOutputSteps.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($sameSideOutputSteps) -Percentile 0.95), 2)
            } else { 0.0 }
            RequestedOutputStepP95Counts = if ($requestedOutputSteps.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($requestedOutputSteps) -Percentile 0.95), 2)
            } else { 0.0 }
            IntegralOutputStepP95Counts = if ($integralOutputSteps.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($integralOutputSteps) -Percentile 0.95), 2)
            } else { 0.0 }
            ControllerUpdateIntervalP05Ms = if ($controllerUpdateIntervalsMs.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($controllerUpdateIntervalsMs) -Percentile 0.05), 2)
            } else { 0.0 }
            ControllerUpdateIntervalP95Ms = if ($controllerUpdateIntervalsMs.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($controllerUpdateIntervalsMs) -Percentile 0.95), 2)
            } else { 0.0 }
            ObservationIntervalP05Ms = if ($observationIntervalsMs.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($observationIntervalsMs) -Percentile 0.05), 2)
            } else { 0.0 }
            ObservationIntervalP95Ms = if ($observationIntervalsMs.Count -gt 0) {
                [math]::Round((Get-PercentileValue -Values @($observationIntervalsMs) -Percentile 0.95), 2)
            } else { 0.0 }
            OutputSameSidePulseCount = $sameSideOutputPulseCount
            OutputSameSidePulseRateHz = [math]::Round(
                $sameSideOutputPulseCount / [math]::Max(0.001, $sampleDurationSeconds), 2)
            SpeedLimitedPct = [math]::Round(100.0 * $limitedRows / [math]::Max(1, $samples.Count), 1)
            PredictionActivePct = [math]::Round(
                100.0 * $predictionRows.Count / [math]::Max(1, $samples.Count), 1)
            PredictionSelfMotionSuppressedPct = [math]::Round(
                100.0 * $selfMotionSuppressedRows / [math]::Max(1, $samples.Count), 1)
            PredictionOscillationSuppressedPct = [math]::Round(
                100.0 * $oscillationSuppressedRows / [math]::Max(1, $samples.Count), 1)
            PredictionHighSpeedSuppressedPct = [math]::Round(
                100.0 * $highSpeedSuppressedRows / [math]::Max(1, $samples.Count), 1)
            PredictionStationarySuppressedPct = [math]::Round(
                100.0 * $stationarySuppressedRows / [math]::Max(1, $samples.Count), 1)
            PredictionMotionEvidenceSuppressedPct = [math]::Round(
                100.0 * $motionEvidenceSuppressedRows / [math]::Max(1, $samples.Count), 1)
            P50PredictionLeadPx = [math]::Round($predictionLeadP50, 2)
            P95PredictionLeadPx = [math]::Round($predictionLeadP95, 2)
            PredictionActiveSamples = $predictionRows.Count
            P95PredictionLeadDeltaPx = [math]::Round($predictionLeadDeltaP95, 2)
            P95PredictionLeadJerkPx = [math]::Round($predictionLeadJerkP95, 2)
            PredictionLeadCappedPct = [math]::Round(
                100.0 * $predictionCappedRows / [math]::Max(1, $predictionRows.Count), 1)
            PredictionSideFlipCount = $predictionSideFlipCount
            PredictionInterruptionCount = $predictionInterruptionCount
            P50PredictionActiveRunFrames = [math]::Round($predictionActiveRunP50Frames, 1)
            MovingInsideSettlePct = [math]::Round(100.0 * $movingInsideSettleRows / [math]::Max(1, $samples.Count), 1)
            AxisMovingInsideSettlePct = [math]::Round(
                100.0 * $axisMovingInsideSettleRows / [math]::Max(1, $samples.Count), 1)
            AxisSettledPct = [math]::Round(
                100.0 * $axisSettledRows / [math]::Max(1, $samples.Count), 1)
            HorizontalCatchUpPct = [math]::Round(100.0 * $horizontalCatchUpRows / [math]::Max(1, $samples.Count), 1)
            VerticalCatchUpPct = [math]::Round(100.0 * $verticalCatchUpRows / [math]::Max(1, $samples.Count), 1)
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
    $predictionSampleCount = [int]($group | Measure-Object PredictionActiveSamples -Sum).Sum
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
        ObservedTrackingAvailable = [int]($group | Measure-Object ObservedTrackingAvailable -Minimum).Minimum
        MeanObservedP95AbsAxisErrorPx = [math]::Round(
            [double]($group | Measure-Object ObservedP95AbsAxisErrorPx -Average).Average, 2)
        WorstObservedP95AbsAxisErrorPx = [math]::Round(
            [double]($group | Measure-Object ObservedP95AbsAxisErrorPx -Maximum).Maximum, 2)
        MeanObservedP99AbsAxisErrorPx = [math]::Round(
            [double]($group | Measure-Object ObservedP99AbsAxisErrorPx -Average).Average, 2)
        MeanObservedP95DistancePx = [math]::Round(
            [double]($group | Measure-Object ObservedP95DistancePx -Average).Average, 2)
        ObservedInsideTargetPct = [math]::Round([double](($group | ForEach-Object {
            $_.ObservedInsideTargetPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        MeanSteadyMeanAbsAxisErrorPx = [math]::Round([double]($group | Measure-Object SteadyMeanAbsAxisErrorPx -Average).Average, 2)
        MeanSteadyP95AbsAxisErrorPx = [math]::Round([double]($group | Measure-Object SteadyP95AbsAxisErrorPx -Average).Average, 2)
        MeanObservedSteadyMeanAbsAxisErrorPx = [math]::Round(
            [double]($group | Measure-Object ObservedSteadyMeanAbsAxisErrorPx -Average).Average, 2)
        MeanObservedSteadyP95AbsAxisErrorPx = [math]::Round(
            [double]($group | Measure-Object ObservedSteadyP95AbsAxisErrorPx -Average).Average, 2)
        MeanP95ErrorDistancePx = [math]::Round([double]($group | Measure-Object P95ErrorDistancePx -Average).Average, 2)
        MeanAbsOutputCountsPerSecond = [math]::Round([double]($group | Measure-Object MeanAbsOutputCountsPerSecond -Average).Average, 1)
        MeanApproxClosedLoopLagMs = [math]::Round([double]($group | Measure-Object ApproxClosedLoopLagMs -Average).Average, 1)
        MeanObservedApproxClosedLoopLagMs = [math]::Round(
            [double]($group | Measure-Object ObservedApproxClosedLoopLagMs -Average).Average, 1)
        OutputSideFlipCount = [int]($group | Measure-Object OutputSideFlipCount -Sum).Sum
        OutputSideFlipRateHz = [math]::Round(
            [double]($group | Measure-Object OutputSideFlipCount -Sum).Sum /
            [math]::Max(0.001, [double]($group | Measure-Object DurationMs -Sum).Sum / 1000.0), 2)
        OutputSideFlipMeanAbsCounts = if (
            [int]($group | Measure-Object OutputSideFlipCount -Sum).Sum -gt 0) {
            [math]::Round([double](($group | ForEach-Object {
                $_.OutputSideFlipMeanAbsCounts * $_.OutputSideFlipCount
            } | Measure-Object -Sum).Sum) /
                [int]($group | Measure-Object OutputSideFlipCount -Sum).Sum, 2)
        } else { 0.0 }
        OutputSideFlipMaxAbsCounts = [math]::Round(
            [double]($group | Measure-Object OutputSideFlipMaxAbsCounts -Maximum).Maximum, 2)
        MeanOutputSameSideStepP95Counts = [math]::Round(
            [double]($group | Measure-Object OutputSameSideStepP95Counts -Average).Average, 2)
        MeanRequestedOutputStepP95Counts = [math]::Round(
            [double]($group | Measure-Object RequestedOutputStepP95Counts -Average).Average, 2)
        MeanIntegralOutputStepP95Counts = [math]::Round(
            [double]($group | Measure-Object IntegralOutputStepP95Counts -Average).Average, 2)
        MeanControllerUpdateIntervalP05Ms = [math]::Round(
            [double]($group | Measure-Object ControllerUpdateIntervalP05Ms -Average).Average, 2)
        MeanControllerUpdateIntervalP95Ms = [math]::Round(
            [double]($group | Measure-Object ControllerUpdateIntervalP95Ms -Average).Average, 2)
        MeanObservationIntervalP05Ms = [math]::Round(
            [double]($group | Measure-Object ObservationIntervalP05Ms -Average).Average, 2)
        MeanObservationIntervalP95Ms = [math]::Round(
            [double]($group | Measure-Object ObservationIntervalP95Ms -Average).Average, 2)
        OutputSameSidePulseCount = [int](
            $group | Measure-Object OutputSameSidePulseCount -Sum).Sum
        OutputSameSidePulseRateHz = [math]::Round(
            [double]($group | Measure-Object OutputSameSidePulseCount -Sum).Sum /
            [math]::Max(0.001, [double]($group | Measure-Object DurationMs -Sum).Sum / 1000.0), 2)
        SpeedLimitedPct = [math]::Round([double](($group | ForEach-Object { $_.SpeedLimitedPct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionActivePct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionActivePct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionSelfMotionSuppressedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionSelfMotionSuppressedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionOscillationSuppressedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionOscillationSuppressedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionHighSpeedSuppressedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionHighSpeedSuppressedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionStationarySuppressedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionStationarySuppressedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        PredictionMotionEvidenceSuppressedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionMotionEvidenceSuppressedPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        MeanP50PredictionLeadPx = [math]::Round(
            [double]($group | Measure-Object P50PredictionLeadPx -Average).Average, 2)
        MeanP95PredictionLeadPx = [math]::Round(
            [double]($group | Measure-Object P95PredictionLeadPx -Average).Average, 2)
        MeanP95PredictionLeadDeltaPx = [math]::Round(
            [double]($group | Measure-Object P95PredictionLeadDeltaPx -Average).Average, 2)
        WorstP95PredictionLeadDeltaPx = [math]::Round(
            [double]($group | Measure-Object P95PredictionLeadDeltaPx -Maximum).Maximum, 2)
        MeanP95PredictionLeadJerkPx = [math]::Round(
            [double]($group | Measure-Object P95PredictionLeadJerkPx -Average).Average, 2)
        WorstP95PredictionLeadJerkPx = [math]::Round(
            [double]($group | Measure-Object P95PredictionLeadJerkPx -Maximum).Maximum, 2)
        PredictionLeadCappedPct = [math]::Round([double](($group | ForEach-Object {
            $_.PredictionLeadCappedPct * $_.PredictionActiveSamples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $predictionSampleCount), 1)
        PredictionSideFlipCount = [int]($group | Measure-Object PredictionSideFlipCount -Sum).Sum
        PredictionInterruptionCount = [int]($group | Measure-Object PredictionInterruptionCount -Sum).Sum
        MeanP50PredictionActiveRunFrames = [math]::Round(
            [double]($group | Measure-Object P50PredictionActiveRunFrames -Average).Average, 1)
        MovingInsideSettlePct = [math]::Round([double](($group | ForEach-Object { $_.MovingInsideSettlePct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        AxisMovingInsideSettlePct = [math]::Round([double](($group | ForEach-Object {
            $_.AxisMovingInsideSettlePct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        AxisSettledPct = [math]::Round([double](($group | ForEach-Object {
            $_.AxisSettledPct * $_.Samples
        } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        HorizontalCatchUpPct = [math]::Round([double](($group | ForEach-Object { $_.HorizontalCatchUpPct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
        VerticalCatchUpPct = [math]::Round([double](($group | ForEach-Object { $_.VerticalCatchUpPct * $_.Samples } | Measure-Object -Sum).Sum) / [math]::Max(1, $sampleCount), 1)
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
        'ObservedTrackingAvailable', 'ObservedMeanAxisErrorPx', 'ObservedMeanAbsAxisErrorPx',
        'ObservedP95AbsAxisErrorPx', 'ObservedP99AbsAxisErrorPx', 'ObservedP95DistancePx',
        'ObservedInsideTargetPct', 'MeanObservedP95AbsAxisErrorPx', 'WorstObservedP95AbsAxisErrorPx',
        'MeanObservedP99AbsAxisErrorPx', 'MeanObservedP95DistancePx',
        'SteadySamples', 'SteadyMeanAbsAxisErrorPx', 'SteadyP95AbsAxisErrorPx',
        'ObservedSteadyMeanAbsAxisErrorPx', 'ObservedSteadyP95AbsAxisErrorPx',
        'MeanP95AbsAxisErrorPx', 'WorstP95AbsAxisErrorPx', 'P95ErrorDistancePx',
        'MeanSteadyMeanAbsAxisErrorPx', 'MeanSteadyP95AbsAxisErrorPx',
        'MeanObservedSteadyMeanAbsAxisErrorPx', 'MeanObservedSteadyP95AbsAxisErrorPx',
        'MeanP95ErrorDistancePx', 'MaxErrorDistancePx', 'P50ObservedAxisSpeed',
        'P95ObservedAxisSpeed', 'P95FilterResidualPx', 'SignedOutputCountsPerSecond',
        'MeanAbsOutputCountsPerSecond', 'EstimatedCountsPerPixel', 'ApproxClosedLoopLagMs',
        'ObservedApproxClosedLoopLagMs', 'MeanApproxClosedLoopLagMs',
        'MeanObservedApproxClosedLoopLagMs', 'OutputSideFlipCount', 'OutputSideFlipRateHz',
        'OutputSideFlipMeanAbsCounts', 'OutputSideFlipMaxAbsCounts',
        'OutputSameSideStepP95Counts', 'MeanOutputSameSideStepP95Counts',
        'RequestedOutputStepP95Counts', 'MeanRequestedOutputStepP95Counts',
        'IntegralOutputStepP95Counts', 'MeanIntegralOutputStepP95Counts',
        'ControllerUpdateIntervalP05Ms', 'ControllerUpdateIntervalP95Ms',
        'MeanControllerUpdateIntervalP05Ms', 'MeanControllerUpdateIntervalP95Ms',
        'ObservationIntervalP05Ms', 'ObservationIntervalP95Ms',
        'MeanObservationIntervalP05Ms', 'MeanObservationIntervalP95Ms',
        'OutputSameSidePulseCount', 'OutputSameSidePulseRateHz',
        'SpeedLimitedPct', 'PredictionActivePct', 'PredictionSelfMotionSuppressedPct',
        'PredictionOscillationSuppressedPct', 'PredictionHighSpeedSuppressedPct',
        'PredictionStationarySuppressedPct', 'PredictionMotionEvidenceSuppressedPct',
        'P50PredictionLeadPx', 'P95PredictionLeadPx', 'MeanP50PredictionLeadPx', 'MeanP95PredictionLeadPx',
        'PredictionActiveSamples', 'P95PredictionLeadDeltaPx', 'P95PredictionLeadJerkPx',
        'PredictionLeadCappedPct', 'MeanP95PredictionLeadDeltaPx',
        'WorstP95PredictionLeadDeltaPx', 'MeanP95PredictionLeadJerkPx',
        'WorstP95PredictionLeadJerkPx',
        'PredictionSideFlipCount', 'PredictionInterruptionCount',
        'P50PredictionActiveRunFrames', 'MeanP50PredictionActiveRunFrames',
        'MovingInsideSettlePct', 'AxisMovingInsideSettlePct', 'AxisSettledPct',
        'HorizontalCatchUpPct', 'VerticalCatchUpPct', 'InferenceFps',
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
$trialMetrics | Format-Table Chain, Scenario, Trial, DurationMs, ObservedP95AbsAxisErrorPx, ObservedSteadyP95AbsAxisErrorPx, ObservedInsideTargetPct, P95AbsAxisErrorPx, OutputSideFlipCount, OutputSameSideStepP95Counts, RequestedOutputStepP95Counts, IntegralOutputStepP95Counts, ControllerUpdateIntervalP05Ms, ControllerUpdateIntervalP95Ms, ObservationIntervalP05Ms, ObservationIntervalP95Ms, OutputSameSidePulseCount, OutputSameSidePulseRateHz, AxisSettledPct, AxisMovingInsideSettlePct, PredictionActivePct, PredictionSelfMotionSuppressedPct, PredictionMotionEvidenceSuppressedPct, PredictionOscillationSuppressedPct, PredictionHighSpeedSuppressedPct, PredictionStationarySuppressedPct, HorizontalCatchUpPct, VerticalCatchUpPct, P50PredictionLeadPx, P95PredictionLeadPx, P95PredictionLeadDeltaPx, P95PredictionLeadJerkPx, PredictionLeadCappedPct, PredictionInterruptionCount, P50PredictionActiveRunFrames, PredictionSideFlipCount, ReversalCount, SpeedLimitedPct -AutoSize
Write-Host '[moving-target] Scenario summary' -ForegroundColor Cyan
$scenarioMetrics | Format-Table Chain, Scenario, Trials, MeanObservedP95AbsAxisErrorPx, MeanObservedSteadyP95AbsAxisErrorPx, ObservedInsideTargetPct, MeanP95AbsAxisErrorPx, OutputSideFlipCount, MeanOutputSameSideStepP95Counts, MeanRequestedOutputStepP95Counts, MeanIntegralOutputStepP95Counts, MeanControllerUpdateIntervalP05Ms, MeanControllerUpdateIntervalP95Ms, MeanObservationIntervalP05Ms, MeanObservationIntervalP95Ms, OutputSameSidePulseCount, OutputSameSidePulseRateHz, AxisSettledPct, AxisMovingInsideSettlePct, PredictionActivePct, PredictionSelfMotionSuppressedPct, PredictionMotionEvidenceSuppressedPct, PredictionOscillationSuppressedPct, PredictionHighSpeedSuppressedPct, PredictionStationarySuppressedPct, HorizontalCatchUpPct, VerticalCatchUpPct, MeanP50PredictionLeadPx, MeanP95PredictionLeadPx, MeanP95PredictionLeadDeltaPx, MeanP95PredictionLeadJerkPx, PredictionLeadCappedPct, PredictionInterruptionCount, MeanP50PredictionActiveRunFrames, PredictionSideFlipCount, ReversalCount, SpeedLimitedPct -AutoSize
