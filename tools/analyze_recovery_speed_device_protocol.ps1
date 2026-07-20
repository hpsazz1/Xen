[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DataRoot,
    [string]$DecisionPath = '',
    [switch]$RequirePass
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $parsed = 0.0
    if (-not [double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed) -or
        [double]::IsNaN($parsed) -or [double]::IsInfinity($parsed)) {
        throw "Column '$Name' contains a non-finite value: $Value"
    }
    return $parsed
}

$root = [IO.Path]::GetFullPath($DataRoot)
$summaryPath = Join-Path $root 'recovery_speed_device_summary.csv'
$commandsPath = Join-Path $root 'recovery_speed_device_commands.csv'
$framesPath = Join-Path $root 'recovery_speed_device_frames.csv'
foreach ($path in @($summaryPath, $commandsPath, $framesPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Protocol output not found: $path" }
}
if (-not $DecisionPath) { $DecisionPath = Join-Path $root 'recovery_speed_device_analysis.txt' }
$resolvedDecision = [IO.Path]::GetFullPath($DecisionPath)
$summary = @(Import-Csv -LiteralPath $summaryPath | Sort-Object { [int]$_.Trial })
$commands = @(Import-Csv -LiteralPath $commandsPath)
$frames = @(Import-Csv -LiteralPath $framesPath)
$issues = [Collections.Generic.List[string]]::new()

if ($summary.Count -ne 4) { $issues.Add("summary-count:$($summary.Count)") }
if ($commands.Count -ne 64) { $issues.Add("command-count:$($commands.Count)") }
if ($frames.Count -eq 0) { $issues.Add('frames-empty') }
$identities = @($summary | ForEach-Object {
    "$($_.BuildBackend)|$($_.BuildRevision)|$($_.BuildTimestampUtc)|r$($_.ControllerRevision)|$($_.PlanId)"
} | Sort-Object -Unique)
if ($identities.Count -ne 1) { $issues.Add('summary-identity') }
$allIdentities = @(@($summary) + @($commands) + @($frames) | ForEach-Object {
    "$($_.BuildBackend)|$($_.BuildRevision)|$($_.BuildTimestampUtc)|r$($_.ControllerRevision)|$($_.PlanId)"
} | Sort-Object -Unique)
if ($allIdentities.Count -ne 1) { $issues.Add('command-identity') }
if (@($frames | Where-Object { $_.Valid -ne '1' -or (Get-FiniteDouble $_.TrackingQuality 'TrackingQuality') -lt 0.75 }).Count -gt 0) {
    $issues.Add('frame-tracking')
}

foreach ($trial in 1..4) {
    $row = @($summary | Where-Object { [int]$_.Trial -eq $trial })
    $trialCommands = @($commands | Where-Object { [int]$_.Trial -eq $trial } | Sort-Object { [int]$_.Command })
    if ($row.Count -ne 1 -or $trialCommands.Count -ne 16) {
        $issues.Add("trial-shape:$trial")
        continue
    }
    $expectedRate = if ($trial -le 2) { 1440 } else { 1800 }
    $expectedDirection = if ($trial % 2) { 1 } else { -1 }
    $pulse = if ($expectedRate -eq 1440) { @(6,6,6,6,6,6,6,6) } else { @(7,8,7,8,7,8,7,8) }
    for ($index = 0; $index -lt $trialCommands.Count; $index++) {
        $returning = $index -ge 8
        $frame = $index % 8
        $pulseIndex = if ($returning) { 7 - $frame } else { $frame }
        $commandDirection = if ($returning) { -$expectedDirection } else { $expectedDirection }
        $expectedDelta = $commandDirection * $pulse[$pulseIndex]
        $command = $trialCommands[$index]
        $jitter = [math]::Abs((Get-FiniteDouble $command.AttemptNs 'AttemptNs') -
            (Get-FiniteDouble $command.ScheduledNs 'ScheduledNs')) / 1e6
        if ([int]$command.Command -ne ($index + 1) -or [int]$command.DeltaX -ne $expectedDelta -or
            $command.Succeeded -ne '1' -or $jitter -gt 4.2) {
            $issues.Add("command:$trial/$($index + 1)")
        }
    }
    $value = $row[0]
    if ([int]$value.RateCountsPerSecond -ne $expectedRate -or
        [int]$value.LeadingDirection -ne $expectedDirection -or $value.Passed -ne '1' -or
        (Get-FiniteDouble $value.MinimumTrackingQuality 'MinimumTrackingQuality') -lt 0.75 -or
        (Get-FiniteDouble $value.PixelsPerCount 'PixelsPerCount') -lt 0.25 -or
        (Get-FiniteDouble $value.PixelsPerCount 'PixelsPerCount') -gt 0.75 -or
        (Get-FiniteDouble $value.PeakDisplacementPx 'PeakDisplacementPx') -gt 48.0 -or
        (Get-FiniteDouble $value.VisualResponseLatencyMs 'VisualResponseLatencyMs') -lt 0.0 -or
        (Get-FiniteDouble $value.VisualResponseLatencyMs 'VisualResponseLatencyMs') -gt 100.0 -or
        (Get-FiniteDouble $value.StopAnchorDisplacementPx 'StopAnchorDisplacementPx') -lt 0.0 -or
        (Get-FiniteDouble $value.StopAnchorDisplacementPx 'StopAnchorDisplacementPx') -gt
            (Get-FiniteDouble $value.PeakDisplacementPx 'PeakDisplacementPx') -or
        (Get-FiniteDouble $value.StopDistancePx 'StopDistancePx') -gt 12.0 -or
        (Get-FiniteDouble $value.FinalResidualPx 'FinalResidualPx') -gt 3.0 -or
        (Get-FiniteDouble $value.CrossAxisLeakagePercent 'CrossAxisLeakagePercent') -gt 10.0) {
        $issues.Add("trial-gate:$trial")
    }
}

if ($summary.Count -eq 4) {
    foreach ($pair in @(@(0,1), @(2,3))) {
        $a = Get-FiniteDouble $summary[$pair[0]].PixelsPerCount 'PixelsPerCount'
        $b = Get-FiniteDouble $summary[$pair[1]].PixelsPerCount 'PixelsPerCount'
        $center = ($a + $b) * 0.5
        if ($center -le 0.0 -or 100.0 * [math]::Abs($a - $b) / $center -gt 15.0) {
            $issues.Add("direction-asymmetry:$($summary[$pair[0]].RateCountsPerSecond)")
        }
    }
    foreach ($index in 2..3) {
        $baseline = Get-FiniteDouble $summary[$index - 2].StopDistancePx 'StopDistancePx'
        $candidate = Get-FiniteDouble $summary[$index].StopDistancePx 'StopDistancePx'
        if ($candidate -gt $baseline + [math]::Max(2.0, $baseline * 0.25)) {
            $issues.Add("cross-rate-stop-distance:$($summary[$index].LeadingDirection)")
        }
    }
}

$passed = $issues.Count -eq 0
$recommendation = if ($passed) { 'MANUAL_REVIEW_ONLY' } else { 'HOLD_DIAGNOSTIC' }
@(
    "ProtocolPassed=$([int]$passed)"
    "Identity=$($identities -join ';')"
    "Trials=$($summary.Count)"
    "Commands=$($commands.Count)"
    "Frames=$($frames.Count)"
    "Recommendation=$recommendation"
    'ConfigurationAutoWrite=0'
    'ActiveAutoEnable=0'
    "Issues=$($issues -join '|')"
) | Set-Content -LiteralPath $resolvedDecision -Encoding UTF8
$result = [pscustomobject]@{ ProtocolPassed = $passed; Recommendation = $recommendation; Issues = $issues -join '|' }
$result | Format-List | Out-Host
if ($RequirePass -and -not $passed) { throw "Recovery speed device protocol failed: $($result.Issues)" }
$result
