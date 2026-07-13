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
Timestamp,SourceWidth,SourceHeight,InferenceFPS,SourceReceiveFPS,ObservationAgeSec,ErrorX,ErrorY,ErrorDistance,FilterResidual,ObservedVelocityX,ObservedVelocityY,SpeedLimited,QueuedMoveCount
1000,2560,1440,120,240,0.010,12,1,12.04,2,100,0,0,1
1010,2560,1440,120,240,0.010,10,1,10.05,2,100,0,0,1
1020,2560,1440,120,240,0.010,20,1,20.02,3,-100,0,1,1
1030,2560,1440,120,240,0.010,7,1,7.07,2,-100,0,0,1
1040,2560,1440,120,240,0.010,6,1,6.08,1,-100,0,0,0
1050,2560,1440,120,240,0.010,5,1,5.10,1,-100,0,0,0
1300,2560,1440,120,240,0.010,-9,1,9.06,2,-80,0,0,1
1310,2560,1440,120,240,0.010,-8,1,8.06,2,-80,0,0,1
1320,2560,1440,120,240,0.010,-7,1,7.07,2,-80,0,0,0
'@ | Set-Content -LiteralPath $csvPath -Encoding UTF8

    $outputCsv = Join-Path $temporaryRoot 'summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_moving_target.ps1') -DataRoot $temporaryRoot -WarmupMs 0 -ReversalConfirmFrames 2 -RecoveryConfirmFrames 2 -OutputCsv $outputCsv -PassThru)
    $trials = @($metrics | Where-Object { $_.Level -eq 'Trial' })
    $scenario = @($metrics | Where-Object { $_.Level -eq 'Scenario' })

    Assert-Equal 2 $trials.Count 'A gap larger than 150 ms must create a new moving trial.'
    Assert-Equal 1 $trials[0].ReversalCount 'Persistent signed velocity reversal must be counted.'
    Assert-Equal 1 $trials[0].RecoveredReversals 'Reversal recovery inside the radius must be counted.'
    Assert-Equal 10 $trials[0].RecoveryMeanMs 'Recovery time must start at the first opposite-direction sample.'
    Assert-Equal 1 $scenario.Count 'Both trials in one file must share a scenario summary.'
    Assert-Equal 2 $scenario[0].Trials 'Scenario summary must report both trials.'
    Assert-Equal 1 $scenario[0].MaxQueuedMoves 'Scenario summary must preserve maximum queue depth.'
    $exportedRows = @(Import-Csv -LiteralPath $outputCsv)
    Assert-Equal 3 $exportedRows.Count 'CSV export must include trials and scenario summary.'
    Assert-Equal 2 $exportedRows[-1].Trials 'CSV export must retain scenario-only summary columns.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] Moving-target analysis tests passed.' -ForegroundColor Green
