[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DataRoot,
    [ValidateRange(1, 100)][int]$RecoveryFrames = 20,
    [ValidateRange(1, 10000)][int]$MinimumResetGapMs = 100,
    [ValidateRange(1, 10000)][int]$MinimumEventsPerDirection = 6,
    [ValidateRange(30.0, 4000.0)][double]$ExpectedShadowMaxCountsPerSecond = 1440.0,
    [string]$ExpectedBuildRevision = '',
    [double]$ReferenceLeftResidualCounts = 303.442641,
    [double]$ReferenceRightResidualCounts = 304.589065,
    [double]$ReferenceTolerancePercent = 20.0,
    [string]$OutputEventsCsv = '',
    [string]$OutputSummaryCsv = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $parsed = 0.0
    if (-not [double]::TryParse([string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed) -or
        [double]::IsNaN($parsed) -or [double]::IsInfinity($parsed)) {
        throw "Column '$Name' contains a non-finite value: $Value"
    }
    return $parsed
}

function Get-Percentile {
    param([double[]]$Values, [double]$Ratio)
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $index = [math]::Floor([math]::Max(0.0,
        [math]::Min(1.0, $Ratio)) * ($ordered.Count - 1))
    return [double]$ordered[$index]
}

function Write-AnalysisCsv {
    param([object[]]$Rows, [string]$Path, [string]$Description)
    if (-not $Path) { return }
    $resolved = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $resolved
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Rows | Export-Csv -LiteralPath $resolved -NoTypeInformation -Encoding UTF8
    Write-Host "$Description written to $resolved"
}

$resolvedRoot = [IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "Real reacquisition data root does not exist: $resolvedRoot"
}

$configFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Filter 'config.ini')
if ($configFiles.Count -ne 1) {
    throw "Data root must contain exactly one config.ini snapshot: $resolvedRoot"
}
$shadowSpeedLines = @(Get-Content -LiteralPath $configFiles[0].FullName |
    Where-Object { $_ -match '^\s*aim_shadow_max_speed_cps\s*=' })
if ($shadowSpeedLines.Count -ne 1) {
    throw "config.ini must contain exactly one aim_shadow_max_speed_cps setting: $($configFiles[0].FullName)"
}
$shadowSpeedText = ($shadowSpeedLines[0] -split '=', 2)[1].Trim()
$configuredShadowSpeed = Get-FiniteDouble $shadowSpeedText 'aim_shadow_max_speed_cps'
if ([math]::Abs($configuredShadowSpeed - $ExpectedShadowMaxCountsPerSecond) -gt 1e-6) {
    throw "config.ini does not use the frozen $ExpectedShadowMaxCountsPerSecond counts/s shadow limit: $($configFiles[0].FullName)"
}

$requiredColumns = @(
    'FrameID', 'BuildBackend', 'BuildRevision', 'BuildTimestampUtc', 'ControllerRevision',
    'AimPipelineEffectiveMode', 'AimPipelineShadowProcessed',
    'AimPipelineResetGeneration', 'AimPipelineObservationSequence',
    'AimPipelineControlValid', 'AimPipelineControlSpeedLimited',
    'AimPipelineUnlimitedCountsX', 'AimPipelineUnlimitedCountsY',
    'AimPipelineRequestedCountsX', 'AimPipelineFrameCountLimit',
    'RelativeErrorYawDegrees', 'DegreesPerCountX', 'ViewMotionShadowValid',
    'CommandToFrameDelayMs', 'CommandResponseMs', 'ControlTimeNs',
    'TargetDetected', 'CommandSendAttempted', 'CommandSendSucceeded',
    'AimPipelineRateX'
)
$eventRows = [Collections.Generic.List[object]]::new()
$eventSummaries = [Collections.Generic.List[object]]::new()
$identityRows = [Collections.Generic.List[object]]::new()
$inputFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Filter '*.csv' |
    Where-Object { $_.BaseName -match '(?i)(left|right)' } |
    Sort-Object FullName)
if ($inputFiles.Count -eq 0) {
    throw "No left/right pipeline CSV was found below: $resolvedRoot"
}

foreach ($csvFile in $inputFiles) {
    $rows = @(Import-Csv -LiteralPath $csvFile.FullName |
        Where-Object { $_.TargetDetected -eq '1' })
    if ($rows.Count -eq 0) { continue }
    $missing = @($requiredColumns |
        Where-Object { $rows[0].PSObject.Properties.Name -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "CSV misses real reacquisition columns [$($missing -join ', ')]: $($csvFile.FullName)"
    }

    $identity = [pscustomobject]@{
        Backend = [string]$rows[0].BuildBackend
        Revision = [string]$rows[0].BuildRevision
        TimestampUtc = [string]$rows[0].BuildTimestampUtc
        ControllerRevision = [int](Get-FiniteDouble $rows[0].ControllerRevision 'ControllerRevision')
    }
    if ($ExpectedBuildRevision -and $identity.Revision -ne $ExpectedBuildRevision) {
        continue
    }
    if ($identity.Revision -match '-dirty' -or $identity.ControllerRevision -ne 64) {
        throw "CSV identity is not a clean r64 build: $($csvFile.FullName)"
    }
    if ($identityRows.Count -gt 0) {
        $expectedIdentity = $identityRows[0]
        if ($identity.Backend -ne $expectedIdentity.Backend -or
            $identity.Revision -ne $expectedIdentity.Revision -or
            $identity.TimestampUtc -ne $expectedIdentity.TimestampUtc -or
            $identity.ControllerRevision -ne $expectedIdentity.ControllerRevision) {
            throw "Data root contains mixed executable identities: $($csvFile.FullName)"
        }
    }
    $identityRows.Add($identity)
    foreach ($row in $rows) {
        if ([string]$row.BuildBackend -ne $identity.Backend -or
            [string]$row.BuildRevision -ne $identity.Revision -or
            [string]$row.BuildTimestampUtc -ne $identity.TimestampUtc -or
            [int](Get-FiniteDouble $row.ControllerRevision 'ControllerRevision') -ne
                $identity.ControllerRevision) {
            throw "CSV contains mixed executable identities: $($csvFile.FullName)"
        }
        if ([string]$row.AimPipelineEffectiveMode.ToLowerInvariant() -ne 'shadow' -or
            $row.AimPipelineShadowProcessed -ne '1' -or
            $row.ViewMotionShadowValid -ne '1') {
            throw "CSV contains a non-shadow or unprocessed row: $($csvFile.FullName)"
        }
    }

    $hasLeft = $csvFile.BaseName -match '(?i)left'
    $hasRight = $csvFile.BaseName -match '(?i)right'
    if ($hasLeft -eq $hasRight) {
        throw "CSV filename must identify exactly one direction (left or right): $($csvFile.FullName)"
    }
    $scenario = if ($hasLeft) { 'left' } else { 'right' }
    $ordered = @($rows | Sort-Object { Get-FiniteDouble $_.ControlTimeNs 'ControlTimeNs' })
    $observedSpeedCaps = [Collections.Generic.List[double]]::new()
    for ($index = 1; $index -lt $ordered.Count; ++$index) {
        $intervalSeconds = ((Get-FiniteDouble $ordered[$index].ControlTimeNs 'ControlTimeNs') -
            (Get-FiniteDouble $ordered[$index - 1].ControlTimeNs 'ControlTimeNs')) / 1000000000.0
        $frameLimit = Get-FiniteDouble $ordered[$index].AimPipelineFrameCountLimit 'AimPipelineFrameCountLimit'
        if ($intervalSeconds -gt 0.0 -and $intervalSeconds -le 0.05 -and $frameLimit -gt 0.0) {
            $observedSpeedCaps.Add($frameLimit / $intervalSeconds)
        }
    }
    if ($observedSpeedCaps.Count -eq 0) {
        throw "CSV has no normal control intervals for shadow speed-limit validation: $($csvFile.FullName)"
    }
    $observedSpeedP05 = Get-Percentile -Values ([double[]]$observedSpeedCaps.ToArray()) -Ratio 0.05
    $observedSpeedP95 = Get-Percentile -Values ([double[]]$observedSpeedCaps.ToArray()) -Ratio 0.95
    if ([math]::Abs($observedSpeedP05 - $ExpectedShadowMaxCountsPerSecond) -gt 1.0 -or
        [math]::Abs($observedSpeedP95 - $ExpectedShadowMaxCountsPerSecond) -gt 1.0) {
        throw "CSV frame limits do not implement the frozen $ExpectedShadowMaxCountsPerSecond counts/s shadow limit: $($csvFile.FullName)"
    }
    for ($index = 1; $index -lt $ordered.Count; ++$index) {
        $previous = $ordered[$index - 1]
        $first = $ordered[$index]
        $previousGeneration = [int64](Get-FiniteDouble $previous.AimPipelineResetGeneration 'AimPipelineResetGeneration')
        $generation = [int64](Get-FiniteDouble $first.AimPipelineResetGeneration 'AimPipelineResetGeneration')
        $previousSequence = [int64](Get-FiniteDouble $previous.AimPipelineObservationSequence 'AimPipelineObservationSequence')
        $sequence = [int64](Get-FiniteDouble $first.AimPipelineObservationSequence 'AimPipelineObservationSequence')
        $firstControlTimeNs = Get-FiniteDouble $first.ControlTimeNs 'ControlTimeNs'
        $previousControlTimeNs = Get-FiniteDouble $previous.ControlTimeNs 'ControlTimeNs'
        $gapMs = ($firstControlTimeNs - $previousControlTimeNs) / 1000000.0
        $responseHorizonMs = (Get-FiniteDouble $first.CommandToFrameDelayMs 'CommandToFrameDelayMs') +
            (Get-FiniteDouble $first.CommandResponseMs 'CommandResponseMs') * 0.5
        $isRecovery = $sequence -eq 1 -and $previousSequence -gt 0 -and
            $generation -gt $previousGeneration -and
            $gapMs -ge [math]::Max($MinimumResetGapMs, $responseHorizonMs)
        if (-not $isRecovery) { continue }

        $generationRows = [Collections.Generic.List[object]]::new()
        for ($offset = 0; $index + $offset -lt $ordered.Count; ++$offset) {
            $row = $ordered[$index + $offset]
            $rowGeneration = [int64](Get-FiniteDouble $row.AimPipelineResetGeneration 'AimPipelineResetGeneration')
            if ($offset -gt 0 -and $rowGeneration -ne $generation) { break }
            $generationRows.Add($row)
        }
        if ($generationRows.Count -lt $RecoveryFrames) { continue }

        $saturationFrames = 0
        foreach ($row in $generationRows) {
            if ($row.AimPipelineControlSpeedLimited -ne '1') { break }
            ++$saturationFrames
        }
        $saturationExitObserved = $saturationFrames -lt $generationRows.Count
        $actualExitFrame = if ($saturationExitObserved) { $saturationFrames + 1 } else { 0 }
        $windowFrames = if ($saturationExitObserved) {
            [math]::Max($RecoveryFrames, $actualExitFrame)
        } else { $RecoveryFrames }
        $window = [Collections.Generic.List[object]]::new()
        for ($offset = 0; $offset -lt $windowFrames; ++$offset) {
            $window.Add($generationRows[$offset])
        }

        $firstResidualDegrees = Get-FiniteDouble $first.RelativeErrorYawDegrees 'RelativeErrorYawDegrees'
        $firstDegreesPerCount = Get-FiniteDouble $first.DegreesPerCountX 'DegreesPerCountX'
        if ([math]::Abs($firstDegreesPerCount) -le 1e-9) {
            throw "Recovery row has zero DegreesPerCountX: $($csvFile.FullName)"
        }
        $firstResidual = $firstResidualDegrees / $firstDegreesPerCount
        $firstRequest = Get-FiniteDouble $first.AimPipelineRequestedCountsX 'AimPipelineRequestedCountsX'
        $firstLimit = Get-FiniteDouble $first.AimPipelineFrameCountLimit 'AimPipelineFrameCountLimit'
        $firstUnlimitedX = Get-FiniteDouble $first.AimPipelineUnlimitedCountsX 'AimPipelineUnlimitedCountsX'
        $firstUnlimitedY = Get-FiniteDouble $first.AimPipelineUnlimitedCountsY 'AimPipelineUnlimitedCountsY'
        $firstUnlimited = [math]::Sqrt(
            $firstUnlimitedX * $firstUnlimitedX + $firstUnlimitedY * $firstUnlimitedY)
        $commandFailures = 0
        foreach ($row in $window) {
            if ($row.CommandSendAttempted -eq '1' -and $row.CommandSendSucceeded -ne '1') {
                ++$commandFailures
            }
        }
        $windowIntervals = [Collections.Generic.List[double]]::new()
        for ($offset = 1; $offset -lt $window.Count; ++$offset) {
            $intervalMs = ((Get-FiniteDouble $window[$offset].ControlTimeNs 'ControlTimeNs') -
                (Get-FiniteDouble $window[$offset - 1].ControlTimeNs 'ControlTimeNs')) / 1000000.0
            if ($intervalMs -gt 0.0) { $windowIntervals.Add($intervalMs) }
        }
        $nominalFrameMs = if ($windowIntervals.Count -gt 0) {
            Get-Percentile -Values ([double[]]$windowIntervals.ToArray()) -Ratio 0.50
        } else { 0.0 }
        $aligned = [math]::Abs($firstRequest) -le 1e-9 -or
            [math]::Abs($firstResidual) -le 1e-9 -or
            [math]::Sign($firstRequest) -eq [math]::Sign($firstResidual)
        for ($offset = 0; $offset -lt $window.Count; ++$offset) {
            $row = $window[$offset]
            $eventRows.Add([pscustomobject]@{
                File = $csvFile.Name
                Scenario = $scenario
                Event = $eventSummaries.Count + 1
                RecoveryFrame = $offset + 1
                ResetGeneration = $generation
                DetectionGapMs = $gapMs
                ResponseHorizonMs = $responseHorizonMs
                NominalFrameMs = $nominalFrameMs
                RelativeErrorYawDegrees = Get-FiniteDouble $row.RelativeErrorYawDegrees 'RelativeErrorYawDegrees'
                DegreesPerCountX = Get-FiniteDouble $row.DegreesPerCountX 'DegreesPerCountX'
                ResidualCountsX = (Get-FiniteDouble $row.RelativeErrorYawDegrees 'RelativeErrorYawDegrees') /
                    (Get-FiniteDouble $row.DegreesPerCountX 'DegreesPerCountX')
                EstimateRateX = Get-FiniteDouble $row.AimPipelineRateX 'AimPipelineRateX'
                UnlimitedCounts = [math]::Sqrt(
                    [math]::Pow((Get-FiniteDouble $row.AimPipelineUnlimitedCountsX 'AimPipelineUnlimitedCountsX'), 2.0) +
                    [math]::Pow((Get-FiniteDouble $row.AimPipelineUnlimitedCountsY 'AimPipelineUnlimitedCountsY'), 2.0))
                FrameCountLimit = Get-FiniteDouble $row.AimPipelineFrameCountLimit 'AimPipelineFrameCountLimit'
                SpeedLimited = [int]($row.AimPipelineControlSpeedLimited -eq '1')
                RequestedCountsX = Get-FiniteDouble $row.AimPipelineRequestedCountsX 'AimPipelineRequestedCountsX'
                CommandSendAttempted = [int]($row.CommandSendAttempted -eq '1')
                CommandSendSucceeded = [int]($row.CommandSendSucceeded -eq '1')
            })
        }
        $eventSummaries.Add([pscustomobject]@{
            File = $csvFile.Name
            Scenario = $scenario
            Event = $eventSummaries.Count + 1
            ResetGeneration = $generation
            DetectionGapMs = $gapMs
            ResponseHorizonMs = $responseHorizonMs
            NominalFrameMs = $nominalFrameMs
            FirstResidualCountsX = $firstResidual
            FirstResidualAbsCounts = [math]::Abs($firstResidual)
            FirstEstimateRateX = Get-FiniteDouble $first.AimPipelineRateX 'AimPipelineRateX'
            FirstUnlimitedToLimitRatio = if ($firstLimit -gt 1e-9) { $firstUnlimited / $firstLimit } else { 0.0 }
            FirstRequestDirectionAligned = [int]$aligned
            SaturationFrames = $saturationFrames
            SaturationExitFrame = $actualExitFrame
            SaturationExitObserved = [int]$saturationExitObserved
            SaturationDurationMs = $saturationFrames * $nominalFrameMs
            CommandFailureCount = $commandFailures
        })
    }
}

if ($eventSummaries.Count -eq 0) {
    throw 'No complete left/right reset-and-reacquisition events were found.'
}

$summaryRows = [Collections.Generic.List[object]]::new()
foreach ($group in ($eventSummaries | Group-Object Scenario)) {
    $events = @($group.Group)
    $reference = if ($group.Name -eq 'left') { $ReferenceLeftResidualCounts } else { $ReferenceRightResidualCounts }
    $residualP50 = Get-Percentile -Values @($events.FirstResidualAbsCounts) -Ratio 0.50
    $summaryRows.Add([pscustomobject]@{
        Scenario = $group.Name
        Events = $events.Count
        CoverageReady = [int]($events.Count -ge $MinimumEventsPerDirection)
        DetectionGapP50Ms = Get-Percentile -Values @($events.DetectionGapMs) -Ratio 0.50
        ResponseHorizonP50Ms = Get-Percentile -Values @($events.ResponseHorizonMs) -Ratio 0.50
        FirstResidualAbsP50Counts = $residualP50
        FirstResidualAbsP95Counts = Get-Percentile -Values @($events.FirstResidualAbsCounts) -Ratio 0.95
        CrossDomainReferenceCounts = $reference
        CrossDomainDeltaPercent = if ($reference -gt 1e-9) { 100.0 * ($residualP50 - $reference) / $reference } else { 0.0 }
        CrossDomainRepresentative = [int]([math]::Abs(100.0 * ($residualP50 - $reference) / [math]::Max(1.0, $reference)) -le $ReferenceTolerancePercent)
        FirstFrameZeroRatePercent = 100.0 * @($events | Where-Object { [math]::Abs($_.FirstEstimateRateX) -le 1e-9 }).Count / $events.Count
        FirstFrameDirectionAlignedPercent = 100.0 * @($events | Where-Object FirstRequestDirectionAligned -eq 1).Count / $events.Count
        FirstFrameSaturatedPercent = 100.0 * @($events | Where-Object { $_.FirstUnlimitedToLimitRatio -gt 1.0 + 1e-9 }).Count / $events.Count
        SaturationFramesP50 = Get-Percentile -Values @($events.SaturationFrames) -Ratio 0.50
        SaturationExitFrameP50 = Get-Percentile -Values @($events.SaturationExitFrame) -Ratio 0.50
        SaturationExitFrameP95 = Get-Percentile -Values @($events.SaturationExitFrame) -Ratio 0.95
        SaturationExitFrameMax = ($events.SaturationExitFrame | Measure-Object -Maximum).Maximum
        SaturationDurationP50Ms = Get-Percentile -Values @($events.SaturationDurationMs) -Ratio 0.50
        SaturationExitObservedPercent = 100.0 * @($events | Where-Object SaturationExitObserved -eq 1).Count / $events.Count
        CommandFailureEvents = @($events | Where-Object CommandFailureCount -gt 0).Count
    })
}

$identitySummary = $identityRows | Select-Object -First 1
$overallReady = @($summaryRows | Where-Object {
    $_.CoverageReady -eq 1 -and $_.CrossDomainRepresentative -eq 1 -and
    $_.FirstFrameDirectionAlignedPercent -eq 100 -and $_.CommandFailureEvents -eq 0 -and
    $_.SaturationExitObservedPercent -eq 100
}).Count -eq 2
$summaryRows.Add([pscustomobject]@{
    Scenario = 'overall'
    Events = $eventSummaries.Count
    CoverageReady = [int]$overallReady
    DetectionGapP50Ms = 0.0
    ResponseHorizonP50Ms = 0.0
    FirstResidualAbsP50Counts = 0.0
    FirstResidualAbsP95Counts = 0.0
    CrossDomainReferenceCounts = 0.0
    CrossDomainDeltaPercent = 0.0
    CrossDomainRepresentative = [int]$overallReady
    FirstFrameZeroRatePercent = 0.0
    FirstFrameDirectionAlignedPercent = 0.0
    FirstFrameSaturatedPercent = 0.0
    SaturationFramesP50 = 0.0
    SaturationExitFrameP50 = 0.0
    SaturationExitFrameP95 = 0.0
    SaturationExitFrameMax = 0.0
    SaturationDurationP50Ms = 0.0
    SaturationExitObservedPercent = 0.0
    CommandFailureEvents = @($eventSummaries | Where-Object CommandFailureCount -gt 0).Count
    BuildBackend = $identitySummary.Backend
    BuildRevision = $identitySummary.Revision
    BuildTimestampUtc = $identitySummary.TimestampUtc
    ControllerRevision = $identitySummary.ControllerRevision
})

$summaryRows | Sort-Object @{ Expression = { if ($_.Scenario -eq 'overall') { 1 } else { 0 } } }, Scenario |
    Format-Table -AutoSize | Out-Host
Write-AnalysisCsv @($eventRows) $OutputEventsCsv 'Real reacquisition event trace'
Write-AnalysisCsv @($summaryRows) $OutputSummaryCsv 'Real reacquisition envelope summary'
$summaryRows
