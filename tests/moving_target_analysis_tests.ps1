[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [Parameter(Mandatory)][string]$Message
    )
    if ([string]$Expected -ne [string]$Actual) {
        throw "$Message Expected='$Expected', Actual='$Actual'."
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('xen-moving-test-' + [guid]::NewGuid().ToString('N'))
try {
    $dataDirectory = Join-Path $temporaryRoot 'CUDA\udp'
    New-Item -ItemType Directory -Path $dataDirectory | Out-Null
    $unrelatedDirectory = Join-Path $temporaryRoot 'Video\analysis'
    New-Item -ItemType Directory -Path $unrelatedDirectory -Force | Out-Null
    'LeadMs,P95AbsX`n20,12.5' | Set-Content -LiteralPath (Join-Path $unrelatedDirectory 'prediction_grid.csv') -Encoding UTF8
    $csvPath = Join-Path $dataDirectory 'horizontal_reverse.csv'
    @'
Timestamp,SourceWidth,SourceHeight,InferenceFPS,SourceReceiveFPS,ObservationAgeSec,ErrorX,ErrorY,ErrorDistance,FilterResidual,ObservedVelocityX,ObservedVelocityY,RequestedPixelX,RequestedPixelY,RequestedCountsX,RequestedCountsY,FinalMx,FinalMy,SpeedLimited,QueuedMoveCount,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,MovingInsideSettle
1000,2560,1440,120,240,0.010,11,1,11.05,2,100,0,1,0,1.344,0,1,0,0,1,CUDA,test-revision,20260714T010000Z,4,0
1010,2560,1440,120,240,0.010,11,1,11.05,2,100,0,1,0,1.344,0,1,0,0,1,CUDA,test-revision,20260714T010000Z,4,1
1020,2560,1440,120,240,0.010,11,1,11.05,3,-100,0,1,0,1.344,0,1,0,1,1,CUDA,test-revision,20260714T010000Z,4,1
1030,2560,1440,120,240,0.010,7,1,7.07,2,-100,0,1,0,1.344,0,1,0,0,1,CUDA,test-revision,20260714T010000Z,4,0
1040,2560,1440,120,240,0.010,0,1,1.00,1,-100,0,1,0,1.344,0,1,0,0,0,CUDA,test-revision,20260714T010000Z,4,0
1050,2560,1440,120,240,0.010,-11,1,11.05,1,-100,0,-1,0,-1.344,0,-1,0,0,0,CUDA,test-revision,20260714T010000Z,4,0
1060,2560,1440,120,240,0.010,-11,1,11.05,1,-100,0,-1,0,-1.344,0,-1,0,0,0,CUDA,test-revision,20260714T010000Z,4,0
1300,2560,1440,120,240,0.010,-9,1,9.06,2,-80,0,-1,0,-1.344,0,-1,0,0,1,CUDA,test-revision,20260714T010000Z,4,0
1310,2560,1440,120,240,0.010,-8,1,8.06,2,-80,0,-1,0,-1.344,0,-1,0,0,1,CUDA,test-revision,20260714T010000Z,4,0
1320,2560,1440,120,240,0.010,-7,1,7.07,2,-80,0,-1,0,-1.344,0,-1,0,0,0,CUDA,test-revision,20260714T010000Z,4,0
'@ | Set-Content -LiteralPath $csvPath -Encoding UTF8

    $augmentedRows = @(Import-Csv -LiteralPath $csvPath)
    foreach ($row in $augmentedRows) {
        $timestamp = [double]$row.Timestamp
        $predictionActive = $timestamp -in @(1000, 1010, 1020, 1050, 1060)
        $offset = if ($timestamp -in @(1000, 1020)) { 30.0 }
            elseif ($timestamp -eq 1010) { 20.0 }
            elseif ($timestamp -eq 1050) { -20.0 }
            elseif ($timestamp -eq 1060) { -10.0 }
            else { 0.0 }
        # Predicted minus Error is the crosshair center; lead offset is not observed error.
        $predictionLocked = if ($predictionActive) { 1 } else { 0 }
        $row | Add-Member -NotePropertyName PredictionDirectionLocked -NotePropertyValue $predictionLocked
        $row | Add-Member -NotePropertyName PredictionOffsetX -NotePropertyValue $offset
        $row | Add-Member -NotePropertyName PredictionOffsetY -NotePropertyValue 0.0
        $row | Add-Member -NotePropertyName CaptureRoiWidth -NotePropertyValue 320.0
        $row | Add-Member -NotePropertyName PredictedX -NotePropertyValue $(100.0 + [double]$row.ErrorX)
        $row | Add-Member -NotePropertyName PredictedY -NotePropertyValue $(100.0 + [double]$row.ErrorY)
        $row | Add-Member -NotePropertyName RawPivotX -NotePropertyValue 105.0
        $row | Add-Member -NotePropertyName RawPivotY -NotePropertyValue 103.0
        $row | Add-Member -NotePropertyName TargetBoxX -NotePropertyValue 90.0
        $row | Add-Member -NotePropertyName TargetBoxY -NotePropertyValue 90.0
        $row | Add-Member -NotePropertyName TargetBoxWidth -NotePropertyValue 20.0
        $row | Add-Member -NotePropertyName TargetBoxHeight -NotePropertyValue 20.0
        $row | Add-Member -NotePropertyName MovingInsideSettleX -NotePropertyValue $row.MovingInsideSettle
        $row | Add-Member -NotePropertyName MovingInsideSettleY -NotePropertyValue 0
        $row | Add-Member -NotePropertyName SettledX -NotePropertyValue 0
        $row | Add-Member -NotePropertyName SettledY -NotePropertyValue 1
        $row | Add-Member -NotePropertyName PredictionStationarySuppressed `
            -NotePropertyValue $(if ($timestamp -eq 1030) { 1 } else { 0 })
    }
    $augmentedRows | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8

    $singleDirectionPath = Join-Path $dataDirectory 'horizontal_right.csv'
    Copy-Item -LiteralPath $csvPath -Destination $singleDirectionPath

    $outputCsv = Join-Path $temporaryRoot 'summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -ReversalErrorThresholdPx 8 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -OutputCsv $outputCsv -PassThru)
    $trials = @($metrics | Where-Object { $_.Level -eq 'Trial' })
    $scenario = @($metrics | Where-Object { $_.Level -eq 'Scenario' })

    Assert-Equal 4 $trials.Count 'A gap larger than 150 ms must create two trials per scenario.'
    $reverseTrials = @($trials | Where-Object { $_.Scenario -eq 'horizontal_reverse' })
    $singleDirectionTrials = @($trials | Where-Object { $_.Scenario -eq 'horizontal_right' })
    Assert-Equal 1 $reverseTrials[0].ReversalCount 'Persistent tracking-error side reversal must be counted.'
    Assert-Equal 11 $reverseTrials[0].StartAbsAxisErrorPx 'Trial metrics must preserve the initial absolute axis error.'
    Assert-Equal 16.67 $reverseTrials[0].ReversalRateHz 'Trial reversal rate must use the effective trial duration.'
    Assert-Equal 1 $reverseTrials[0].RecoveredReversals 'Reversal recovery inside the radius must be counted.'
    Assert-Equal 30 $reverseTrials[0].RecoveryMeanMs 'Recovery time must start at the confirmed positive peak.'
    Assert-Equal $true ([double]$reverseTrials[0].RecoveryMeanMs -ge 0) 'Reversal recovery time must never be negative.'
    Assert-Equal 0 $singleDirectionTrials[0].ReversalCount 'Single-direction scenarios must not report reversal metrics.'
    Assert-Equal 1.344 $reverseTrials[0].EstimatedCountsPerPixel 'Counts-per-pixel must be derived from exported requests.'
    Assert-Equal test-revision $reverseTrials[0].BuildRevision 'Build revision must be preserved in trial metrics.'
    Assert-Equal 4 $reverseTrials[0].ControllerRevision 'Controller revision must be preserved in trial metrics.'
    Assert-Equal 28.6 $reverseTrials[0].MovingInsideSettlePct 'Settle motion diagnostics must be summarized per trial.'
    Assert-Equal 28.6 $reverseTrials[0].AxisMovingInsideSettlePct 'Per-axis settle release diagnostics must use the selected axis.'
    Assert-Equal 0 $reverseTrials[0].AxisSettledPct 'Per-axis settle occupancy must use the selected axis.'
    Assert-Equal 0 $reverseTrials[0].VerticalCatchUpPct 'Legacy CSV without vertical catch-up diagnostics must remain compatible.'
    Assert-Equal 0 $reverseTrials[0].HorizontalCatchUpPct 'Legacy CSV without horizontal catch-up diagnostics must remain compatible.'
    Assert-Equal 71.4 $reverseTrials[0].PredictionActivePct 'Kinematic prediction activation must be summarized.'
    Assert-Equal 0 $reverseTrials[0].PredictionSelfMotionSuppressedPct 'Legacy CSV without self-motion gate diagnostics must remain compatible.'
    Assert-Equal 0 $reverseTrials[0].PredictionOscillationSuppressedPct 'Legacy CSV without oscillation gate diagnostics must remain compatible.'
    Assert-Equal 0 $reverseTrials[0].PredictionHighSpeedSuppressedPct 'Legacy CSV without high-speed gate diagnostics must remain compatible.'
    Assert-Equal 14.3 $reverseTrials[0].PredictionStationarySuppressedPct 'Confirmed stationary prediction suppression must be summarized.'
    Assert-Equal 20 $reverseTrials[0].P50PredictionLeadPx 'Kinematic median lead must use active prediction offsets.'
    Assert-Equal 10 $reverseTrials[0].P95PredictionLeadDeltaPx 'Prediction smoothness must use active-to-active lead magnitude deltas.'
    Assert-Equal 20 $reverseTrials[0].P95PredictionLeadJerkPx 'Prediction smoothness must expose alternating lead-magnitude deltas.'
    Assert-Equal 40 $reverseTrials[0].PredictionLeadCappedPct 'Prediction cap occupancy must use active prediction samples only.'
    Assert-Equal 1 $reverseTrials[0].PredictionInterruptionCount 'Prediction active-to-inactive interruptions must be counted.'
    Assert-Equal 2 $reverseTrials[0].P50PredictionActiveRunFrames 'Prediction active run length must expose one-frame gating churn.'
    Assert-Equal 1 $reverseTrials[0].PredictionSideFlipCount 'Prediction side changes must remain independently auditable.'
    Assert-Equal 1 $reverseTrials[0].ObservedTrackingAvailable 'Raw pivot diagnostics must enable observed tracking metrics.'
    Assert-Equal 5 $reverseTrials[0].ObservedP95AbsAxisErrorPx 'Observed error must use raw pivot relative to the inferred aim center.'
    Assert-Equal 5 $reverseTrials[0].ObservedSteadyP95AbsAxisErrorPx 'Observed steady error must use the same raw target coordinate.'
    Assert-Equal 5.83 $reverseTrials[0].ObservedP95DistancePx 'Observed distance must preserve the two-axis screen error.'
    Assert-Equal 100 $reverseTrials[0].ObservedInsideTargetPct 'Aim center inside the detected box must be reported.'
    Assert-Equal 1 $reverseTrials[0].OutputSideFlipCount 'Final device output side changes must be counted independently.'
    Assert-Equal 1 $reverseTrials[0].OutputSideFlipMeanAbsCounts 'Output flip mean magnitude must use the new-side command.'
    Assert-Equal 1 $reverseTrials[0].OutputSideFlipMaxAbsCounts 'Output flip maximum magnitude must remain auditable.'
    Assert-Equal $reverseTrials[0].P95AbsAxisErrorPx $reverseTrials[0].SteadyP95AbsAxisErrorPx 'Short synthetic trials must use all samples as the steady window.'
    Assert-Equal 2 $scenario.Count 'Each scenario file must create one summary.'
    Assert-Equal 2 $scenario[0].Trials 'Scenario summary must report both trials.'
    Assert-Equal 10 $scenario[0].MeanStartAbsAxisErrorPx 'Scenario summary must report the mean trial start error.'
    Assert-Equal 9 $scenario[0].MinStartAbsAxisErrorPx 'Scenario summary must report the minimum trial start error.'
    Assert-Equal 11 $scenario[0].MaxStartAbsAxisErrorPx 'Scenario summary must report the maximum trial start error.'
    Assert-Equal 12.5 $scenario[0].ReversalRateHz 'Scenario reversal rate must use total valid duration.'
    Assert-Equal 1 $scenario[0].MaxQueuedMoves 'Scenario summary must preserve maximum queue depth.'
    Assert-Equal 30 $scenario[0].RecoveryP95Ms 'Scenario summary must preserve the worst trial recovery P95.'
    Assert-Equal 5 $scenario[0].MeanObservedP95AbsAxisErrorPx 'Scenario summary must retain observed target error.'
    Assert-Equal 5 $scenario[0].MeanObservedSteadyP95AbsAxisErrorPx 'Scenario summary must retain observed steady target error.'
    Assert-Equal 1 $scenario[0].OutputSideFlipCount 'Scenario summary must sum device output side changes across trials.'
    Assert-Equal 1 $scenario[0].OutputSideFlipMeanAbsCounts 'Scenario summary must weight output flip magnitude by flip count.'
    Assert-Equal 5 $scenario[0].MeanP95PredictionLeadDeltaPx 'Scenario summary must preserve prediction lead smoothness.'
    Assert-Equal 10 $scenario[0].MeanP95PredictionLeadJerkPx 'Scenario summary must preserve prediction lead jerk.'
    Assert-Equal 40 $scenario[0].PredictionLeadCappedPct 'Scenario summary must weight cap occupancy by active samples.'
    Assert-Equal 10 $scenario[0].PredictionStationarySuppressedPct 'Scenario summary must weight stationary suppression by samples.'
    $exportedRows = @(Import-Csv -LiteralPath $outputCsv)
    Assert-Equal 6 $exportedRows.Count 'CSV export must include trials and scenario summaries.'
    Assert-Equal 2 $exportedRows[-1].Trials 'CSV export must retain scenario-only summary columns.'
    $rerunMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -ReversalErrorThresholdPx 8 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -OutputCsv $outputCsv -PassThru)
    Assert-Equal 6 $rerunMetrics.Count 'Repeated analysis must ignore its own root-level summary CSV.'

    $filteredMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 50 -MinTrialSamples 5 -PassThru -WarningAction SilentlyContinue)
    $filteredTrials = @($filteredMetrics | Where-Object { $_.Level -eq 'Trial' })
    Assert-Equal 2 $filteredTrials.Count 'Short moving fragments must be excluded from scenario metrics.'

    $defaultThresholdMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -PassThru)
    $defaultReverseTrial = @($defaultThresholdMetrics | Where-Object { $_.Level -eq 'Trial' -and $_.Scenario -eq 'horizontal_reverse' })[0]
    Assert-Equal 1 $defaultReverseTrial.ReversalCount 'Default 10 px reversal threshold must detect moderate high-frequency swings.'

    $zeroMotionRoot = Join-Path $temporaryRoot 'zero-motion-root'
    $zeroMotionDirectory = Join-Path $zeroMotionRoot 'DML\ndi'
    New-Item -ItemType Directory -Path $zeroMotionDirectory -Force | Out-Null
    $zeroMotionPath = Join-Path $zeroMotionDirectory 'static.csv'
    $zeroMotionRows = @(Import-Csv -LiteralPath $csvPath)
    foreach ($row in $zeroMotionRows) {
        $row.RequestedPixelX = 0
        $row.RequestedCountsX = 0
        $row.FinalMx = 0
    }
    $zeroMotionRows | Export-Csv -LiteralPath $zeroMotionPath -NoTypeInformation -Encoding UTF8
    $zeroMotionMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $zeroMotionRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -PassThru)
    $zeroMotionTrial = @($zeroMotionMetrics | Where-Object { $_.Level -eq 'Trial' })[0]
    Assert-Equal 0 $zeroMotionTrial.EstimatedCountsPerPixel 'A valid zero-motion axis must report unavailable counts-per-pixel as zero.'
    Assert-Equal 0 $zeroMotionTrial.ApproxClosedLoopLagMs 'A valid zero-motion axis must not fabricate closed-loop lag.'

    $legacyRows = @(Import-Csv -LiteralPath $csvPath)
    foreach ($row in $legacyRows) {
        foreach ($propertyName in @('RawPivotX', 'RawPivotY', 'TargetBoxX', 'TargetBoxY',
                                    'TargetBoxWidth', 'TargetBoxHeight', 'PredictedX', 'PredictedY')) {
            $row.PSObject.Properties.Remove($propertyName)
        }
    }
    $legacyPath = Join-Path $zeroMotionDirectory 'legacy.csv'
    $legacyRows | Export-Csv -LiteralPath $legacyPath -NoTypeInformation -Encoding UTF8
    $legacyMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $zeroMotionRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -PassThru)
    $legacyTrial = @($legacyMetrics | Where-Object { $_.Level -eq 'Trial' -and $_.Scenario -eq 'legacy' })[0]
    Assert-Equal 0 $legacyTrial.ObservedTrackingAvailable 'Legacy CSV must remain analyzable without raw target diagnostics.'
    Assert-Equal 0 $legacyTrial.ObservedP95AbsAxisErrorPx 'Unavailable observed metrics must use zero plus an explicit availability flag.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] Moving-target analysis tests passed.' -ForegroundColor Green
