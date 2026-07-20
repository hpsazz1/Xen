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
    ('xen-real-reacquisition-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $header = 'FrameID,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,AimPipelineEffectiveMode,AimPipelineShadowProcessed,AimPipelineResetGeneration,AimPipelineObservationSequence,AimPipelineControlValid,AimPipelineControlSpeedLimited,AimPipelineUnlimitedCountsX,AimPipelineUnlimitedCountsY,AimPipelineRequestedCountsX,AimPipelineFrameCountLimit,RelativeErrorYawDegrees,DegreesPerCountX,ViewMotionShadowValid,CommandToFrameDelayMs,CommandResponseMs,ControlTimeNs,TargetDetected,CommandSendAttempted,CommandSendSucceeded,AimPipelineRateX,MaxCountsPerSecond'
    foreach ($scenario in @('left', 'right')) {
        $sign = if ($scenario -eq 'left') { -1.0 } else { 1.0 }
        $recoveryCounts = if ($scenario -eq 'left') { 298.7 } else { 301.9 }
        $recoveryDegrees = $sign * $recoveryCounts * 0.0308
        $path = Join-Path $temporaryRoot "horizontal_$scenario.csv"
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add($header)
        $lines.Add("1,DML,abc1234,20260720T010000Z,64,shadow,1,1,1,1,0,20,0,$($sign * 15),10,$($sign * 9.2),0.0308,1,60,20,1000000000,1,1,1,0,1440")
        $lines.Add("2,DML,abc1234,20260720T010000Z,64,shadow,1,1,2,1,1,30,0,$($sign * 10),10,$($sign * 8.5),0.0308,1,60,20,1010000000,1,1,1,$($sign * 5),1440")
        $lines.Add("3,DML,abc1234,20260720T010000Z,64,shadow,1,1,3,1,0,5,0,$($sign * 5),10,$($sign * 8),0.0308,1,60,20,1020000000,1,1,1,$($sign * 10),1440")
        $lines.Add("4,DML,abc1234,20260720T010000Z,64,shadow,1,2,1,1,1,20,0,$($sign * 15),10,$recoveryDegrees,0.0308,1,60,20,1220000000,1,1,1,0,1440")
        $lines.Add("5,DML,abc1234,20260720T010000Z,64,shadow,1,2,2,1,1,18,0,$($sign * 14),10,$($sign * 8.7),0.0308,1,60,20,1230000000,1,1,1,$($sign * 5),1440")
        $lines.Add("6,DML,abc1234,20260720T010000Z,64,shadow,1,2,3,1,0,5,0,$($sign * 5),10,$($sign * 8),0.0308,1,60,20,1240000000,1,1,1,$($sign * 10),1440")
        $lines | Set-Content -LiteralPath $path -Encoding UTF8
    }
    $summaryCsv = Join-Path $temporaryRoot 'summary.csv'
    $eventsCsv = Join-Path $temporaryRoot 'events.csv'
    $summary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_real_reacquisition_envelope.ps1') `
        -DataRoot $temporaryRoot -RecoveryFrames 3 -MinimumEventsPerDirection 1 `
        -ReferenceLeftResidualCounts 298.7 -ReferenceRightResidualCounts 301.9 `
        -OutputEventsCsv $eventsCsv -OutputSummaryCsv $summaryCsv)
    if ($summary.Count -ne 3) { throw "Expected two directions plus overall, got $($summary.Count)." }
    $left = $summary | Where-Object Scenario -eq 'left'
    $right = $summary | Where-Object Scenario -eq 'right'
    $overall = $summary | Where-Object Scenario -eq 'overall'
    Assert-Near 1.0 $left.Events 0.001 'Left reset event must be detected.'
    Assert-Near 1.0 $right.Events 0.001 'Right reset event must be detected.'
    Assert-Near 100.0 $left.FirstFrameDirectionAlignedPercent 0.001 'Left request must align with residual.'
    Assert-Near 100.0 $right.FirstFrameDirectionAlignedPercent 0.001 'Right request must align with residual.'
    Assert-Near 298.7 $left.FirstResidualAbsP50Counts 0.001 'Left residual must convert from degrees to counts.'
    Assert-Near 301.9 $right.FirstResidualAbsP50Counts 0.001 'Right residual must convert from degrees to counts.'
    Assert-Near 2.0 $left.SaturationFramesP50 0.001 'Two recovery frames must be speed limited.'
    Assert-Near 20.0 $left.SaturationDurationP50Ms 0.001 'Saturation duration must use the 10 ms frame interval.'
    Assert-Near 1.0 $overall.CoverageReady 0.001 'Synthetic dual-direction coverage must be ready.'
    if (-not (Test-Path -LiteralPath $eventsCsv -PathType Leaf) -or
        -not (Test-Path -LiteralPath $summaryCsv -PathType Leaf)) {
        throw 'Machine-readable real reacquisition outputs were not written.'
    }
    Write-Output 'real reacquisition envelope analysis tests passed'
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
