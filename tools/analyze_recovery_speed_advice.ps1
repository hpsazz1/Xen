[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DataRoot,
    [string]$ExpectedBuildRevision = '',
    [string]$LeftFileName = '',
    [string]$RightFileName = '',
    [double]$ExpectedBaselineMaxCountsPerSecond = 1440.0,
    [double]$ExpectedAdvisoryMaxCountsPerSecond = 1800.0,
    [ValidateRange(0, 1000)]
    [int]$MinimumPostExitFrames = 5,
    [ValidateRange(1, 1000)]
    [int]$MinimumActiveExitedEventsPerDirection = 1,
    [string]$OutputEventsCsv = '',
    [string]$OutputSummaryCsv = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $number = 0.0
    if (-not [double]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$number) -or [double]::IsNaN($number) -or
        [double]::IsInfinity($number)) {
        throw "Invalid finite number in $Name`: $Value"
    }
    return $number
}

function Get-Percentile {
    param([double[]]$Values, [double]$Ratio)
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $position = [math]::Max(0.0, [math]::Min(1.0, $Ratio)) * ($ordered.Count - 1)
    $lower = [math]::Floor($position)
    $upper = [math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$ordered[$lower] }
    return [double]$ordered[$lower] +
        ($position - $lower) * ([double]$ordered[$upper] - [double]$ordered[$lower])
}

$resolvedRoot = Resolve-Path -LiteralPath $DataRoot
$files = @(Get-ChildItem -LiteralPath $resolvedRoot -File | Where-Object {
    $_.Name -match '^horizontal_(left|right).*\.csv$'
})
if ($files.Count -eq 0) {
    throw "No horizontal_left/right CSV files found in $resolvedRoot"
}
if ($LeftFileName) {
    $selectedLeft = @($files | Where-Object Name -eq $LeftFileName)
    if ($selectedLeft.Count -ne 1 -or $selectedLeft[0].Name -notmatch '^horizontal_left') {
        throw "Selected left file was not found or is not a horizontal_left CSV: $LeftFileName"
    }
    $files = @($files | Where-Object { $_.Name -notmatch '^horizontal_left' }) + $selectedLeft
}
if ($RightFileName) {
    $selectedRight = @($files | Where-Object Name -eq $RightFileName)
    if ($selectedRight.Count -ne 1 -or $selectedRight[0].Name -notmatch '^horizontal_right') {
        throw "Selected right file was not found or is not a horizontal_right CSV: $RightFileName"
    }
    $files = @($files | Where-Object { $_.Name -notmatch '^horizontal_right' }) + $selectedRight
}

$requiredColumns = @(
    'BuildBackend', 'BuildRevision', 'ControllerRevision',
    'AimPipelineResetGeneration', 'AimPipelineObservationSequence',
    'AimPipelineEffectiveMode', 'AimPipelineShadowProcessed',
    'AimPipelineControlValid', 'AimPipelineControlSpeedLimited',
    'AimPipelineUnlimitedCountsX', 'AimPipelineUnlimitedCountsY',
    'AimPipelineRequestedCountsX', 'AimPipelineRequestedCountsY',
    'AimPipelineFrameCountLimit', 'RecoverySpeedAdviceEligible',
    'RecoverySpeedAdviceActive', 'RecoverySpeedAdviceExited',
    'RecoverySpeedAdviceLimited', 'RecoverySpeedBaselineMaxCps',
    'RecoverySpeedAdvisoryMaxCps', 'RecoverySpeedAdvisoryFrameCountLimit',
    'RecoverySpeedAdvisoryRequestedCountsX',
    'RecoverySpeedAdvisoryRequestedCountsY',
    'RecoverySpeedBaselineStaticBudgetFrames',
    'RecoverySpeedAdvisoryStaticBudgetFrames',
    'RecoverySpeedStaticBudgetFramesSaved'
)

$frameRows = [Collections.Generic.List[object]]::new()
$events = [Collections.Generic.List[object]]::new()
$identityKeys = [Collections.Generic.HashSet[string]]::new()
$violations = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $rows = @(Import-Csv -LiteralPath $file.FullName)
    if ($rows.Count -eq 0) { continue }
    foreach ($column in $requiredColumns) {
        if (-not $rows[0].PSObject.Properties.Name.Contains($column)) {
            throw "Missing required column $column in $($file.FullName)"
        }
    }
    $selectedRows = @($rows | Where-Object {
        -not $ExpectedBuildRevision -or $_.BuildRevision -eq $ExpectedBuildRevision
    })
    if ($selectedRows.Count -eq 0) { continue }
    $direction = if ($file.Name -match '^horizontal_(left|right)') { $Matches[1] } else { 'unknown' }
    foreach ($row in $selectedRows) {
        [void]$identityKeys.Add("$($row.BuildBackend)|$($row.BuildRevision)|$($row.ControllerRevision)")
        $eligible = $row.RecoverySpeedAdviceEligible -eq '1'
        $active = $row.RecoverySpeedAdviceActive -eq '1'
        $exited = $row.RecoverySpeedAdviceExited -eq '1'
        $frameLimit = Get-FiniteDouble $row.AimPipelineFrameCountLimit 'AimPipelineFrameCountLimit'
        $formalX = Get-FiniteDouble $row.AimPipelineRequestedCountsX 'AimPipelineRequestedCountsX'
        $formalY = Get-FiniteDouble $row.AimPipelineRequestedCountsY 'AimPipelineRequestedCountsY'
        $formalMagnitude = [math]::Sqrt($formalX * $formalX + $formalY * $formalY)
        # The default CSV stream keeps about six significant digits. This tolerance
        # covers vector reconstruction rounding while remaining far below one count.
        $formalTolerance = [math]::Max(0.002, [math]::Abs($frameLimit) * 0.0001)
        if ($row.AimPipelineControlValid -eq '1' -and
            $formalMagnitude -gt $frameLimit + $formalTolerance) {
            $violations.Add("$($file.Name): formal request exceeds the 1440 frame limit")
        }
        if (($active -or $exited) -and -not $eligible) {
            $violations.Add("$($file.Name): advice state escaped its eligible recovery generation")
        }
        if (-not $eligible) { continue }

        $baselineMax = Get-FiniteDouble $row.RecoverySpeedBaselineMaxCps 'RecoverySpeedBaselineMaxCps'
        $advisoryMax = Get-FiniteDouble $row.RecoverySpeedAdvisoryMaxCps 'RecoverySpeedAdvisoryMaxCps'
        $advisoryLimit = Get-FiniteDouble $row.RecoverySpeedAdvisoryFrameCountLimit 'RecoverySpeedAdvisoryFrameCountLimit'
        $advisoryX = Get-FiniteDouble $row.RecoverySpeedAdvisoryRequestedCountsX 'RecoverySpeedAdvisoryRequestedCountsX'
        $advisoryY = Get-FiniteDouble $row.RecoverySpeedAdvisoryRequestedCountsY 'RecoverySpeedAdvisoryRequestedCountsY'
        $advisoryMagnitude = [math]::Sqrt($advisoryX * $advisoryX + $advisoryY * $advisoryY)
        if ($active -and (-not $eligible -or $row.AimPipelineControlSpeedLimited -ne '1' -or
                $row.AimPipelineControlValid -ne '1' -or
                $row.AimPipelineEffectiveMode -ne 'shadow' -or
                $row.AimPipelineShadowProcessed -ne '1')) {
            $violations.Add("$($file.Name): active advice outside an eligible baseline-limited frame")
        }
        if ([math]::Abs($baselineMax - $ExpectedBaselineMaxCountsPerSecond) -gt 0.01) {
            $violations.Add("$($file.Name): baseline identity is $baselineMax counts/s")
        }
        if ([math]::Abs($advisoryMax - $ExpectedAdvisoryMaxCountsPerSecond) -gt 0.01) {
            $violations.Add("$($file.Name): advisory identity is $advisoryMax counts/s")
        }
        $advisoryTolerance = [math]::Max(0.002, [math]::Abs($advisoryLimit) * 0.0001)
        if ($active -and $advisoryMagnitude -gt $advisoryLimit + $advisoryTolerance) {
            $violations.Add("$($file.Name): advisory request exceeds its diagnostic frame limit")
        }
        if ($active -and $exited) {
            $violations.Add("$($file.Name): exited recovery advice remained active")
        }
        $frameRows.Add([pscustomobject]@{
            File = $file.Name
            Direction = $direction
            ResetGeneration = [int64](Get-FiniteDouble $row.AimPipelineResetGeneration 'AimPipelineResetGeneration')
            ObservationSequence = [int64](Get-FiniteDouble $row.AimPipelineObservationSequence 'AimPipelineObservationSequence')
            Eligible = [int]$eligible
            Active = [int]$active
            Exited = [int]$exited
            AdvisoryLimited = [int]($row.RecoverySpeedAdviceLimited -eq '1')
            BaselineStaticBudgetFrames = Get-FiniteDouble $row.RecoverySpeedBaselineStaticBudgetFrames 'RecoverySpeedBaselineStaticBudgetFrames'
            AdvisoryStaticBudgetFrames = Get-FiniteDouble $row.RecoverySpeedAdvisoryStaticBudgetFrames 'RecoverySpeedAdvisoryStaticBudgetFrames'
            StaticBudgetFramesSaved = Get-FiniteDouble $row.RecoverySpeedStaticBudgetFramesSaved 'RecoverySpeedStaticBudgetFramesSaved'
        })
    }

    $maximumGeneration = ($selectedRows | ForEach-Object {
        [int64](Get-FiniteDouble $_.AimPipelineResetGeneration 'AimPipelineResetGeneration')
    } | Measure-Object -Maximum).Maximum
    foreach ($group in @($frameRows | Where-Object File -eq $file.Name |
            Group-Object ResetGeneration)) {
        $ordered = @($group.Group | Sort-Object ObservationSequence)
        if ($ordered.Count -eq 0) { continue }
        if ($ordered[0].ObservationSequence -ne 1) {
            $violations.Add("$($file.Name): recovery generation $($group.Name) does not start at sequence 1")
        }
        $firstExit = @($ordered | Where-Object Exited -eq 1 | Select-Object -First 1)
        $activeRows = @($ordered | Where-Object Active -eq 1)
        $firstActive = @($activeRows | Select-Object -First 1)
        $firstAdvisoryUnclipped = @($activeRows |
            Where-Object AdvisoryLimited -eq 0 | Select-Object -First 1)
        $exitSequence = if ($firstExit.Count -gt 0) {
            [int64]$firstExit[0].ObservationSequence
        } else { 0 }
        if ($exitSequence -gt 0 -and (@($activeRows | Where-Object {
                $_.ObservationSequence -ge $exitSequence
            })).Count -gt 0) {
            $violations.Add("$($file.Name): recovery generation $($group.Name) reactivated after exit")
        }
        $postExitFrames = if ($exitSequence -gt 0) {
            (@($ordered | Where-Object ObservationSequence -gt $exitSequence)).Count
        } else { 0 }
        $outcome = if ($exitSequence -gt 0) {
            'exited'
        } elseif ([int64]$group.Name -lt [int64]$maximumGeneration) {
            'interrupted'
        } else {
            'open'
        }
        $captureComplete = $outcome -eq 'exited' -and
            $postExitFrames -ge $MinimumPostExitFrames
        if ($outcome -eq 'exited' -and -not $captureComplete) {
            $violations.Add("$($file.Name): recovery generation $($group.Name) has only $postExitFrames post-exit frames")
        }
        $advisoryObservedPathExitSequence = if ($firstAdvisoryUnclipped.Count -gt 0) {
            [int64]$firstAdvisoryUnclipped[0].ObservationSequence
        } else { 0 }
        $firstBaselineBudget = if ($firstActive.Count -gt 0) {
            [double]$firstActive[0].BaselineStaticBudgetFrames
        } else { 0.0 }
        $firstAdvisoryBudget = if ($firstActive.Count -gt 0) {
            [double]$firstActive[0].AdvisoryStaticBudgetFrames
        } else { 0.0 }
        $events.Add([pscustomobject]@{
            File = $file.Name
            Direction = $direction
            ResetGeneration = [int64]$group.Name
            ObservedFrames = $ordered.Count
            ActiveFrames = $activeRows.Count
            AdvisoryLimitedFrames = (@($activeRows | Where-Object AdvisoryLimited -eq 1)).Count
            ExitSequence = $exitSequence
            PostExitFrames = $postExitFrames
            Outcome = $outcome
            CaptureComplete = [int]$captureComplete
            FirstActiveSequence = if ($firstActive.Count -gt 0) {
                [int64]$firstActive[0].ObservationSequence
            } else { 0 }
            FirstBaselineStaticBudgetFrames = $firstBaselineBudget
            FirstAdvisoryStaticBudgetFrames = $firstAdvisoryBudget
            FirstStaticBudgetFramesSaved = [math]::Max(
                0.0, $firstBaselineBudget - $firstAdvisoryBudget)
            ActualToFirstBaselineBudgetRatio = if ($firstBaselineBudget -gt 0.0) {
                $activeRows.Count / $firstBaselineBudget
            } else { 0.0 }
            ActualToFirstAdvisoryBudgetRatio = if ($firstAdvisoryBudget -gt 0.0) {
                $activeRows.Count / $firstAdvisoryBudget
            } else { 0.0 }
            AdvisoryObservedPathExitSequence = $advisoryObservedPathExitSequence
            ObservedPathFramesEarlier = if ($exitSequence -gt 0 -and
                    $advisoryObservedPathExitSequence -gt 0) {
                $exitSequence - $advisoryObservedPathExitSequence
            } else { 0 }
            AdvisoryObservedPathExitObserved = [int](
                $advisoryObservedPathExitSequence -gt 0)
            BaselineStaticBudgetP50 = Get-Percentile @($activeRows | ForEach-Object BaselineStaticBudgetFrames) 0.50
            BaselineStaticBudgetP95 = Get-Percentile @($activeRows | ForEach-Object BaselineStaticBudgetFrames) 0.95
            AdvisoryStaticBudgetP50 = Get-Percentile @($activeRows | ForEach-Object AdvisoryStaticBudgetFrames) 0.50
            AdvisoryStaticBudgetP95 = Get-Percentile @($activeRows | ForEach-Object AdvisoryStaticBudgetFrames) 0.95
            StaticBudgetFramesSavedP50 = Get-Percentile @($activeRows | ForEach-Object StaticBudgetFramesSaved) 0.50
            StaticBudgetFramesSavedP95 = Get-Percentile @($activeRows | ForEach-Object StaticBudgetFramesSaved) 0.95
        })
    }
}

if ($identityKeys.Count -eq 0) { throw 'No rows matched the requested build revision.' }
if (-not $ExpectedBuildRevision -and $identityKeys.Count -ne 1) {
    throw "Mixed build identities detected: $($identityKeys -join ', ')"
}
if ($events.Count -eq 0) { throw 'No recovery speed advice windows were found.' }

$summary = [Collections.Generic.List[object]]::new()
foreach ($direction in @('left', 'right', 'overall')) {
    $directionEvents = @(if ($direction -eq 'overall') {
        $events
    } else {
        $events | Where-Object Direction -eq $direction
    })
    $directionFrames = @(if ($direction -eq 'overall') {
        $frameRows
    } else {
        $frameRows | Where-Object Direction -eq $direction
    })
    $activeFrames = @($directionFrames | Where-Object Active -eq 1)
    $exitedEvents = @($directionEvents | Where-Object Outcome -eq 'exited')
    $activeExitedEvents = @($directionEvents | Where-Object {
        $_.ActiveFrames -gt 0 -and $_.Outcome -eq 'exited' -and $_.CaptureComplete -eq 1
    })
    $observedPathExitedEvents = @($activeExitedEvents | Where-Object {
        $_.AdvisoryObservedPathExitObserved -eq 1
    })
    $coverageReady = $directionEvents.Count -gt 0 -and
        $activeExitedEvents.Count -ge $MinimumActiveExitedEventsPerDirection -and
        (@($directionEvents | Where-Object Outcome -eq 'open')).Count -eq 0 -and
        (@($exitedEvents | Where-Object CaptureComplete -ne 1)).Count -eq 0
    if ($direction -eq 'overall') {
        $coverageReady = $coverageReady -and
            (@($summary | Where-Object CoverageReady -eq 1)).Count -eq 2
    }
    $summary.Add([pscustomobject]@{
        Direction = $direction
        BuildIdentity = @($identityKeys)[0]
        RecoveryEvents = $directionEvents.Count
        ActiveEvents = (@($directionEvents | Where-Object ActiveFrames -gt 0)).Count
        ActiveExitedEvents = $activeExitedEvents.Count
        ActiveFrames = $activeFrames.Count
        ExitedWindows = $exitedEvents.Count
        InterruptedWindows = (@($directionEvents | Where-Object Outcome -eq 'interrupted')).Count
        OpenWindows = (@($directionEvents | Where-Object Outcome -eq 'open')).Count
        ExitSequenceP50 = Get-Percentile @($exitedEvents | ForEach-Object ExitSequence) 0.50
        ExitSequenceP95 = Get-Percentile @($exitedEvents | ForEach-Object ExitSequence) 0.95
        ExitSequenceMax = (@($exitedEvents | ForEach-Object ExitSequence) | Measure-Object -Maximum).Maximum
        BaselineStaticBudgetP50 = Get-Percentile @($activeFrames | ForEach-Object BaselineStaticBudgetFrames) 0.50
        BaselineStaticBudgetP95 = Get-Percentile @($activeFrames | ForEach-Object BaselineStaticBudgetFrames) 0.95
        AdvisoryStaticBudgetP50 = Get-Percentile @($activeFrames | ForEach-Object AdvisoryStaticBudgetFrames) 0.50
        AdvisoryStaticBudgetP95 = Get-Percentile @($activeFrames | ForEach-Object AdvisoryStaticBudgetFrames) 0.95
        StaticBudgetFramesSavedP50 = Get-Percentile @($activeFrames | ForEach-Object StaticBudgetFramesSaved) 0.50
        StaticBudgetFramesSavedP95 = Get-Percentile @($activeFrames | ForEach-Object StaticBudgetFramesSaved) 0.95
        ActualToFirstBaselineBudgetRatioP50 = Get-Percentile @(
            $activeExitedEvents | ForEach-Object ActualToFirstBaselineBudgetRatio) 0.50
        ActualToFirstBaselineBudgetRatioP95 = Get-Percentile @(
            $activeExitedEvents | ForEach-Object ActualToFirstBaselineBudgetRatio) 0.95
        AdvisoryObservedPathExitPercent = if ($activeExitedEvents.Count -gt 0) {
            100.0 * $observedPathExitedEvents.Count / $activeExitedEvents.Count
        } else { 0.0 }
        ObservedPathFramesEarlierP50 = Get-Percentile @(
            $observedPathExitedEvents | ForEach-Object ObservedPathFramesEarlier) 0.50
        ObservedPathFramesEarlierP95 = Get-Percentile @(
            $observedPathExitedEvents | ForEach-Object ObservedPathFramesEarlier) 0.95
        ObservedPathFramesEarlierMax = (@(
            $observedPathExitedEvents | ForEach-Object ObservedPathFramesEarlier) |
            Measure-Object -Maximum).Maximum
        CoverageReady = [int]$coverageReady
        ViolationCount = $violations.Count
        Conclusion = if ($violations.Count -gt 0) {
            'REJECT'
        } elseif ($coverageReady) {
            'DIAGNOSTIC_ONLY_HOLD_SHADOW'
        } else {
            'MORE_REAL_DATA_REQUIRED_HOLD_SHADOW'
        }
    })
}

if ($OutputEventsCsv) { $events | Export-Csv -LiteralPath $OutputEventsCsv -NoTypeInformation -Encoding UTF8 }
if ($OutputSummaryCsv) { $summary | Export-Csv -LiteralPath $OutputSummaryCsv -NoTypeInformation -Encoding UTF8 }
if ($violations.Count -gt 0) { throw ($violations -join [Environment]::NewLine) }
$summary
