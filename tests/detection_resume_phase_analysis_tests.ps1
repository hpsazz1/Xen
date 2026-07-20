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
    ('xen-detection-resume-phase-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $header = 'Scenario,Variant,TimeSeconds,Detected,TruthRateX,EstimateAngleX,EstimateRateX,EstimateTruthBiasX,CommittedEndpointResidualX,UnlimitedToFrameLimitRatio,LimitedToUnlimitedRatio,PreGuardRequestedX,CommittedEndpointGuardActive,RequestedX,CandidateErrorX'
    $framePaths = [Collections.Generic.List[string]]::new()
    foreach ($endpoint in @('14x0', '15x2')) {
        $directory = Join-Path $temporaryRoot "analysis-physical$endpoint-unit"
        New-Item -ItemType Directory -Path $directory | Out-Null
        $path = Join-Path $directory 'cross_domain_frames.csv'
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add($header)
        foreach ($scenario in @('left', 'right')) {
            $sign = if ($scenario -eq 'left') { -1.0 } else { 1.0 }
            $lines.Add("$scenario,unit,0.00,1,$($sign * 10),0,$($sign * 10),0,$($sign * 5),0.5,1,$($sign * 5),0,$($sign * 5),0")
            $lines.Add("$scenario,unit,0.01,1,$($sign * 10),0,$($sign * 10),0,$($sign * 5),0.5,1,$($sign * 5),0,$($sign * 5),0")
            $lines.Add("$scenario,unit,0.02,1,$($sign * 10),0,$($sign * 10),0,$($sign * 5),0.5,1,$($sign * 5),0,$($sign * 5),0")
            $lines.Add("$scenario,unit,0.08,1,$($sign * 10),0,0,0.2,$($sign * 300),2.0,0.5,$($sign * 15),0,$($sign * 15),9")
            $lines.Add("$scenario,unit,0.09,1,$($sign * 10),0,$($sign * 5),0.1,$($sign * 280),1.5,0.67,$($sign * 15),0,$($sign * 15),8")
            $lines.Add("$scenario,unit,0.10,1,$($sign * 10),0,$($sign * 10),0.05,$($sign * 250),0.9,1,$($sign * 14),1,$($sign * 10),7")
            $lines.Add("$scenario,unit,0.11,1,$($sign * 10),0,$($sign * 10),0.02,$($sign * 220),0.8,1,$($sign * 12),0,$($sign * 12),6")
        }
        $lines | Set-Content -LiteralPath $path -Encoding UTF8
        $framePaths.Add($path)
    }

    $eventsCsv = Join-Path $temporaryRoot 'events.csv'
    $summaryCsv = Join-Path $temporaryRoot 'summary.csv'
    $summary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_detection_resume_phase.ps1') `
        -Frames @($framePaths) -RecoveryFrames 4 `
        -OutputEventsCsv $eventsCsv -OutputSummaryCsv $summaryCsv)
    if ($summary.Count -ne 4) {
        throw "Expected two endpoints by two directions, got $($summary.Count)."
    }
    foreach ($row in $summary) {
        Assert-Near 100.0 $row.FirstFrameZeroRatePercent 0.001 `
            'Every synthetic recovery must begin with a reset estimator rate.'
        Assert-Near 100.0 $row.FirstFrameDirectionAlignedPercent 0.001 `
            'Every synthetic first request must point toward the endpoint residual.'
        Assert-Near 300.0 $row.FirstFrameResidualAbsP50Counts 0.001 `
            'The analyzer must preserve the committed endpoint residual magnitude.'
        Assert-Near 2.0 $row.SaturationFramesP50 0.001 `
            'The first two synthetic recovery frames must be classified as saturated.'
        Assert-Near 20.0 $row.SaturationDurationP50Ms 0.001 `
            'Saturation duration must use the inferred 10 ms frame period.'
        Assert-Near 25.0 $row.GuardActiveFramePercent 0.001 `
            'One of four recovery frames must expose endpoint guard activity.'
    }
    if (-not (Test-Path -LiteralPath $eventsCsv -PathType Leaf) -or
        -not (Test-Path -LiteralPath $summaryCsv -PathType Leaf)) {
        throw 'Machine-readable recovery outputs were not written.'
    }
    Write-Output 'detection resume phase analysis tests passed'
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
