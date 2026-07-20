[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Frames,
    [ValidateRange(1, 100)][int]$RecoveryFrames = 12,
    [ValidateRange(1.1, 10.0)][double]$GapFactor = 1.5,
    [string]$OutputEventsCsv = '',
    [string]$OutputSummaryCsv = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $parsed = 0.0
    if (-not [double]::TryParse(
            [string]$Value,
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

function Get-EndpointName {
    param([string]$Path)
    $resolved = [IO.Path]::GetFullPath($Path)
    if ($resolved -match 'physical(?<Endpoint>\d+x\d+)') {
        return $Matches.Endpoint
    }
    return [IO.Path]::GetFileNameWithoutExtension($resolved)
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

$requiredColumns = @(
    'Scenario', 'Variant', 'TimeSeconds', 'Detected',
    'TruthRateX', 'EstimateAngleX', 'EstimateRateX', 'EstimateTruthBiasX',
    'CommittedEndpointResidualX', 'UnlimitedToFrameLimitRatio',
    'LimitedToUnlimitedRatio', 'PreGuardRequestedX',
    'CommittedEndpointGuardActive', 'RequestedX', 'CandidateErrorX'
)
$eventRows = [Collections.Generic.List[object]]::new()
$eventSummaries = [Collections.Generic.List[object]]::new()

foreach ($framePath in $Frames) {
    $resolvedFramePath = [IO.Path]::GetFullPath($framePath)
    if (-not (Test-Path -LiteralPath $resolvedFramePath -PathType Leaf)) {
        throw "Cross-domain frame CSV does not exist: $resolvedFramePath"
    }
    $rows = @(Import-Csv -LiteralPath $resolvedFramePath |
        Where-Object { $_.Detected -eq '1' -and $_.Scenario -in @('left', 'right') })
    if ($rows.Count -eq 0) {
        throw "Cross-domain frame CSV has no detected left/right rows: $resolvedFramePath"
    }
    $missing = @($requiredColumns |
        Where-Object { $rows[0].PSObject.Properties.Name -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "Cross-domain frame CSV misses recovery diagnostics [$($missing -join ', ')]: $resolvedFramePath"
    }

    $endpoint = Get-EndpointName $resolvedFramePath
    foreach ($group in ($rows | Group-Object Scenario,Variant)) {
        $ordered = @($group.Group | Sort-Object { Get-FiniteDouble $_.TimeSeconds 'TimeSeconds' })
        $intervals = [Collections.Generic.List[double]]::new()
        for ($index = 1; $index -lt $ordered.Count; ++$index) {
            $delta = (Get-FiniteDouble $ordered[$index].TimeSeconds 'TimeSeconds') -
                (Get-FiniteDouble $ordered[$index - 1].TimeSeconds 'TimeSeconds')
            if ($delta -gt 0.0) { $intervals.Add($delta) }
        }
        if ($intervals.Count -eq 0) { continue }
        # 中位帧间隔不受少量检测空洞影响；按它判定恢复边界可兼容不同回放FPS。
        $nominalInterval = 0.0
        $nominalInterval = Get-Percentile -Values ([double[]]$intervals.ToArray()) -Ratio 0.50
        $eventNumber = 0
        for ($index = 1; $index -lt $ordered.Count; ++$index) {
            $time = Get-FiniteDouble $ordered[$index].TimeSeconds 'TimeSeconds'
            $previousTime = Get-FiniteDouble $ordered[$index - 1].TimeSeconds 'TimeSeconds'
            $gap = $time - $previousTime
            if ($gap -le $nominalInterval * $GapFactor) { continue }

            ++$eventNumber
            $window = [Collections.Generic.List[object]]::new()
            for ($offset = 0; $offset -lt $RecoveryFrames -and
                $index + $offset -lt $ordered.Count; ++$offset) {
                $row = $ordered[$index + $offset]
                if ($offset -gt 0) {
                    $rowTime = Get-FiniteDouble $row.TimeSeconds 'TimeSeconds'
                    $priorTime = Get-FiniteDouble $ordered[$index + $offset - 1].TimeSeconds 'TimeSeconds'
                    if ($rowTime - $priorTime -gt $nominalInterval * $GapFactor) { break }
                }
                $window.Add($row)
            }

            $saturationFrames = 0
            foreach ($row in $window) {
                if ((Get-FiniteDouble $row.UnlimitedToFrameLimitRatio 'UnlimitedToFrameLimitRatio') -le
                    1.0 + 1e-9) { break }
                ++$saturationFrames
            }
            $first = $window[0]
            $firstResidual = Get-FiniteDouble $first.CommittedEndpointResidualX 'CommittedEndpointResidualX'
            $firstRequest = Get-FiniteDouble $first.RequestedX 'RequestedX'
            $directionAligned = [math]::Abs($firstRequest) -le 1e-9 -or
                [math]::Abs($firstResidual) -le 1e-9 -or
                [math]::Sign($firstRequest) -eq [math]::Sign($firstResidual)
            $halfRateFrame = 0

            for ($offset = 0; $offset -lt $window.Count; ++$offset) {
                $row = $window[$offset]
                $truthRate = Get-FiniteDouble $row.TruthRateX 'TruthRateX'
                $estimateRate = Get-FiniteDouble $row.EstimateRateX 'EstimateRateX'
                $rateFraction = if ([math]::Abs($truthRate) -gt 1e-9) {
                    [math]::Abs($estimateRate) / [math]::Abs($truthRate)
                } else { 1.0 }
                if ($halfRateFrame -eq 0 -and $rateFraction -ge 0.5) {
                    $halfRateFrame = $offset + 1
                }
                $eventRows.Add([pscustomobject]@{
                    Endpoint = $endpoint
                    Scenario = [string]$row.Scenario
                    Variant = [string]$row.Variant
                    Event = $eventNumber
                    RecoveryFrame = $offset + 1
                    TimeSeconds = Get-FiniteDouble $row.TimeSeconds 'TimeSeconds'
                    DetectionGapMs = 1000.0 * $gap
                    NominalFrameMs = 1000.0 * $nominalInterval
                    TruthRateX = $truthRate
                    EstimateAngleX = Get-FiniteDouble $row.EstimateAngleX 'EstimateAngleX'
                    EstimateRateX = $estimateRate
                    EstimateRateFraction = $rateFraction
                    EstimateTruthBiasX = Get-FiniteDouble $row.EstimateTruthBiasX 'EstimateTruthBiasX'
                    CommittedEndpointResidualX = Get-FiniteDouble $row.CommittedEndpointResidualX 'CommittedEndpointResidualX'
                    UnlimitedToFrameLimitRatio = Get-FiniteDouble $row.UnlimitedToFrameLimitRatio 'UnlimitedToFrameLimitRatio'
                    LimitedToUnlimitedRatio = Get-FiniteDouble $row.LimitedToUnlimitedRatio 'LimitedToUnlimitedRatio'
                    PreGuardRequestedX = Get-FiniteDouble $row.PreGuardRequestedX 'PreGuardRequestedX'
                    CommittedEndpointGuardActive = [int](Get-FiniteDouble $row.CommittedEndpointGuardActive 'CommittedEndpointGuardActive')
                    RequestedX = Get-FiniteDouble $row.RequestedX 'RequestedX'
                    CandidateErrorX = Get-FiniteDouble $row.CandidateErrorX 'CandidateErrorX'
                })
            }

            $eventSummaries.Add([pscustomobject]@{
                Endpoint = $endpoint
                Scenario = [string]$first.Scenario
                Variant = [string]$first.Variant
                Event = $eventNumber
                DetectionGapMs = 1000.0 * $gap
                NominalFrameMs = 1000.0 * $nominalInterval
                FirstEstimateRateX = Get-FiniteDouble $first.EstimateRateX 'EstimateRateX'
                FirstEstimateTruthBiasAbsDeg = [math]::Abs(
                    (Get-FiniteDouble $first.EstimateTruthBiasX 'EstimateTruthBiasX'))
                FirstResidualAbsCounts = [math]::Abs($firstResidual)
                FirstLimitRatio = Get-FiniteDouble $first.UnlimitedToFrameLimitRatio 'UnlimitedToFrameLimitRatio'
                FirstRequestDirectionAligned = [int]$directionAligned
                SaturationFrames = $saturationFrames
                SaturationDurationMs = 1000.0 * $saturationFrames * $nominalInterval
                SaturationExitObserved = [int]($saturationFrames -lt $window.Count)
                EstimateHalfTruthRateFrame = $halfRateFrame
            })
        }
    }
}

if ($eventSummaries.Count -eq 0) {
    throw 'No left/right detection recovery events were found.'
}

$summaryRows = [Collections.Generic.List[object]]::new()
foreach ($group in ($eventSummaries | Group-Object Endpoint,Scenario)) {
    $events = @($group.Group)
    $endpoint = [string]$events[0].Endpoint
    $scenario = [string]$events[0].Scenario
    $matchingFrames = @($eventRows | Where-Object {
        $_.Endpoint -eq $endpoint -and $_.Scenario -eq $scenario
    })
    $summaryRows.Add([pscustomobject]@{
        Endpoint = $endpoint
        Scenario = $scenario
        RecoveryEvents = $events.Count
        DetectionGapP50Ms = Get-Percentile -Values @($events.DetectionGapMs) -Ratio 0.50
        FirstFrameZeroRatePercent = 100.0 * @($events |
            Where-Object { [math]::Abs($_.FirstEstimateRateX) -le 1e-9 }).Count / $events.Count
        FirstFrameDirectionAlignedPercent = 100.0 * @($events |
            Where-Object FirstRequestDirectionAligned -eq 1).Count / $events.Count
        FirstFrameBiasAbsP95Deg = Get-Percentile -Values @($events.FirstEstimateTruthBiasAbsDeg) -Ratio 0.95
        FirstFrameResidualAbsP50Counts = Get-Percentile -Values @($events.FirstResidualAbsCounts) -Ratio 0.50
        FirstFrameLimitRatioP50 = Get-Percentile -Values @($events.FirstLimitRatio) -Ratio 0.50
        FirstFrameSaturatedPercent = 100.0 * @($events |
            Where-Object { $_.FirstLimitRatio -gt 1.0 + 1e-9 }).Count / $events.Count
        SaturationFramesP50 = Get-Percentile -Values @($events.SaturationFrames) -Ratio 0.50
        SaturationFramesP95 = Get-Percentile -Values @($events.SaturationFrames) -Ratio 0.95
        SaturationDurationP50Ms = Get-Percentile -Values @($events.SaturationDurationMs) -Ratio 0.50
        SaturationExitObservedPercent = 100.0 * @($events |
            Where-Object SaturationExitObserved -eq 1).Count / $events.Count
        EstimateHalfTruthRateFrameP50 = Get-Percentile -Values @($events.EstimateHalfTruthRateFrame) -Ratio 0.50
        GuardActiveFramePercent = 100.0 * @($matchingFrames |
            Where-Object CommittedEndpointGuardActive -eq 1).Count / $matchingFrames.Count
    })
}

$summaryRows | Sort-Object Endpoint,Scenario | Format-Table -AutoSize | Out-Host
Write-AnalysisCsv @($eventRows) $OutputEventsCsv 'Recovery event trace'
Write-AnalysisCsv @($summaryRows) $OutputSummaryCsv 'Recovery phase summary'
$summaryRows
