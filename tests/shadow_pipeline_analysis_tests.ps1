[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param([object]$Expected, [object]$Actual, [string]$Message)
    if ([string]$Expected -ne [string]$Actual) {
        throw "$Message Expected='$Expected', Actual='$Actual'."
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('xen-shadow-analysis-' + [guid]::NewGuid().ToString('N'))
$header = 'BuildBackend,BuildRevision,ControllerRevision,AimPipelineRequestedMode,AimPipelineEffectiveMode,AimPipelineActiveAvailable,AimPipelineShadowProcessed,AimPipelineCommandSuppressed,AimPipelineOutputPaused,AimPipelineResetGeneration,AimPipelineObservationSequence,AimPipelineTargetId,AimPipelineEstimateValid,AimPipelineCovarianceX,AimPipelineCovarianceY,AimPipelineInnovationVarianceX,AimPipelineInnovationVarianceY,AimPipelineNisX,AimPipelineNisY,AimPipelineTrackingFeedforwardX,AimPipelineTrackingFeedforwardY,AimPipelineLeadCountsX,AimPipelineLeadCountsY,AimPipelineIntegralCountsX,AimPipelineIntegralCountsY,TrajectoryShaperMode,AimPipelineTrajectoryCommandSuppressed,TimingComplete,TimingOrderValid,FinalMx,FinalMy,CommandEnqueueSucceeded,CommandSendAttempted,CommandSendSucceeded,CommandRequestedCountsX,CommandRequestedCountsY,CommandAppliedCountsX,CommandAppliedCountsY,QueuedMoveCount,ControlTimeNs'
$activeRow = 'DML,test-revision,59,shadow,shadow,0,1,1,0,4,1,7,1,0.02,0.03,0.04,0.05,0.2,0.3,0,0,0,0,0,0,off,1,1,1,0,0,0,0,0,0,0,0,0,0,0'
$pausedRow = 'DML,test-revision,59,shadow,shadow,0,1,1,1,4,2,7,1,0.02,0.03,0.04,0.05,0.2,0.3,0,0,0,0,0,0,off,1,1,1,0,0,0,0,0,0,0,0,0,0,100000000'
$resumedRow = 'DML,test-revision,59,shadow,shadow,0,1,1,0,4,3,7,1,0.02,0.03,0.04,0.05,0.2,0.3,0,0,0,0,0,0,off,1,1,1,0,0,0,0,0,0,0,0,0,0,200000000'
$secondPausedRow = $pausedRow -replace ',4,2,7,', ',4,4,7,' -replace ',100000000$', ',300000000'
$secondResumedRow = $resumedRow -replace ',4,3,7,', ',4,5,7,' -replace ',200000000$', ',800000000'
try {
    $validRoot = Join-Path $temporaryRoot 'valid'
    New-Item -ItemType Directory -Path $validRoot | Out-Null
    foreach ($scenario in @('static', 'horizontal_left', 'horizontal_right', 'horizontal_reverse', 'horizontal_jump')) {
        $scenarioRows = if ($scenario -match 'left|right') {
            @($header, $activeRow, $pausedRow, $resumedRow, $secondPausedRow, $secondResumedRow)
        }
        else { @($header, $activeRow, $pausedRow, $resumedRow) }
        $scenarioRows | Set-Content `
            -LiteralPath (Join-Path $validRoot "$scenario.csv") -Encoding UTF8
    }
    $outputCsv = Join-Path $validRoot 'shadow_pipeline_summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
        -DataRoot $validRoot -MinEstimateSamples 3 -ExpectedBuildRevision test-revision `
        -ExpectedControllerRevision 59 -RequirePausedObservations -MinPausedObservations 5 `
        -OutputCsv $outputCsv -RequireStandardScenarios -RequirePauseScenarioCoverage `
        -MinShortPauseEventsPerDirection 1 -MinLongPauseEventsPerDirection 1 -PassThru)
    Assert-Equal 6 $metrics.Count 'Five scenarios and one overall shadow summary must be emitted.'
    Assert-Equal PASS $metrics[-1].Status 'Valid shadow data must pass.'
    Assert-Equal 19 $metrics[-1].EstimateSamples 'Estimate samples must be counted.'
    Assert-Equal 7 $metrics[-1].PausedObservations 'Paused observations must be counted.'
    Assert-Equal 5 $metrics[-1].ShortPauseEvents 'Short pause events must be counted.'
    Assert-Equal 2 $metrics[-1].LongPauseEvents 'Long pause events must be counted.'
    Assert-Equal 0 $metrics[-1].PausedCommandViolations 'Paused rows must not reach device commands.'
    Assert-Equal 0 $metrics[-1].PauseContinuityViolations 'Pause boundaries must keep observation sequence continuous.'

    $candidateRoot = Join-Path $temporaryRoot 'maneuver-candidate'
    New-Item -ItemType Directory -Path $candidateRoot | Out-Null
    $candidateHeader = $header + ',AimPipelineEstimatorMode,AimPipelineManeuverModelActive,AimPipelineEstimatorSelectionChanged,AimPipelineEstimatorSelectionCount,AimPipelineCaJerkStdDps3,AimPipelineManeuverRateThresholdDps,AimPipelineManeuverHoldMs,AimPipelineManeuverHoldRemainingMs,AimPipelineManeuverRateUncertaintyX,AimPipelineManeuverRateUncertaintyY,AimPipelineManeuverRateEvidenceDps,AimPipelineModelAngleDeltaDeg,AimPipelineModelRateDeltaDps,AimPipelineBaselineCovarianceX,AimPipelineBaselineCovarianceY,AimPipelineCaCovarianceX,AimPipelineCaCovarianceY,ViewMotionShadowValid,CommandToFrameDelayMs,CommandResponseMs,ManeuverRateUncertaintyGain,AppliedCameraRateYawDps,AppliedCameraRatePitchDps,ViewMotionManeuverRateUncertaintyX,ViewMotionManeuverRateUncertaintyY'
    $inactiveSuffix = ',maneuver_gated_ca,0,0,0,8000,12,120,0,0,0,0,0.1,1,0.02,0.03,0.02,0.03,1,20,20,1.25,0,0,0,0'
    $activationSuffix = ',maneuver_gated_ca,1,1,1,8000,12,120,120,2.5,0,12.3,0.1,1,0.02,0.03,0.02,0.03,1,20,20,1.25,2,0,2.5,0'
    $activeSuffix = ',maneuver_gated_ca,1,0,1,8000,12,120,120,2.5,0,12.3,0.1,1,0.02,0.03,0.02,0.03,1,20,20,1.25,2,0,2.5,0'
    $deactivationSuffix = ',maneuver_gated_ca,0,1,2,8000,12,120,0,0,0,2,0.1,1,0.02,0.03,0.02,0.03,1,20,20,1.25,0,0,0,0'
    foreach ($scenario in @('static', 'horizontal_left', 'horizontal_right', 'horizontal_reverse', 'horizontal_jump')) {
        $moving = $scenario -match 'reverse|jump'
        @(
            $candidateHeader,
            ($activeRow + $inactiveSuffix),
            ($pausedRow + $(if ($moving) { $activationSuffix } else { $inactiveSuffix })),
            ($resumedRow + $(if ($moving) { $activeSuffix } else { $inactiveSuffix }))
        ) | Set-Content -LiteralPath (Join-Path $candidateRoot "$scenario.csv") -Encoding UTF8
    }
    $candidateMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
        -DataRoot $candidateRoot -MinEstimateSamples 3 -ExpectedBuildRevision test-revision `
        -ExpectedControllerRevision 59 -RequireStandardScenarios -RequireManeuverCandidate `
        -RequireFiniteViewResponse -PassThru)
    Assert-Equal PASS $candidateMetrics[-1].Status 'Valid maneuver shadow data must pass.'
    Assert-Equal 4 $candidateMetrics[-1].ManeuverActiveSamples `
        'Jump and reverse maneuver activation samples must be counted.'
    Assert-Equal 2 $candidateMetrics[-1].ManeuverPausedActiveSamples `
        'Paused maneuver activation must be reported separately.'
    Assert-Equal 2 $candidateMetrics[-1].ManeuverRunningActiveSamples `
        'Running maneuver activation must be reported separately.'
    Assert-Equal 0 $candidateMetrics[-1].ViewResponseContractViolations `
        'The frozen 20/20 finite response must pass validation.'

    @($candidateHeader, ($activeRow + $inactiveSuffix),
        ($pausedRow + $activationSuffix), ($resumedRow + $deactivationSuffix)) |
        Set-Content -LiteralPath (Join-Path $candidateRoot 'static.csv') -Encoding UTF8
    $pausedStaticMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
        -DataRoot $candidateRoot -MinEstimateSamples 3 -ExpectedBuildRevision test-revision `
        -ExpectedControllerRevision 59 -RequireStandardScenarios -RequireManeuverCandidate -PassThru)
    $staticMetric = @($pausedStaticMetrics | Where-Object Source -eq 'static.csv')[0]
    Assert-Equal PASS $pausedStaticMetrics[-1].Status `
        'Paused static repositioning must not fail the running candidate gate.'
    Assert-Equal 1 $staticMetric.ManeuverPausedActiveSamples `
        'Paused static activation must remain visible in diagnostics.'
    Assert-Equal 0 $staticMetric.ManeuverRunningActiveSamples `
        'Paused static activation must not be misreported as running residency.'

    $unsafeStaticRow = $pausedRow + $activationSuffix
    @($candidateHeader, ($activeRow + $inactiveSuffix), $unsafeStaticRow,
        ($resumedRow + $activeSuffix)) |
        Set-Content -LiteralPath (Join-Path $candidateRoot 'static.csv') -Encoding UTF8
    $staticActivationFailed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $candidateRoot -MinEstimateSamples 3 -ExpectedBuildRevision test-revision `
            -ExpectedControllerRevision 59 -RequireStandardScenarios -RequireManeuverCandidate | Out-Null
    }
    catch { $staticActivationFailed = $true }
    Assert-Equal True $staticActivationFailed 'Running static maneuver-model residency must fail validation.'

    $invalidResponseRoot = Join-Path $temporaryRoot 'invalid-response'
    New-Item -ItemType Directory -Path $invalidResponseRoot | Out-Null
    @($candidateHeader,
        (($activeRow + $inactiveSuffix) -replace ',1,20,20,1.25,0,0,0,0$', ',1,20,0,1.25,0,0,0,0'),
        (($pausedRow + $inactiveSuffix) -replace ',1,20,20,1.25,0,0,0,0$', ',1,20,0,1.25,0,0,0,0'),
        (($resumedRow + $inactiveSuffix) -replace ',1,20,20,1.25,0,0,0,0$', ',1,20,0,1.25,0,0,0,0')) |
        Set-Content -LiteralPath (Join-Path $invalidResponseRoot 'profile.csv') -Encoding UTF8
    $invalidResponseFailed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $invalidResponseRoot -MinEstimateSamples 3 `
            -ExpectedBuildRevision test-revision -ExpectedControllerRevision 59 `
            -RequireFiniteViewResponse | Out-Null
    }
    catch { $invalidResponseFailed = $true }
    Assert-Equal True $invalidResponseFailed `
        'A step response must fail the frozen finite-response contract.'

    $nonstandardRoot = Join-Path $temporaryRoot 'nonstandard-name'
    New-Item -ItemType Directory -Path $nonstandardRoot | Out-Null
    @($header, $activeRow, $pausedRow, $resumedRow) |
        Set-Content -LiteralPath (Join-Path $nonstandardRoot 'profile_calibration.csv') -Encoding UTF8
    $nonstandardMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
        -DataRoot $nonstandardRoot -MinEstimateSamples 3 `
        -ExpectedBuildRevision test-revision -ExpectedControllerRevision 59 `
        -RequirePausedObservations -PassThru)
    Assert-Equal PASS $nonstandardMetrics[-1].Status `
        'A nonstandard profile filename must not trigger empty scenario aggregation errors.'

    $coverageFailed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $validRoot -MinEstimateSamples 3 -ExpectedBuildRevision test-revision `
            -ExpectedControllerRevision 59 -RequirePauseScenarioCoverage `
            -MinShortPauseEventsPerDirection 1 -MinLongPauseEventsPerDirection 2 | Out-Null
    }
    catch { $coverageFailed = $true }
    Assert-Equal True $coverageFailed 'Each direction must independently meet short and long pause event coverage.'

    $invalidRoot = Join-Path $temporaryRoot 'invalid'
    New-Item -ItemType Directory -Path $invalidRoot | Out-Null
    $invalidRow = $pausedRow -replace ',shadow,shadow,0,1,1,1,', ',active,shadow,0,1,0,1,'
    $invalidRow = $invalidRow -replace ',0,0,0,0,0,0,0,0,0,0$', ',1,0,1,1,0,5,0,0,0,1'
    @($header, $invalidRow) | Set-Content -LiteralPath (Join-Path $invalidRoot 'unsafe.csv') -Encoding UTF8
    $failed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $invalidRoot -MinEstimateSamples 1 -ExpectedBuildRevision test-revision `
            -ExpectedControllerRevision 59 -RequirePausedObservations | Out-Null
    }
    catch { $failed = $true }
    Assert-Equal True $failed 'Unsafe shadow mode contract must fail validation.'

    $unsafePauseRoot = Join-Path $temporaryRoot 'unsafe-pause'
    New-Item -ItemType Directory -Path $unsafePauseRoot | Out-Null
    $unsafePausedRow = $pausedRow -replace `
        ',0,0,0,0,0,0,0,0,0,0,100000000$', `
        ',1,0,1,1,0,5,0,0,0,1,100000000'
    @($header, $activeRow, $unsafePausedRow, $resumedRow) | Set-Content `
        -LiteralPath (Join-Path $unsafePauseRoot 'left.csv') -Encoding UTF8
    $unsafePauseFailed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $unsafePauseRoot -MinEstimateSamples 3 `
            -ExpectedBuildRevision test-revision -ExpectedControllerRevision 59 `
            -RequirePausedObservations | Out-Null
    }
    catch { $unsafePauseFailed = $true }
    Assert-Equal True $unsafePauseFailed 'Paused rows that reach command lifecycle fields must fail.'

    $missingPauseRoot = Join-Path $temporaryRoot 'missing-pause'
    New-Item -ItemType Directory -Path $missingPauseRoot | Out-Null
    @($header, $activeRow) | Set-Content -LiteralPath (Join-Path $missingPauseRoot 'left.csv') -Encoding UTF8
    $missingPauseFailed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $missingPauseRoot -MinEstimateSamples 1 `
            -ExpectedBuildRevision test-revision -ExpectedControllerRevision 59 `
            -RequirePausedObservations | Out-Null
    }
    catch { $missingPauseFailed = $true }
    Assert-Equal True $missingPauseFailed 'Required paused observations cannot be omitted.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] P0-6 shadow pipeline analysis tests passed.' -ForegroundColor Green
