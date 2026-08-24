param(
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [string]$OutputPath = "",
    [ValidateRange(0.0, 100.0)][double]$HoldBandPixels = 2.25
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "aim_report.ps1")
. (Join-Path $PSScriptRoot "aim_control_diagnostics.ps1")

function Replace-FileAtomically([string]$Pending, [string]$Target) {
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $backup = "$Target.backup-$([guid]::NewGuid().ToString('N'))"
        try {
            [System.IO.File]::Replace($Pending, $Target, $backup)
        } finally {
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
    } else {
        Move-Item -LiteralPath $Pending -Destination $Target
    }
}

$resolvedRun = (Resolve-Path -LiteralPath $RunDirectory -ErrorAction Stop).Path
$automaticRoot = Join-Path $resolvedRun "automatic"
if (-not (Test-Path -LiteralPath $automaticRoot -PathType Container)) {
    throw "Run 尚无 automatic 报告目录：$automaticRoot"
}
$reports = @(Get-ChildItem -LiteralPath $automaticRoot -File -Filter "*.json" |
    Sort-Object FullName)
$samples = [System.Collections.Generic.List[object]]::new()
$evidence = @()
foreach ($file in $reports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ([int]$report.schema -ne 8 -or $null -eq $report.samples) {
        continue
    }
    $reportSamples = @($report.samples)
    if ($reportSamples.Count -ne [int]$report.sample_count) {
        throw "Runtime JSON 样本数与声明不一致：$($file.FullName)"
    }
    foreach ($sample in $reportSamples) { $samples.Add($sample) }
    $evidence += [ordered]@{
        path = $file.FullName
        size = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        sample_count = $reportSamples.Count
    }
}
if ($samples.Count -eq 0) {
    throw "Run 没有可分析的 schema 8 Runtime JSON。"
}

$taskPath = Join-Path $resolvedRun "task.json"
$task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$document = [ordered]@{
    schema = 1
    task_id = [string]$task.task_id
    run_id = [string]$task.run_id
    generated_utc = [DateTimeOffset]::UtcNow.ToString("o")
    source_reports = $evidence
    control = Get-XenAimControlDiagnosticsSummary $samples.ToArray() `
        -HoldBandPixels $HoldBandPixels
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $resolvedRun "control-diagnostics-summary.json"
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$parent = Split-Path -Parent $resolvedOutput
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$pending = "$resolvedOutput.pending-$([guid]::NewGuid().ToString('N'))"
try {
    $document | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $pending -Encoding UTF8
    Replace-FileAtomically $pending $resolvedOutput
} finally {
    if (Test-Path -LiteralPath $pending -PathType Leaf) {
        Remove-Item -LiteralPath $pending -Force
    }
}
Write-Host "Aim 控制停发诊断已发布：$resolvedOutput"
Write-Host "  controllable_frames=$($document.control.controllable_frames)"
Write-Host "  x_zero_frames=$($document.control.x.command_zero_frames)"
Write-Host "  x_reversals=$($document.control.x.nonzero_direction_reversals)"
Write-Output $resolvedOutput
