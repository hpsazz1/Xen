[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Near {
    param([double]$Expected, [double]$Actual, [double]$Tolerance, [string]$Message)
    if ([math]::Abs($Expected - $Actual) -gt $Tolerance) {
        throw "$Message Expected=$Expected Actual=$Actual"
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('xen-phase-separation-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $header = 'Scenario,Variant,TimeSeconds,Detected,CandidateErrorX,CandidateErrorY,TruthRateX,TruthRateY,PhysicalCameraRateX,PhysicalCameraRateY,ModelAngleDeltaDeg,ModelRateDeltaDps,ManeuverRateEvidenceDps,ManeuverModelActive,UnlimitedToFrameLimitRatio,LimitedToUnlimitedRatio'
    $baseline = Join-Path $temporaryRoot 'baseline.csv'
    $comparison = Join-Path $temporaryRoot 'comparison.csv'
    $baselineRows = [Collections.Generic.List[string]]::new()
    $comparisonRows = [Collections.Generic.List[string]]::new()
    $baselineRows.Add($header)
    $comparisonRows.Add($header)
    foreach ($scenario in @('jump', 'left', 'reverse', 'right', 'static')) {
        $phase = if ($scenario -match 'left|right') { 'sustained' } elseif (
            $scenario -match 'jump|reverse') { 'maneuver' } else { 'static' }
        for ($sample = 0; $sample -lt 20; ++$sample) {
            $baselineError = if ($phase -eq 'sustained') { 2.0 } elseif ($phase -eq 'maneuver') { 1.0 } else { 0.1 }
            $comparisonError = if ($phase -eq 'sustained') { 1.8 } elseif ($phase -eq 'maneuver') { 1.2 } else { 0.1 }
            $truthRate = if ($phase -eq 'sustained') { 10.0 } elseif ($phase -eq 'maneuver') { 20.0 } else { 0.0 }
            $modelDelta = if ($phase -eq 'maneuver') { 2.0 } else { 0.2 }
            $active = if ($phase -eq 'maneuver') { 1 } else { 0 }
            $limitRatio = if ($phase -eq 'sustained') { 2.0 } else { 0.5 }
            $appliedRatio = if ($limitRatio -gt 1.0) { 0.5 } else { 1.0 }
            $suffix = "0,$truthRate,0,$truthRate,0,$modelDelta,$modelDelta,$truthRate,$active,$limitRatio,$appliedRatio"
            $baselineRows.Add("$scenario,unit,$sample,1,$baselineError,$suffix")
            $comparisonRows.Add("$scenario,unit,$sample,1,$comparisonError,$suffix")
        }
    }
    $baselineRows | Set-Content -LiteralPath $baseline -Encoding UTF8
    $comparisonRows | Set-Content -LiteralPath $comparison -Encoding UTF8
    $output = Join-Path $temporaryRoot 'summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_phase_separation.ps1') `
        -BaselineFrames $baseline -ComparisonFrames $comparison -OutputCsv $output)
    if ($metrics.Count -ne 7) { throw "Expected five scenarios and two phase cohorts, got $($metrics.Count)." }
    $sustained = $metrics | Where-Object Cohort -eq 'sustained'
    $maneuver = $metrics | Where-Object Cohort -eq 'maneuver'
    Assert-Near -10.0 $sustained.ErrorP95DeltaPercent 0.001 `
        'Weaker response must improve the synthetic sustained cohort.'
    Assert-Near 20.0 $maneuver.ErrorP95DeltaPercent 0.001 `
        'Weaker response must regress the synthetic maneuver cohort.'
    Assert-Near 100.0 $sustained.SaturatedPercent 0.001 `
        'Unlimited request above the frame budget must count as saturated.'
    Assert-Near 100.0 $maneuver.ManeuverActivePercent 0.001 `
        'Maneuver rows must preserve model activity.'
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw 'Machine-readable phase summary was not written.'
    }
    Write-Output 'phase separation analysis tests passed'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        $resolvedSystemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedTemporaryRoot.StartsWith(
                $resolvedSystemTemp, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove a test directory outside the system temp root: $resolvedTemporaryRoot"
        }
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
    }
}
