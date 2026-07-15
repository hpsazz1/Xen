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

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('xen-shadow-analysis-' + [guid]::NewGuid().ToString('N'))
$header = 'BuildBackend,BuildRevision,ControllerRevision,AimPipelineRequestedMode,AimPipelineEffectiveMode,AimPipelineActiveAvailable,AimPipelineShadowProcessed,AimPipelineCommandSuppressed,AimPipelineEstimateValid,AimPipelineCovarianceX,AimPipelineCovarianceY,AimPipelineInnovationVarianceX,AimPipelineInnovationVarianceY,AimPipelineNisX,AimPipelineNisY,AimPipelineTrackingFeedforwardX,AimPipelineTrackingFeedforwardY,AimPipelineLeadCountsX,AimPipelineLeadCountsY,AimPipelineIntegralCountsX,AimPipelineIntegralCountsY,TrajectoryShaperMode,AimPipelineTrajectoryCommandSuppressed,TimingComplete,TimingOrderValid'
$validRow = 'DML,test-revision,30,shadow,shadow,0,1,1,1,0.02,0.03,0.04,0.05,0.2,0.3,0,0,0,0,0,0,off,1,1,1'
try {
    $validRoot = Join-Path $temporaryRoot 'valid'
    New-Item -ItemType Directory -Path $validRoot | Out-Null
    foreach ($scenario in @('static', 'horizontal_left', 'horizontal_right', 'horizontal_reverse', 'horizontal_jump')) {
        @($header, $validRow, $validRow) | Set-Content `
            -LiteralPath (Join-Path $validRoot "$scenario.csv") -Encoding UTF8
    }
    $outputCsv = Join-Path $validRoot 'shadow_pipeline_summary.csv'
    $metrics = @(& (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
        -DataRoot $validRoot -MinEstimateSamples 2 -ExpectedBuildRevision test-revision `
        -OutputCsv $outputCsv -RequireStandardScenarios -PassThru)
    Assert-Equal 6 $metrics.Count 'Five scenarios and one overall shadow summary must be emitted.'
    Assert-Equal PASS $metrics[-1].Status 'Valid shadow data must pass.'
    Assert-Equal 10 $metrics[-1].EstimateSamples 'Estimate samples must be counted.'

    $invalidRoot = Join-Path $temporaryRoot 'invalid'
    New-Item -ItemType Directory -Path $invalidRoot | Out-Null
    $invalidRow = $validRow -replace ',shadow,shadow,0,1,1,', ',active,shadow,0,1,0,'
    @($header, $invalidRow) | Set-Content -LiteralPath (Join-Path $invalidRoot 'unsafe.csv') -Encoding UTF8
    $failed = $false
    try {
        & (Join-Path $PSScriptRoot '..\tools\analyze_shadow_pipeline.ps1') `
            -DataRoot $invalidRoot -MinEstimateSamples 1 -ExpectedBuildRevision test-revision | Out-Null
    }
    catch { $failed = $true }
    Assert-Equal True $failed 'Unsafe shadow mode contract must fail validation.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] P0-6 shadow pipeline analysis tests passed.' -ForegroundColor Green
