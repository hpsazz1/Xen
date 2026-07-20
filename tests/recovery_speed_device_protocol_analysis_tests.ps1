param([Parameter(Mandatory = $true)][string]$RepoRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$analyzer = Join-Path ([IO.Path]::GetFullPath($RepoRoot)) 'tools\analyze_recovery_speed_device_protocol.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('xen_recovery_speed_protocol_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
try {
    $summaries = foreach ($trial in 1..4) {
        $rate = if ($trial -le 2) { 1440 } else { 1800 }
        $direction = if ($trial % 2) { 1 } else { -1 }
        $excursion = if ($rate -eq 1440) { 48 } else { 60 }
        $forward = if ($rate -eq 1440) { 24 } else { 30 }
        $peak = if ($rate -eq 1440) { 25 } else { 31 }
        $stop = if ($rate -eq 1440) { 2 } else { 3 }
        [pscustomobject]@{
            BuildBackend='DML'; BuildRevision='synthetic'; BuildTimestampUtc='test'; ControllerRevision=65; PlanId='plan'
            Trial=$trial; RateCountsPerSecond=$rate; LeadingDirection=$direction
            ExpectedExcursionCounts=$excursion; Samples=600; Commands=16
            MinimumTrackingQuality=0.98; ForwardDisplacementPx=$forward
            PeakDisplacementPx=$peak; StopDistancePx=$stop
            FinalResidualPx=1; PixelsPerCount=0.5; CrossAxisLeakagePercent=2; MaximumCommandJitterMs=0.5
            Passed=1; Reason='passed'
        }
    }
    $commands = foreach ($trial in 1..4) {
        $rate = if ($trial -le 2) { 1440 } else { 1800 }
        $direction = if ($trial % 2) { 1 } else { -1 }
        $pulse = if ($rate -eq 1440) { @(6,6,6,6,6,6,6,6) } else { @(7,8,7,8,7,8,7,8) }
        foreach ($index in 0..15) {
            $returning = $index -ge 8
            $frame = $index % 8
            $pulseIndex = if ($returning) { 7 - $frame } else { $frame }
            $commandDirection = if ($returning) { -$direction } else { $direction }
            $delta = $commandDirection * $pulse[$pulseIndex]
            [pscustomobject]@{ BuildBackend='DML'; BuildRevision='synthetic'; BuildTimestampUtc='test'; ControllerRevision=65; PlanId='plan'; Trial=$trial; Command=$index+1; DeltaX=$delta; ScheduledNs=1000000000+$index*4166667; AttemptNs=1000500000+$index*4166667; Succeeded=1 }
        }
    }
    $frames = 1..100 | ForEach-Object { [pscustomobject]@{ BuildBackend='DML'; BuildRevision='synthetic'; BuildTimestampUtc='test'; ControllerRevision=65; PlanId='plan'; Valid=1; TrackingQuality=0.98 } }
    $summaries | Export-Csv -LiteralPath (Join-Path $temporaryRoot 'recovery_speed_device_summary.csv') -NoTypeInformation -Encoding UTF8
    $commands | Export-Csv -LiteralPath (Join-Path $temporaryRoot 'recovery_speed_device_commands.csv') -NoTypeInformation -Encoding UTF8
    $frames | Export-Csv -LiteralPath (Join-Path $temporaryRoot 'recovery_speed_device_frames.csv') -NoTypeInformation -Encoding UTF8
    $pass = & $analyzer -DataRoot $temporaryRoot -RequirePass
    if (-not $pass.ProtocolPassed -or $pass.Recommendation -ne 'MANUAL_REVIEW_ONLY') { throw 'Valid protocol was rejected.' }
    $summaries[2].StopDistancePx = 8
    $summaries | Export-Csv -LiteralPath (Join-Path $temporaryRoot 'recovery_speed_device_summary.csv') -NoTypeInformation -Encoding UTF8
    $failed = & $analyzer -DataRoot $temporaryRoot
    if ($failed.ProtocolPassed -or $failed.Recommendation -ne 'HOLD_DIAGNOSTIC' -or
        $failed.Issues -notmatch 'cross-rate-stop-distance') { throw 'Cross-rate stop-distance regression was accepted.' }
    Write-Host 'Recovery speed device protocol analysis tests passed.'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
}
