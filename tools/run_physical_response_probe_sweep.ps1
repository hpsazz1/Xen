[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProbeExe,
    [Parameter(Mandatory = $true)]
    [string]$Config,
    [Parameter(Mandatory = $true)]
    [string]$NdiSource,
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,
    [int[]]$Counts = @(8, 16, 32, 64),
    [int]$RoiX = 120,
    [int]$RoiY = 100,
    [int]$RoiWidth = 80,
    [int]$RoiHeight = 80,
    [int]$Repeats = 10,
    [int]$BaselineMs = 300,
    [int]$TailMs = 500,
    [int]$IntervalMs = 500,
    [switch]$ContinueOnFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$probePath = [System.IO.Path]::GetFullPath($ProbeExe)
$configPath = [System.IO.Path]::GetFullPath($Config)
$outputPath = [System.IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) { throw "Probe executable not found: $probePath" }
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { throw "Config file not found: $configPath" }
$countList = @($Counts)
$invalidCounts = @($countList | Where-Object { $_ -le 0 })
if ($countList.Count -eq 0 -or $invalidCounts.Count -gt 0) { throw 'Counts must contain positive integers.' }

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$results = [System.Collections.Generic.List[object]]::new()
foreach ($count in $countList) {
    $trialOutput = Join-Path $outputPath ("counts_{0}" -f $count)
    if (Test-Path -LiteralPath $trialOutput) {
        throw "Output directory already exists; choose a new OutputRoot: $trialOutput"
    }

    $arguments = @(
        '--config', $configPath,
        '--ndi-source', $NdiSource,
        '--output-dir', $trialOutput,
        '--roi-x', $RoiX,
        '--roi-y', $RoiY,
        '--roi-width', $RoiWidth,
        '--roi-height', $RoiHeight,
        '--axis', 'both',
        '--counts', $count,
        '--repeats', $Repeats,
        '--baseline-ms', $BaselineMs,
        '--tail-ms', $TailMs,
        '--interval-ms', $IntervalMs,
        '--confirm-device-motion', 'YES'
    )
    Write-Host "[probe] counts=$count output=$trialOutput" -ForegroundColor Cyan
    & $probePath @arguments
    $exitCode = $LASTEXITCODE
    $results.Add([pscustomobject]@{ Counts = $count; Output = $trialOutput; ExitCode = $exitCode })
    if ($exitCode -ne 0 -and -not $ContinueOnFailure) {
        throw "Probe failed for counts=$count with exit code $exitCode. Output was preserved at $trialOutput"
    }
}

$results | Format-Table -AutoSize
