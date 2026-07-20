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
    'aim_shadow_max_speed_cps = 1440.00' |
        Set-Content -LiteralPath (Join-Path $temporaryRoot 'config.ini') -Encoding ASCII
    $header = 'FrameID,BuildBackend,BuildRevision,BuildTimestampUtc,ControllerRevision,AimPipelineEffectiveMode,AimPipelineShadowProcessed,AimPipelineResetGeneration,AimPipelineObservationSequence,AimPipelineControlValid,AimPipelineControlSpeedLimited,AimPipelineUnlimitedCountsX,AimPipelineUnlimitedCountsY,AimPipelineRequestedCountsX,AimPipelineFrameCountLimit,RelativeErrorYawDegrees,DegreesPerCountX,ViewMotionShadowValid,CommandToFrameDelayMs,CommandResponseMs,ControlTimeNs,TargetDetected,CommandSendAttempted,CommandSendSucceeded,AimPipelineRateX,MaxCountsPerSecond'
    foreach ($scenario in @('left', 'right')) {
        $sign = if ($scenario -eq 'left') { -1.0 } else { 1.0 }
        $recoveryCounts = if ($scenario -eq 'left') { 298.7 } else { 301.9 }
        $recoveryDegrees = $sign * $recoveryCounts * 0.0308
        $path = Join-Path $temporaryRoot ('horizontal_{0}1.csv' -f $scenario)
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add($header)
        $lines.Add("1,DML,abc1234,20260720T010000Z,64,shadow,1,1,1,1,0,20,0,$($sign * 15),14.4,$($sign * 9.2),0.0308,1,60,20,1000000000,1,1,1,0,1440")
        $lines.Add("2,DML,abc1234,20260720T010000Z,64,shadow,1,1,2,1,1,30,0,$($sign * 10),14.4,$($sign * 8.5),0.0308,1,60,20,1010000000,1,1,1,$($sign * 5),1440")
        $lines.Add("3,DML,abc1234,20260720T010000Z,64,shadow,1,1,3,1,0,5,0,$($sign * 5),14.4,$($sign * 8),0.0308,1,60,20,1020000000,1,1,1,$($sign * 10),1440")
        $lines.Add("4,DML,abc1234,20260720T010000Z,64,shadow,1,2,1,1,1,20,0,$($sign * 15),14.4,$recoveryDegrees,0.0308,1,60,20,1220000000,1,1,1,0,1440")
        $lines.Add("5,DML,abc1234,20260720T010000Z,64,shadow,1,2,2,1,1,18,0,$($sign * 14),14.4,$($sign * 8.7),0.0308,1,60,20,1230000000,1,1,1,$($sign * 5),1440")
        $lines.Add("6,DML,abc1234,20260720T010000Z,64,shadow,1,2,3,1,1,5,0,$($sign * 5),14.4,$($sign * 8),0.0308,1,60,20,1240000000,1,1,1,$($sign * 10),1440")
        $lines.Add("7,DML,abc1234,20260720T010000Z,64,shadow,1,2,4,1,0,5,0,$($sign * 5),14.4,$($sign * 7.5),0.0308,1,60,20,1250000000,1,1,1,$($sign * 10),1440")
        $lines.Add("8,DML,abc1234,20260720T010000Z,64,shadow,1,3,1,1,1,20,0,$($sign * 15),14.4,$recoveryDegrees,0.0308,1,60,20,1450000000,1,1,1,0,1440")
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
    Assert-Near 3.0 $left.SaturationFramesP50 0.001 'Actual saturation must extend beyond the minimum window.'
    Assert-Near 4.0 $left.SaturationExitFrameMax 0.001 'Actual exit frame must be reported.'
    Assert-Near 30.0 $left.SaturationDurationP50Ms 0.001 'Saturation duration must use the 10 ms frame interval.'
    Assert-Near 1.0 $overall.CoverageReady 0.001 'Synthetic dual-direction coverage must be ready.'
    if (-not (Test-Path -LiteralPath $eventsCsv -PathType Leaf) -or
        -not (Test-Path -LiteralPath $summaryCsv -PathType Leaf)) {
        throw 'Machine-readable real reacquisition outputs were not written.'
    }

    $adviceRoot = Join-Path $temporaryRoot 'advice'
    New-Item -ItemType Directory -Path $adviceRoot | Out-Null
    $adviceHeader = 'BuildBackend,BuildRevision,ControllerRevision,AimPipelineResetGeneration,AimPipelineObservationSequence,AimPipelineEffectiveMode,AimPipelineShadowProcessed,AimPipelineControlValid,AimPipelineControlSpeedLimited,AimPipelineUnlimitedCountsX,AimPipelineUnlimitedCountsY,AimPipelineRequestedCountsX,AimPipelineRequestedCountsY,AimPipelineFrameCountLimit,RecoverySpeedAdviceEligible,RecoverySpeedAdviceActive,RecoverySpeedAdviceExited,RecoverySpeedAdviceLimited,RecoverySpeedBaselineMaxCps,RecoverySpeedAdvisoryMaxCps,RecoverySpeedAdvisoryFrameCountLimit,RecoverySpeedAdvisoryRequestedCountsX,RecoverySpeedAdvisoryRequestedCountsY,RecoverySpeedBaselineStaticBudgetFrames,RecoverySpeedAdvisoryStaticBudgetFrames,RecoverySpeedStaticBudgetFramesSaved'
    foreach ($scenario in @('left', 'right')) {
        $sign = if ($scenario -eq 'left') { -1 } else { 1 }
        @(
            $adviceHeader
            "DML,abc1234,64,2,1,shadow,1,1,1,$($sign * 30),0,$($sign * 14.4),0,14.4,1,1,0,1,1440,1800,18,$($sign * 18),0,3,2,1"
            "DML,abc1234,64,2,2,shadow,1,1,1,$($sign * 16),0,$($sign * 14.4),0,14.4,1,1,0,0,1440,1800,18,$($sign * 16),0,2,1,1"
            "DML,abc1234,64,2,3,shadow,1,1,0,$($sign * 10),0,$($sign * 10),0,14.4,1,0,1,0,1440,1800,0,0,0,0,0,0"
        ) | Set-Content -LiteralPath (Join-Path $adviceRoot "horizontal_$($scenario)1.csv") -Encoding UTF8
    }
    $adviceSummary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_recovery_speed_advice.ps1') `
        -DataRoot $adviceRoot -ExpectedBuildRevision 'abc1234' -MinimumPostExitFrames 0 `
        -MinimumActiveExitedEventsPerDirection 1)
    $adviceOverall = $adviceSummary | Where-Object Direction -eq 'overall'
    Assert-Near 4.0 $adviceOverall.ActiveFrames 0.001 `
        'Recovery advice analyzer must count active diagnostic frames.'
    Assert-Near 2.0 $adviceOverall.ExitedWindows 0.001 `
        'Recovery advice analyzer must count formal exit frames.'
    Assert-Near 0.0 $adviceOverall.ViolationCount 0.001 `
        'Valid advisory-only rows must preserve all safety invariants.'
    if ($adviceOverall.Conclusion -ne 'DIAGNOSTIC_ONLY_HOLD_SHADOW') {
        throw "Unexpected recovery advice conclusion: $($adviceOverall.Conclusion)"
    }
    $shortTailRejected = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_recovery_speed_advice.ps1') `
            -DataRoot $adviceRoot -ExpectedBuildRevision 'abc1234' `
            -MinimumActiveExitedEventsPerDirection 1 | Out-Null
    }
    catch { $shortTailRejected = $true }
    if (-not $shortTailRejected) {
        throw 'Default recovery advice analysis must require five post-exit frames.'
    }

    $rightAdvicePath = Join-Path $adviceRoot 'horizontal_right1.csv'
    $rightAdviceRows = @(Import-Csv -LiteralPath $rightAdvicePath)
    $interrupted = $rightAdviceRows[0].PSObject.Copy()
    $interrupted.AimPipelineResetGeneration = '3'
    $interrupted.AimPipelineObservationSequence = '1'
    $interrupted.RecoverySpeedAdviceExited = '0'
    $open = $rightAdviceRows[0].PSObject.Copy()
    $open.AimPipelineResetGeneration = '4'
    $open.AimPipelineObservationSequence = '1'
    $open.RecoverySpeedAdviceExited = '0'
    @($rightAdviceRows) + @($interrupted, $open) |
        Export-Csv -LiteralPath $rightAdvicePath -NoTypeInformation -Encoding UTF8
    $incompleteSummary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_recovery_speed_advice.ps1') `
        -DataRoot $adviceRoot -ExpectedBuildRevision 'abc1234' -MinimumPostExitFrames 0 `
        -MinimumActiveExitedEventsPerDirection 1)
    $incompleteRight = $incompleteSummary | Where-Object Direction -eq 'right'
    $incompleteOverall = $incompleteSummary | Where-Object Direction -eq 'overall'
    Assert-Near 1.0 $incompleteRight.InterruptedWindows 0.001 `
        'A later reset must classify a non-exited advisory window as interrupted.'
    Assert-Near 1.0 $incompleteRight.OpenWindows 0.001 `
        'The final non-exited advisory generation must remain open.'
    Assert-Near 0.0 $incompleteOverall.CoverageReady 0.001 `
        'An open direction must block overall real-data coverage.'
    if ($incompleteOverall.Conclusion -ne 'MORE_REAL_DATA_REQUIRED_HOLD_SHADOW') {
        throw "Unexpected incomplete recovery advice conclusion: $($incompleteOverall.Conclusion)"
    }
    Copy-Item -LiteralPath (Join-Path $adviceRoot 'horizontal_left1.csv') `
        -Destination (Join-Path $adviceRoot 'horizontal_left_old.csv')
    $selectedSummary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_recovery_speed_advice.ps1') `
        -DataRoot $adviceRoot -ExpectedBuildRevision 'abc1234' -MinimumPostExitFrames 0 `
        -MinimumActiveExitedEventsPerDirection 1 `
        -LeftFileName 'horizontal_left1.csv' -RightFileName 'horizontal_right1.csv')
    Assert-Near 1.0 ($selectedSummary | Where-Object Direction -eq 'left').RecoveryEvents 0.001 `
        'Explicit file selection must exclude historical same-direction CSV files.'
    Remove-Item -LiteralPath $adviceRoot -Recurse -Force

    $mixedPath = Join-Path $temporaryRoot 'horizontal_left_mixed.csv'
    $mixedRows = @(Import-Csv -LiteralPath (Join-Path $temporaryRoot 'horizontal_left1.csv'))
    $mixedRows | ForEach-Object { $_.BuildRevision = 'other-build' }
    $mixedRows | Export-Csv -LiteralPath $mixedPath -NoTypeInformation -Encoding UTF8
    $mixedRejected = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_real_reacquisition_envelope.ps1') `
            -DataRoot $temporaryRoot -RecoveryFrames 3 -MinimumEventsPerDirection 1 | Out-Null
    }
    catch { $mixedRejected = $true }
    if (-not $mixedRejected) { throw 'Mixed identities must be rejected without an explicit selector.' }
    $selectedSummary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_real_reacquisition_envelope.ps1') `
        -DataRoot $temporaryRoot -RecoveryFrames 3 -MinimumEventsPerDirection 1 `
        -ExpectedBuildRevision 'abc1234' `
        -ReferenceLeftResidualCounts 298.7 -ReferenceRightResidualCounts 301.9)
    Assert-Near 1.0 ($selectedSummary | Where-Object Scenario -eq 'overall').CoverageReady 0.001 `
        'Explicit revision selection must preserve a single-identity batch.'
    Remove-Item -LiteralPath $mixedPath -Force

    $rightPath = Join-Path $temporaryRoot 'horizontal_right1.csv'
    $rightRows = @(Import-Csv -LiteralPath $rightPath)
    $rightRows | Where-Object {
        $_.AimPipelineResetGeneration -eq '2' -and $_.AimPipelineObservationSequence -eq '4'
    } | ForEach-Object { $_.AimPipelineControlSpeedLimited = '1' }
    $rightRows | Export-Csv -LiteralPath $rightPath -NoTypeInformation -Encoding UTF8
    $blockedSummary = @(& (Join-Path $PSScriptRoot '..\tools\analyze_real_reacquisition_envelope.ps1') `
        -DataRoot $temporaryRoot -RecoveryFrames 3 -MinimumEventsPerDirection 1 `
        -ReferenceLeftResidualCounts 298.7 -ReferenceRightResidualCounts 301.9)
    $blockedOverall = $blockedSummary | Where-Object Scenario -eq 'overall'
    Assert-Near 0.0 $blockedOverall.CoverageReady 0.001 `
        'Overall gate must reject a direction whose recovery saturation never exits.'
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
