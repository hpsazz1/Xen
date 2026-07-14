[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DataRoot,
    [ValidateRange(1, 1000000)][int]$MinSamples = 30,
    [string]$OutputCsv = '',
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PercentileValue {
    param(
        [Parameter(Mandatory)][double[]]$Values,
        [Parameter(Mandatory)][ValidateRange(0.0, 1.0)][double]$Percentile
    )
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    return [double]$sorted[[math]::Floor($Percentile * ($sorted.Count - 1))]
}

function New-PerformanceSummary {
    param(
        [Parameter(Mandatory)][string]$Level,
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][object[]]$Rows
    )
    $preprocess = @($Rows | ForEach-Object { [double]$_.DmlPreprocessMs })
    $tensorSetup = @($Rows | ForEach-Object { [double]$_.DmlTensorSetupMs })
    $inference = @($Rows | ForEach-Object { [double]$_.DmlInferenceMs })
    $copy = @($Rows | ForEach-Object { [double]$_.DmlCopyMs })
    $postprocess = @($Rows | ForEach-Object { [double]$_.DmlPostprocessMs })
    $nms = @($Rows | ForEach-Object { [double]$_.DmlNmsMs })
    $total = @($Rows | ForEach-Object { [double]$_.DmlTotalMs })
    $meanTotal = [double]($total | Measure-Object -Average).Average
    $meanInference = [double]($inference | Measure-Object -Average).Average
    $observationAges = @($Rows | ForEach-Object { 1000.0 * [double]$_.ObservationAgeSec })

    [pscustomobject]@{
        Level = $Level
        Source = $Source
        Samples = $Rows.Count
        MeanPreprocessMs = [math]::Round([double]($preprocess | Measure-Object -Average).Average, 3)
        P95PreprocessMs = [math]::Round((Get-PercentileValue -Values $preprocess -Percentile 0.95), 3)
        MeanTensorSetupMs = [math]::Round([double]($tensorSetup | Measure-Object -Average).Average, 3)
        P95TensorSetupMs = [math]::Round((Get-PercentileValue -Values $tensorSetup -Percentile 0.95), 3)
        MeanInferenceMs = [math]::Round($meanInference, 3)
        P95InferenceMs = [math]::Round((Get-PercentileValue -Values $inference -Percentile 0.95), 3)
        MeanCopyMs = [math]::Round([double]($copy | Measure-Object -Average).Average, 3)
        P95CopyMs = [math]::Round((Get-PercentileValue -Values $copy -Percentile 0.95), 3)
        MeanPostprocessMs = [math]::Round([double]($postprocess | Measure-Object -Average).Average, 3)
        P95PostprocessMs = [math]::Round((Get-PercentileValue -Values $postprocess -Percentile 0.95), 3)
        MeanNmsMs = [math]::Round([double]($nms | Measure-Object -Average).Average, 3)
        P95NmsMs = [math]::Round((Get-PercentileValue -Values $nms -Percentile 0.95), 3)
        MeanTotalMs = [math]::Round($meanTotal, 3)
        P95TotalMs = [math]::Round((Get-PercentileValue -Values $total -Percentile 0.95), 3)
        InferenceSharePct = [math]::Round(100.0 * $meanInference / [math]::Max(0.001, $meanTotal), 1)
        ImpliedPipelineFps = [math]::Round(1000.0 / [math]::Max(0.001, $meanTotal), 1)
        PublishedInferenceFps = [math]::Round([double]($Rows | Measure-Object InferenceFPS -Average).Average, 1)
        SourceReceiveFps = [math]::Round([double]($Rows | Measure-Object SourceReceiveFPS -Average).Average, 1)
        ObservationAgeP95Ms = [math]::Round((Get-PercentileValue -Values $observationAges -Percentile 0.95), 1)
        DmlModel = (@($Rows.DmlModel | Select-Object -Unique) -join ';')
        DmlInputWidth = (@($Rows.DmlInputWidth | Select-Object -Unique) -join ';')
        DmlInputHeight = (@($Rows.DmlInputHeight | Select-Object -Unique) -join ';')
        BuildRevision = (@($Rows.BuildRevision | Select-Object -Unique) -join ';')
    }
}

$resolvedRoot = [System.IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    throw "DML performance data root does not exist: $resolvedRoot"
}

$requiredColumns = @(
    'BuildBackend', 'BuildRevision', 'DmlModel', 'DmlInputWidth', 'DmlInputHeight',
    'InferenceFPS', 'SourceReceiveFPS', 'ObservationAgeSec',
    'DmlPreprocessMs', 'DmlTensorSetupMs', 'DmlInferenceMs', 'DmlCopyMs',
    'DmlPostprocessMs', 'DmlNmsMs', 'DmlTotalMs'
)
$fileSummaries = [System.Collections.Generic.List[object]]::new()
$allRows = [System.Collections.Generic.List[object]]::new()
$rootPrefix = $resolvedRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar

foreach ($csvFile in @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Filter '*.csv' | Sort-Object FullName)) {
    if ($csvFile.Name -eq 'dml_performance_summary.csv') { continue }
    $rows = @(Import-Csv -LiteralPath $csvFile.FullName)
    if ($rows.Count -eq 0) { continue }
    foreach ($column in $requiredColumns) {
        if ($rows[0].PSObject.Properties.Name -notcontains $column) {
            Write-Warning "Skipping CSV without DML stage timing column '$column': $($csvFile.FullName)"
            $rows = @()
            break
        }
    }
    if ($rows.Count -eq 0) { continue }
    $validRows = @($rows | Where-Object {
        $_.BuildBackend -eq 'DML' -and [double]$_.DmlTotalMs -gt 0.0
    })
    if ($validRows.Count -lt $MinSamples) {
        Write-Warning "Skipping DML performance CSV with insufficient valid samples: $($csvFile.FullName)"
        continue
    }
    foreach ($row in $validRows) { $allRows.Add($row) }
    $relativePath = if ($csvFile.FullName.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        $csvFile.FullName.Substring($rootPrefix.Length)
    }
    else {
        $csvFile.Name
    }
    $fileSummaries.Add((New-PerformanceSummary -Level 'File' -Source $relativePath -Rows $validRows))
}

if ($allRows.Count -lt $MinSamples) {
    throw "No DML performance CSV with at least $MinSamples valid samples was found under: $resolvedRoot"
}

$overall = New-PerformanceSummary -Level 'Overall' -Source 'all' -Rows @($allRows)
$metrics = @($fileSummaries) + @($overall)

if (-not [string]::IsNullOrWhiteSpace($OutputCsv)) {
    $resolvedOutput = [System.IO.Path]::GetFullPath($OutputCsv)
    $outputDirectory = Split-Path -Parent $resolvedOutput
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory) -and -not (Test-Path -LiteralPath $outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory | Out-Null
    }
    $metrics | Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
}

if ($PassThru) {
    $metrics
    return
}

$metrics | Format-Table Level, Source, Samples, MeanPreprocessMs, MeanTensorSetupMs, MeanInferenceMs, MeanPostprocessMs, MeanTotalMs, P95TotalMs, InferenceSharePct, ImpliedPipelineFps -AutoSize
