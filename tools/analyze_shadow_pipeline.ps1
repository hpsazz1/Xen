[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [ValidateRange(1, 1000000)][int]$MinEstimateSamples = 30,
    [ValidateRange(0, 1000000)][int]$MinPausedObservations = 1,
    [string]$ExpectedBuildRevision = '',
    [ValidateRange(0, 1000000)][int]$ExpectedControllerRevision = 0,
    [ValidateRange(1, 1000)][int]$MinShortPauseEventsPerDirection = 6,
    [ValidateRange(1, 1000)][int]$MinLongPauseEventsPerDirection = 6,
    [ValidateRange(1.0, 10000.0)][double]$ShortPauseMaxMs = 350.0,
    [ValidateRange(1.0, 60000.0)][double]$LongPauseMaxMs = 2000.0,
    [string]$OutputCsv = '',
    [switch]$RequireStandardScenarios,
    [switch]$RequirePausedObservations,
    [switch]$RequirePauseScenarioCoverage,
    [switch]$RequireManeuverCandidate,
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-FiniteNumber {
    param([object]$Value, [switch]$Positive, [switch]$NonNegative)
    try {
        $number = [Convert]::ToDouble($Value, [Globalization.CultureInfo]::InvariantCulture)
    }
    catch { return $false }
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $false }
    if ($Positive -and $number -le 0.0) { return $false }
    if ($NonNegative -and $number -lt 0.0) { return $false }
    return $true
}

function Test-NearZero {
    param([object]$Value)
    if (-not (Test-FiniteNumber $Value)) { return $false }
    return [math]::Abs([Convert]::ToDouble($Value, [Globalization.CultureInfo]::InvariantCulture)) -le 1e-9
}

function Test-NearValue {
    param([object]$Value, [double]$Expected, [double]$Tolerance = 1e-6)
    if (-not (Test-FiniteNumber $Value)) { return $false }
    $number = [Convert]::ToDouble($Value, [Globalization.CultureInfo]::InvariantCulture)
    return [math]::Abs($number - $Expected) -le $Tolerance
}

$resolvedRoot = [IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "Shadow data root does not exist: $resolvedRoot"
}

$requiredColumns = @(
    'BuildBackend', 'BuildRevision', 'ControllerRevision',
    'AimPipelineRequestedMode', 'AimPipelineEffectiveMode',
    'AimPipelineActiveAvailable', 'AimPipelineShadowProcessed',
    'AimPipelineCommandSuppressed', 'AimPipelineOutputPaused',
    'AimPipelineResetGeneration', 'AimPipelineObservationSequence',
    'AimPipelineTargetId', 'AimPipelineEstimateValid',
    'AimPipelineCovarianceX', 'AimPipelineCovarianceY',
    'AimPipelineInnovationVarianceX', 'AimPipelineInnovationVarianceY',
    'AimPipelineNisX', 'AimPipelineNisY',
    'AimPipelineTrackingFeedforwardX', 'AimPipelineTrackingFeedforwardY',
    'AimPipelineLeadCountsX', 'AimPipelineLeadCountsY',
    'AimPipelineIntegralCountsX', 'AimPipelineIntegralCountsY',
    'TrajectoryShaperMode', 'AimPipelineTrajectoryCommandSuppressed',
    'TimingComplete', 'TimingOrderValid',
    'FinalMx', 'FinalMy', 'CommandEnqueueSucceeded',
    'CommandSendAttempted', 'CommandSendSucceeded',
    'CommandRequestedCountsX', 'CommandRequestedCountsY',
    'CommandAppliedCountsX', 'CommandAppliedCountsY', 'QueuedMoveCount'
)
if ($RequirePauseScenarioCoverage) { $requiredColumns += 'ControlTimeNs' }
if ($RequireManeuverCandidate) {
    $requiredColumns += @(
        'AimPipelineEstimatorMode', 'AimPipelineManeuverModelActive',
        'AimPipelineEstimatorSelectionChanged', 'AimPipelineEstimatorSelectionCount',
        'AimPipelineCaJerkStdDps3', 'AimPipelineManeuverRateThresholdDps',
        'AimPipelineManeuverHoldMs', 'AimPipelineManeuverHoldRemainingMs',
        'AimPipelineModelAngleDeltaDeg', 'AimPipelineModelRateDeltaDps',
        'AimPipelineBaselineCovarianceX', 'AimPipelineBaselineCovarianceY',
        'AimPipelineCaCovarianceX', 'AimPipelineCaCovarianceY'
    )
}
if ($LongPauseMaxMs -le $ShortPauseMaxMs) {
    throw 'LongPauseMaxMs must be greater than ShortPauseMaxMs.'
}

$summaries = [Collections.Generic.List[object]]::new()
$skipped = 0
foreach ($csvFile in @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Filter '*.csv' | Sort-Object FullName)) {
    if ($csvFile.Name -eq 'shadow_pipeline_summary.csv') { continue }
    $rows = @(Import-Csv -LiteralPath $csvFile.FullName)
    if ($rows.Count -eq 0) { continue }
    $missing = @($requiredColumns | Where-Object { $rows[0].PSObject.Properties.Name -notcontains $_ })
    if ($missing.Count -gt 0) {
        Write-Warning "Skipping CSV without shadow columns [$($missing -join ', ')]: $($csvFile.FullName)"
        ++$skipped
        continue
    }

    $estimateRows = @($rows | Where-Object AimPipelineEstimateValid -eq '1')
    $modeViolations = @($rows | Where-Object {
        $_.AimPipelineRequestedMode -ne 'shadow' -or
        $_.AimPipelineEffectiveMode -ne 'shadow' -or
        $_.AimPipelineActiveAvailable -ne '0' -or
        $_.AimPipelineShadowProcessed -ne '1' -or
        $_.AimPipelineCommandSuppressed -ne '1' -or
        $_.AimPipelineTrajectoryCommandSuppressed -ne '1'
    }).Count
    $identityViolations = @($rows | Where-Object {
        $_.BuildBackend -ne 'DML' -or $_.BuildRevision -match 'dirty' -or
        ($ExpectedBuildRevision -ne '' -and $_.BuildRevision -ne $ExpectedBuildRevision) -or
        ($ExpectedControllerRevision -gt 0 -and
            [int]$_.ControllerRevision -ne $ExpectedControllerRevision)
    }).Count
    $timingViolations = @($rows | Where-Object {
        $_.TimingComplete -eq '1' -and $_.TimingOrderValid -ne '1'
    }).Count
    $diagnosticViolations = @($estimateRows | Where-Object {
        -not (Test-FiniteNumber $_.AimPipelineCovarianceX -Positive) -or
        -not (Test-FiniteNumber $_.AimPipelineCovarianceY -Positive) -or
        -not (Test-FiniteNumber $_.AimPipelineInnovationVarianceX -Positive) -or
        -not (Test-FiniteNumber $_.AimPipelineInnovationVarianceY -Positive) -or
        -not (Test-FiniteNumber $_.AimPipelineNisX -NonNegative) -or
        -not (Test-FiniteNumber $_.AimPipelineNisY -NonNegative)
    }).Count
    $stageOneViolations = @($rows | Where-Object {
        $_.TrajectoryShaperMode -ne 'off' -or
        -not (Test-NearZero $_.AimPipelineTrackingFeedforwardX) -or
        -not (Test-NearZero $_.AimPipelineTrackingFeedforwardY) -or
        -not (Test-NearZero $_.AimPipelineLeadCountsX) -or
        -not (Test-NearZero $_.AimPipelineLeadCountsY) -or
        -not (Test-NearZero $_.AimPipelineIntegralCountsX) -or
        -not (Test-NearZero $_.AimPipelineIntegralCountsY)
    }).Count
    $maneuverActiveRows = @()
    $maneuverContractViolations = 0
    $maneuverSelectionViolations = 0
    if ($RequireManeuverCandidate) {
        $maneuverActiveRows = @($estimateRows | Where-Object AimPipelineManeuverModelActive -eq '1')
        $maneuverContractViolations = @($estimateRows | Where-Object {
            $_.AimPipelineEstimatorMode -ne 'maneuver_gated_ca' -or
            -not (Test-NearValue $_.AimPipelineCaJerkStdDps3 8000.0 0.01) -or
            -not (Test-NearValue $_.AimPipelineManeuverRateThresholdDps 12.0 0.001) -or
            -not (Test-NearValue $_.AimPipelineManeuverHoldMs 120.0 0.001) -or
            -not (Test-FiniteNumber $_.AimPipelineManeuverHoldRemainingMs -NonNegative) -or
            -not (Test-FiniteNumber $_.AimPipelineModelAngleDeltaDeg -NonNegative) -or
            -not (Test-FiniteNumber $_.AimPipelineModelRateDeltaDps -NonNegative) -or
            -not (Test-FiniteNumber $_.AimPipelineBaselineCovarianceX -Positive) -or
            -not (Test-FiniteNumber $_.AimPipelineBaselineCovarianceY -Positive) -or
            -not (Test-FiniteNumber $_.AimPipelineCaCovarianceX -Positive) -or
            -not (Test-FiniteNumber $_.AimPipelineCaCovarianceY -Positive)
        }).Count
        if ($csvFile.BaseName.ToLowerInvariant() -match 'static' -and
            $maneuverActiveRows.Count -gt 0) {
            $maneuverContractViolations += $maneuverActiveRows.Count
        }
        for ($candidateIndex = 1; $candidateIndex -lt $estimateRows.Count; ++$candidateIndex) {
            $previousCandidate = $estimateRows[$candidateIndex - 1]
            $currentCandidate = $estimateRows[$candidateIndex]
            $sameCandidateGeneration = $previousCandidate.AimPipelineResetGeneration -eq
                $currentCandidate.AimPipelineResetGeneration
            $sameCandidateTarget = $previousCandidate.AimPipelineTargetId -eq
                $currentCandidate.AimPipelineTargetId
            if (-not $sameCandidateGeneration -or -not $sameCandidateTarget) { continue }
            $countDelta = [uint64]$currentCandidate.AimPipelineEstimatorSelectionCount -
                [uint64]$previousCandidate.AimPipelineEstimatorSelectionCount
            $reportedChange = $currentCandidate.AimPipelineEstimatorSelectionChanged -eq '1'
            if (($reportedChange -and $countDelta -ne 1) -or
                (-not $reportedChange -and $countDelta -ne 0)) {
                ++$maneuverSelectionViolations
            }
        }
    }
    $pausedRows = @($rows | Where-Object AimPipelineOutputPaused -eq '1')
    $pausedCommandViolations = @($pausedRows | Where-Object {
        -not (Test-NearZero $_.FinalMx) -or -not (Test-NearZero $_.FinalMy) -or
        $_.CommandEnqueueSucceeded -ne '0' -or
        $_.CommandSendAttempted -ne '0' -or $_.CommandSendSucceeded -ne '0' -or
        -not (Test-NearZero $_.CommandRequestedCountsX) -or
        -not (Test-NearZero $_.CommandRequestedCountsY) -or
        -not (Test-NearZero $_.CommandAppliedCountsX) -or
        -not (Test-NearZero $_.CommandAppliedCountsY) -or
        -not (Test-NearZero $_.QueuedMoveCount)
    }).Count
    $pauseContinuityViolations = 0
    $shortPauseEvents = 0
    $longPauseEvents = 0
    $pauseDurationViolations = 0
    $hasControlTime = $rows[0].PSObject.Properties.Name -contains 'ControlTimeNs'
    for ($index = 1; $index -lt $rows.Count; ++$index) {
        $previous = $rows[$index - 1]
        $current = $rows[$index]
        $touchesPause = $previous.AimPipelineOutputPaused -eq '1' -or
            $current.AimPipelineOutputPaused -eq '1'
        $sameGeneration = $previous.AimPipelineResetGeneration -eq
            $current.AimPipelineResetGeneration
        $sameTarget = $previous.AimPipelineTargetId -eq $current.AimPipelineTargetId
        if ($touchesPause -and $sameGeneration -and $sameTarget -and
            ([uint64]$current.AimPipelineObservationSequence -ne
             [uint64]$previous.AimPipelineObservationSequence + 1)) {
            ++$pauseContinuityViolations
        }
        if ($hasControlTime -and $previous.AimPipelineOutputPaused -eq '1' -and
            $current.AimPipelineOutputPaused -eq '0' -and $sameGeneration -and $sameTarget) {
            $pauseStartIndex = $index - 1
            while ($pauseStartIndex -gt 0) {
                $candidate = $rows[$pauseStartIndex - 1]
                if ($candidate.AimPipelineOutputPaused -ne '1' -or
                    $candidate.AimPipelineResetGeneration -ne $current.AimPipelineResetGeneration -or
                    $candidate.AimPipelineTargetId -ne $current.AimPipelineTargetId) { break }
                --$pauseStartIndex
            }
            $pauseStart = $rows[$pauseStartIndex]
            if (-not (Test-FiniteNumber $pauseStart.ControlTimeNs -NonNegative) -or
                -not (Test-FiniteNumber $current.ControlTimeNs -NonNegative)) {
                ++$pauseDurationViolations
                continue
            }
            $pauseDurationMs = (
                [Convert]::ToDouble($current.ControlTimeNs, [Globalization.CultureInfo]::InvariantCulture) -
                [Convert]::ToDouble($pauseStart.ControlTimeNs, [Globalization.CultureInfo]::InvariantCulture)
            ) / 1000000.0
            if ($pauseDurationMs -lt 0.0) {
                ++$pauseDurationViolations
            }
            elseif ($pauseDurationMs -lt $ShortPauseMaxMs) {
                ++$shortPauseEvents
            }
            elseif ($pauseDurationMs -le $LongPauseMaxMs) {
                ++$longPauseEvents
            }
        }
    }
    $identityCount = @($rows | ForEach-Object {
        "$($_.BuildBackend)|$($_.BuildRevision)|$($_.ControllerRevision)"
    } | Select-Object -Unique).Count
    if ($identityCount -ne 1) { $identityViolations += $rows.Count }

    $reasons = [Collections.Generic.List[string]]::new()
    if ($estimateRows.Count -lt $MinEstimateSamples) { $reasons.Add('insufficient estimate samples') }
    if ($modeViolations -gt 0) { $reasons.Add('shadow mode contract violated') }
    if ($identityViolations -gt 0) { $reasons.Add('build identity violated') }
    if ($timingViolations -gt 0) { $reasons.Add('timing order violated') }
    if ($diagnosticViolations -gt 0) { $reasons.Add('Kalman diagnostics invalid') }
    if ($stageOneViolations -gt 0) { $reasons.Add('later-stage controller component enabled') }
    if ($pausedCommandViolations -gt 0) { $reasons.Add('paused device command contract violated') }
    if ($pauseContinuityViolations -gt 0) { $reasons.Add('paused observation sequence interrupted') }
    if ($pauseDurationViolations -gt 0) { $reasons.Add('paused duration timestamp invalid') }
    if ($maneuverContractViolations -gt 0) { $reasons.Add('maneuver estimator contract violated') }
    if ($maneuverSelectionViolations -gt 0) { $reasons.Add('maneuver model selection sequence violated') }
    $status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL' }
    $summaries.Add([pscustomobject]@{
        Level = 'File'
        Source = $csvFile.Name
        Rows = $rows.Count
        EstimateSamples = $estimateRows.Count
        BuildRevision = (@($rows.BuildRevision | Select-Object -Unique) -join ';')
        ModeViolations = $modeViolations
        IdentityViolations = $identityViolations
        TimingViolations = $timingViolations
        DiagnosticViolations = $diagnosticViolations
        StageOneViolations = $stageOneViolations
        PausedObservations = $pausedRows.Count
        PausedCommandViolations = $pausedCommandViolations
        PauseContinuityViolations = $pauseContinuityViolations
        ShortPauseEvents = $shortPauseEvents
        LongPauseEvents = $longPauseEvents
        PauseDurationViolations = $pauseDurationViolations
        ManeuverActiveSamples = $maneuverActiveRows.Count
        ManeuverContractViolations = $maneuverContractViolations
        ManeuverSelectionViolations = $maneuverSelectionViolations
        Status = $status
        Reason = ($reasons -join '; ')
    })
}

if ($summaries.Count -eq 0) {
    throw "No P0-6 shadow CSV was found under: $resolvedRoot (skipped=$skipped)"
}
$failed = @($summaries | Where-Object Status -eq 'FAIL')
$missingScenarios = [Collections.Generic.List[string]]::new()
if ($RequireStandardScenarios) {
    $sources = (@($summaries.Source) -join ' ').ToLowerInvariant()
    foreach ($scenario in @('static', 'left', 'right', 'reverse', 'jump')) {
        if ($sources -notmatch [regex]::Escape($scenario)) { $missingScenarios.Add($scenario) }
    }
}
$buildRevisionCount = @($summaries.BuildRevision | Select-Object -Unique).Count
$pausedObservationCount = [int](($summaries | Measure-Object PausedObservations -Sum).Sum)
$pausedObservationMissing = $RequirePausedObservations -and
    $pausedObservationCount -lt $MinPausedObservations
$leftSummaries = @($summaries | Where-Object { $_.Source.ToLowerInvariant() -match 'left' })
$rightSummaries = @($summaries | Where-Object { $_.Source.ToLowerInvariant() -match 'right' })
$leftShortPauseEvents = [int](($leftSummaries | Measure-Object ShortPauseEvents -Sum).Sum)
$leftLongPauseEvents = [int](($leftSummaries | Measure-Object LongPauseEvents -Sum).Sum)
$rightShortPauseEvents = [int](($rightSummaries | Measure-Object ShortPauseEvents -Sum).Sum)
$rightLongPauseEvents = [int](($rightSummaries | Measure-Object LongPauseEvents -Sum).Sum)
$pauseCoverageMissing = $RequirePauseScenarioCoverage -and (
    $leftShortPauseEvents -lt $MinShortPauseEventsPerDirection -or
    $leftLongPauseEvents -lt $MinLongPauseEventsPerDirection -or
    $rightShortPauseEvents -lt $MinShortPauseEventsPerDirection -or
    $rightLongPauseEvents -lt $MinLongPauseEventsPerDirection)
$staticManeuverSamples = [int](($summaries | Where-Object {
    $_.Source.ToLowerInvariant() -match 'static'
} | Measure-Object ManeuverActiveSamples -Sum).Sum)
$jumpManeuverSamples = [int](($summaries | Where-Object {
    $_.Source.ToLowerInvariant() -match 'jump'
} | Measure-Object ManeuverActiveSamples -Sum).Sum)
$reverseManeuverSamples = [int](($summaries | Where-Object {
    $_.Source.ToLowerInvariant() -match 'reverse'
} | Measure-Object ManeuverActiveSamples -Sum).Sum)
$maneuverCoverageMissing = $RequireManeuverCandidate -and (
    $staticManeuverSamples -ne 0 -or $jumpManeuverSamples -eq 0 -or
    $reverseManeuverSamples -eq 0)
$overallFailed = $failed.Count -gt 0 -or $missingScenarios.Count -gt 0 -or
    $buildRevisionCount -ne 1 -or $pausedObservationMissing -or $pauseCoverageMissing -or
    $maneuverCoverageMissing
$overallReasons = [Collections.Generic.List[string]]::new()
if ($failed.Count -gt 0) { $overallReasons.Add("$($failed.Count) file(s) failed") }
if ($missingScenarios.Count -gt 0) { $overallReasons.Add("missing scenarios: $($missingScenarios -join ',')") }
if ($buildRevisionCount -ne 1) { $overallReasons.Add('multiple build revisions') }
if ($pausedObservationMissing) {
    $overallReasons.Add("paused observations below minimum: $pausedObservationCount/$MinPausedObservations")
}
if ($pauseCoverageMissing) {
    $overallReasons.Add(
        "directional pause coverage below minimum: " +
        "left(short=$leftShortPauseEvents,long=$leftLongPauseEvents), " +
        "right(short=$rightShortPauseEvents,long=$rightLongPauseEvents), " +
        "required(short=$MinShortPauseEventsPerDirection,long=$MinLongPauseEventsPerDirection)")
}
if ($maneuverCoverageMissing) {
    $overallReasons.Add(
        "maneuver coverage violated: static=$staticManeuverSamples, " +
        "jump=$jumpManeuverSamples, reverse=$reverseManeuverSamples")
}
$overall = [pscustomobject]@{
    Level = 'Overall'
    Source = 'all'
    Rows = [int](($summaries | Measure-Object Rows -Sum).Sum)
    EstimateSamples = [int](($summaries | Measure-Object EstimateSamples -Sum).Sum)
    BuildRevision = (@($summaries.BuildRevision | Select-Object -Unique) -join ';')
    ModeViolations = [int](($summaries | Measure-Object ModeViolations -Sum).Sum)
    IdentityViolations = [int](($summaries | Measure-Object IdentityViolations -Sum).Sum)
    TimingViolations = [int](($summaries | Measure-Object TimingViolations -Sum).Sum)
    DiagnosticViolations = [int](($summaries | Measure-Object DiagnosticViolations -Sum).Sum)
    StageOneViolations = [int](($summaries | Measure-Object StageOneViolations -Sum).Sum)
    PausedObservations = $pausedObservationCount
    PausedCommandViolations = [int](($summaries | Measure-Object PausedCommandViolations -Sum).Sum)
    PauseContinuityViolations = [int](($summaries | Measure-Object PauseContinuityViolations -Sum).Sum)
    ShortPauseEvents = [int](($summaries | Measure-Object ShortPauseEvents -Sum).Sum)
    LongPauseEvents = [int](($summaries | Measure-Object LongPauseEvents -Sum).Sum)
    PauseDurationViolations = [int](($summaries | Measure-Object PauseDurationViolations -Sum).Sum)
    ManeuverActiveSamples = [int](($summaries | Measure-Object ManeuverActiveSamples -Sum).Sum)
    ManeuverContractViolations = [int](($summaries | Measure-Object ManeuverContractViolations -Sum).Sum)
    ManeuverSelectionViolations = [int](($summaries | Measure-Object ManeuverSelectionViolations -Sum).Sum)
    Status = if ($overallFailed) { 'FAIL' } else { 'PASS' }
    Reason = ($overallReasons -join '; ')
}
$result = @($summaries) + @($overall)
if ($OutputCsv -ne '') {
    $outputPath = [IO.Path]::GetFullPath($OutputCsv)
    $outputDirectory = Split-Path -Parent $outputPath
    if ($outputDirectory) { New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null }
    $result | Export-Csv -LiteralPath $outputPath -NoTypeInformation -Encoding UTF8
}
$result | Format-Table -AutoSize | Out-Host
if ($PassThru) { $result }
if ($overallFailed) { throw "P0-6 shadow validation failed: $($overallReasons -join '; ')." }
