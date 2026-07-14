[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param([object]$Expected, [object]$Actual, [string]$Message)
    if ([string]$Expected -ne [string]$Actual) {
        throw "$Message Expected='$Expected', Actual='$Actual'."
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('xen-dml-performance-' + [guid]::NewGuid().ToString('N'))
try {
    $dataDirectory = Join-Path $temporaryRoot 'DML\ndi'
    New-Item -ItemType Directory -Path $dataDirectory | Out-Null
    $csvPath = Join-Path $dataDirectory 'performance.csv'
    @'
BuildBackend,BuildRevision,InferenceFPS,SourceReceiveFPS,ObservationAgeSec,DmlPreprocessMs,DmlInferenceMs,DmlCopyMs,DmlPostprocessMs,DmlNmsMs,DmlTotalMs
DML,test-revision,100,220,0.010,1,7,0.1,1.9,0.4,10
DML,test-revision,100,220,0.012,2,8,0.2,1.8,0.5,12
DML,test-revision,100,220,0.014,3,9,0.3,1.7,0.6,14
'@ | Set-Content -LiteralPath $csvPath -Encoding UTF8

    $outputCsv = Join-Path $temporaryRoot 'dml_performance_summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_dml_performance.ps1') -DataRoot $temporaryRoot -MinSamples 1 -OutputCsv $outputCsv -PassThru)
    Assert-Equal 2 $metrics.Count 'One file and one overall summary must be emitted.'
    $overall = @($metrics | Where-Object Level -eq 'Overall')[0]
    Assert-Equal 3 $overall.Samples 'Overall summary must retain all valid rows.'
    Assert-Equal 2 $overall.MeanPreprocessMs 'Mean preprocess time must be correct.'
    Assert-Equal 8 $overall.MeanInferenceMs 'Mean inference time must be correct.'
    Assert-Equal 12 $overall.MeanTotalMs 'Mean total time must be correct.'
    Assert-Equal 12 $overall.P95TotalMs 'Floor percentile must be deterministic for three samples.'
    Assert-Equal 66.7 $overall.InferenceSharePct 'Inference share must use total pipeline time.'
    Assert-Equal 83.3 $overall.ImpliedPipelineFps 'Implied throughput must derive from mean total time.'
    Assert-Equal 12 $overall.ObservationAgeP95Ms 'Observation age P95 must use the deterministic floor percentile.'
    Assert-Equal test-revision $overall.BuildRevision 'Build revision must be preserved.'
    Assert-Equal 2 @(Import-Csv -LiteralPath $outputCsv).Count 'Machine-readable export must include both summaries.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] DML performance analysis tests passed.' -ForegroundColor Green
