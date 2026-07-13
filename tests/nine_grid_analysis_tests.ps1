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

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('xen-nine-grid-test-' + [guid]::NewGuid().ToString('N'))
try {
    $dataDirectory = Join-Path $temporaryRoot 'CUDA\ndi'
    New-Item -ItemType Directory -Path $dataDirectory | Out-Null
    $left = [string][char]0x5DE6
    $up = [string][char]0x4E0A
    $middle = [string][char]0x4E2D
    $csvPath = Join-Path $dataDirectory ($left + '.csv')
    @'
Timestamp,SourceWidth,SourceHeight,FPS,InferenceFPS,SourceReceiveFPS,ObservationAgeSec,ErrorX,ErrorY,ErrorDistance,SpeedLimited,Settled,QueuedMoveCount
1000,2560,1440,120,110,240,0.010,100,0,100,1,0,1
1010,2560,1440,120,110,240,0.010,40,0,40,0,0,1
1020,2560,1440,120,110,240,0.010,4,0,4,0,1,0
1030,2560,1440,120,110,240,0.010,3,0,3,0,1,0
1200,2560,1440,120,110,240,0.010,-80,0,80,1,0,1
1210,2560,1440,120,110,240,0.010,3,0,3,0,1,0
1220,2560,1440,120,110,240,0.010,9,0,9,0,0,1
'@ | Set-Content -LiteralPath $csvPath -Encoding UTF8

    $outputCsv = Join-Path $temporaryRoot 'summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_nine_grid.ps1') `
        -DataRoot $temporaryRoot -SegmentGapMs 100 -OutputCsv $outputCsv -PassThru)
    $segments = @($metrics | Where-Object { $_.Level -eq 'Segment' })
    $chain = @($metrics | Where-Object { $_.Level -eq 'Chain' })

    Assert-Equal 2 $segments.Count 'A gap larger than 100 ms must create a new position segment.'
    Assert-Equal ($left + $up) $segments[0].Position 'The first left-side segment must map to upper position.'
    Assert-Equal ($left + $middle) $segments[1].Position 'The second left-side segment must map to middle position.'
    Assert-Equal 20 $segments[0].SettleMs 'First settle time must use segment-relative timestamps.'
    Assert-Equal 4 $segments[0].PostSettleMaxErrorPx 'Post-settle error must start at the first settled row.'
    Assert-Equal 1 $segments[1].SettleExitCount 'A settled-to-active transition must be counted.'
    Assert-Equal 1 $segments[1].CenterCrossings 'A main-axis sign change must be counted.'
    Assert-Equal 2 $chain[0].Segments 'Chain summary must include both position segments.'
    Assert-Equal 100 $chain[0].LimitedFarPct 'Chain limiting rate must be weighted by far-frame count.'
    Assert-Equal '2560x1440' $chain[0].SourceGeometry 'Source geometry must remain auditable.'
    $exportedRows = @(Import-Csv -LiteralPath $outputCsv)
    Assert-Equal 3 $exportedRows.Count 'CSV export must contain segment and chain rows.'
    Assert-Equal 15 $exportedRows[-1].MeanSettleMs 'CSV export must retain chain-only summary columns.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] Nine-grid analysis tests passed.' -ForegroundColor Green
