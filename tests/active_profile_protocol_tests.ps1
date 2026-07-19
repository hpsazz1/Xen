param([Parameter(Mandatory = $true)][string]$RepoRoot)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RepoRoot)
$analyzer = Join-Path $root 'tools\analyze_active_profile_protocol.ps1'
if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) {
    throw "Active profile analyzer not found: $analyzer"
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'xen_active_profile_protocol_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
try {
    foreach ($run in 1..3) {
        foreach ($count in @(16, 32, 64)) {
            $directory = Join-Path $temporaryRoot ("counts{0}_run{1}" -f $count, $run)
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
            $rows = [Collections.Generic.List[object]]::new()
            $trial = 0
            foreach ($axis in @('x', 'y')) {
                foreach ($direction in @(1, -1)) {
                    foreach ($repeat in 1..5) {
                        ++$trial
                        $axisOffset = if ($axis -eq 'x') { 0.0 } else { 0.01 }
                        $runOffset = ($run - 2) * 0.005
                        $scale = 0.50 + $axisOffset + $runOffset
                        $signedCounts = $direction * $count
                        $rows.Add([pscustomobject]@{
                            BuildBackend = 'DML'
                            BuildRevision = 'synthetic'
                            BuildTimestampUtc = '20260720T000000Z'
                            ControllerRevision = '64'
                            Trial = $trial
                            Axis = $axis
                            SignedCounts = $signedCounts
                            BaselineSamples = 72
                            TailSamples = 120
                            FinalDisplacementPx = -$signedCounts * $scale
                            OrthogonalDisplacementPx = 0.1
                            PixelsPerCount = $scale
                            CrossAxisLeakagePercent = 100.0 * 0.1 /
                                ([math]::Abs($signedCounts) * $scale)
                            T10Ms = 10.0 + 0.2 * $run
                            T50Ms = 14.0 + 0.2 * $run
                            T90Ms = 15.0 + 0.2 * $run
                            T99Ms = 16.0 + 0.2 * $run
                            Valid = 1
                            Reason = 'ok'
                        })
                    }
                }
            }
            $rows | Export-Csv -LiteralPath (Join-Path $directory 'probe_summary.csv') `
                -NoTypeInformation -Encoding UTF8
        }
    }

    $summaryPath = Join-Path $temporaryRoot 'summary.csv'
    $decisionPath = Join-Path $temporaryRoot 'decision.txt'
    $stable = & $analyzer -DataRoot $temporaryRoot -OutputCsv $summaryPath `
        -DecisionPath $decisionPath -RequirePass
    if (-not $stable.ProtocolPassed) { throw 'Stable synthetic protocol did not pass.' }
    $decision = Get-Content -Raw -LiteralPath $decisionPath
    if ($decision -notmatch 'ProtocolPassed=1' -or
        $decision -notmatch 'Recommendation=MANUAL_REVIEW_ONLY' -or
        $decision -notmatch 'ProfileAutoWrite=0') {
        throw 'Stable protocol decision misses the manual-review safety contract.'
    }

    $badPath = Join-Path $temporaryRoot 'counts16_run1\probe_summary.csv'
    $badRows = @(Import-Csv -LiteralPath $badPath)
    foreach ($row in $badRows) {
        if ($row.Axis -eq 'x' -and [int]$row.SignedCounts -gt 0) {
            $row.PixelsPerCount = '0.9'
            $row.FinalDisplacementPx = [string](-16.0 * 0.9)
        }
    }
    $badRows | Export-Csv -LiteralPath $badPath -NoTypeInformation -Encoding UTF8
    $failed = & $analyzer -DataRoot $temporaryRoot `
        -OutputCsv (Join-Path $temporaryRoot 'failed_summary.csv') `
        -DecisionPath (Join-Path $temporaryRoot 'failed_decision.txt')
    if ($failed.ProtocolPassed -or $failed.Recommendation -ne 'HOLD_DIAGNOSTIC') {
        throw 'Direction-asymmetric synthetic protocol was not rejected.'
    }
}
finally {
    $resolvedTemporary = [IO.Path]::GetFullPath($temporaryRoot)
    $systemTemporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolvedTemporary.StartsWith($systemTemporary,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean non-temporary path: $resolvedTemporary"
    }
    Remove-Item -LiteralPath $resolvedTemporary -Recurse -Force -ErrorAction SilentlyContinue
}
