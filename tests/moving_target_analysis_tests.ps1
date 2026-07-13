[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [Parameter(Mandatory)][string]$Message
    )
    if ([string]$Expected -ne [string]$Actual) {
        throw "$Message Expected='$Expected', Actual='$Actual'."
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('xen-moving-test-' + [guid]::NewGuid().ToString('N'))
try {
    $dataDirectory = Join-Path $temporaryRoot 'CUDA\udp'
    New-Item -ItemType Directory -Path $dataDirectory | Out-Null
    $csvPath = Join-Path $dataDirectory 'horizontal_reverse.csv'
    @'
Timestamp,SourceWidth,SourceHeight,InferenceFPS,SourceReceiveFPS,ObservationAgeSec,ErrorX,ErrorY,ErrorDistance,FilterResidual,ObservedVelocityX,ObservedVelocityY,RequestedPixelX,RequestedPixelY,RequestedCountsX,RequestedCountsY,FinalMx,FinalMy,SpeedLimited,QueuedMoveCount
1000,2560,1440,120,240,0.010,12,1,12.04,2,100,0,1,0,1.344,0,1,0,0,1
1010,2560,1440,120,240,0.010,11,1,11.05,2,100,0,1,0,1.344,0,1,0,0,1
1020,2560,1440,120,240,0.010,20,1,20.02,3,-100,0,1,0,1.344,0,1,0,1,1
1030,2560,1440,120,240,0.010,7,1,7.07,2,-100,0,1,0,1.344,0,1,0,0,1
1040,2560,1440,120,240,0.010,0,1,1.00,1,-100,0,1,0,1.344,0,1,0,0,0
1050,2560,1440,120,240,0.010,-10,1,10.05,1,-100,0,-1,0,-1.344,0,-1,0,0,0
1060,2560,1440,120,240,0.010,-12,1,12.04,1,-100,0,-1,0,-1.344,0,-1,0,0,0
1300,2560,1440,120,240,0.010,-9,1,9.06,2,-80,0,-1,0,-1.344,0,-1,0,0,1
1310,2560,1440,120,240,0.010,-8,1,8.06,2,-80,0,-1,0,-1.344,0,-1,0,0,1
1320,2560,1440,120,240,0.010,-7,1,7.07,2,-80,0,-1,0,-1.344,0,-1,0,0,0
'@ | Set-Content -LiteralPath $csvPath -Encoding UTF8

    $singleDirectionPath = Join-Path $dataDirectory 'horizontal_right.csv'
    Copy-Item -LiteralPath $csvPath -Destination $singleDirectionPath

    $outputCsv = Join-Path $temporaryRoot 'summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -ReversalErrorThresholdPx 8 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -OutputCsv $outputCsv -PassThru)
    $trials = @($metrics | Where-Object { $_.Level -eq 'Trial' })
    $scenario = @($metrics | Where-Object { $_.Level -eq 'Scenario' })

    Assert-Equal 4 $trials.Count 'A gap larger than 150 ms must create two trials per scenario.'
    $reverseTrials = @($trials | Where-Object { $_.Scenario -eq 'horizontal_reverse' })
    $singleDirectionTrials = @($trials | Where-Object { $_.Scenario -eq 'horizontal_right' })
    Assert-Equal 1 $reverseTrials[0].ReversalCount 'Persistent tracking-error side reversal must be counted.'
    Assert-Equal 1 $reverseTrials[0].RecoveredReversals 'Reversal recovery inside the radius must be counted.'
    Assert-Equal 10 $reverseTrials[0].RecoveryMeanMs 'Recovery time must start at the first opposite-direction sample.'
    Assert-Equal 0 $singleDirectionTrials[0].ReversalCount 'Single-direction scenarios must not report reversal metrics.'
    Assert-Equal 1.344 $reverseTrials[0].EstimatedCountsPerPixel 'Counts-per-pixel must be derived from exported requests.'
    Assert-Equal 2 $scenario.Count 'Each scenario file must create one summary.'
    Assert-Equal 2 $scenario[0].Trials 'Scenario summary must report both trials.'
    Assert-Equal 1 $scenario[0].MaxQueuedMoves 'Scenario summary must preserve maximum queue depth.'
    $exportedRows = @(Import-Csv -LiteralPath $outputCsv)
    Assert-Equal 6 $exportedRows.Count 'CSV export must include trials and scenario summaries.'
    Assert-Equal 2 $exportedRows[-1].Trials 'CSV export must retain scenario-only summary columns.'
    $rerunMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 0 -MinTrialSamples 1 -ReversalErrorThresholdPx 8 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -OutputCsv $outputCsv -PassThru)
    Assert-Equal 6 $rerunMetrics.Count 'Repeated analysis must ignore its own root-level summary CSV.'

    $filteredMetrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -MinTrialDurationMs 50 -MinTrialSamples 5 -PassThru -WarningAction SilentlyContinue)
    $filteredTrials = @($filteredMetrics | Where-Object { $_.Level -eq 'Trial' })
    Assert-Equal 2 $filteredTrials.Count 'Short moving fragments must be excluded from scenario metrics.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] Moving-target analysis tests passed.' -ForegroundColor Green
