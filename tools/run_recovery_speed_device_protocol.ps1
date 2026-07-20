[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ExecutorExe,
    [Parameter(Mandatory = $true)][string]$PlanDirectory,
    [string]$OutputDirectory = '',
    [ValidateSet('YES', 'NO')][string]$ConfirmDeviceMotion = 'NO',
    [ValidateSet('YES', 'NO')][string]$ConfirmZeroTargetRisk = 'NO',
    [ValidateSet('YES', 'NO')][string]$ConfirmEmergencyStopReady = 'NO',
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$executor = [IO.Path]::GetFullPath($ExecutorExe)
$planRoot = [IO.Path]::GetFullPath($PlanDirectory)
if (-not (Test-Path -LiteralPath $executor -PathType Leaf)) { throw "Executor not found: $executor" }
if (-not (Test-Path -LiteralPath $planRoot -PathType Container)) { throw "Plan directory not found: $planRoot" }

$auditor = Join-Path $PSScriptRoot 'test_recovery_speed_device_plan.ps1'
& $auditor -PlanDirectory $planRoot -RequirePass | Out-Null
$manifestPath = Join-Path $planRoot 'recovery_speed_device_manifest.txt'
$manifest = @{}
foreach ($line in Get-Content -LiteralPath $manifestPath -Encoding UTF8) {
    $parts = $line -split '=', 2
    if ($parts.Count -ne 2) { throw "Invalid manifest line: $line" }
    $manifest[$parts[0]] = $parts[1]
}
if ($ValidateOnly) {
    & $executor --plan-dir $planRoot --validate-only
    if ($LASTEXITCODE -ne 0) { throw "Executor plan validation failed with exit code $LASTEXITCODE" }
    return
}

if ($ConfirmDeviceMotion -ne 'YES' -or $ConfirmZeroTargetRisk -ne 'YES' -or
    $ConfirmEmergencyStopReady -ne 'YES') {
    throw 'Physical execution requires all three confirmations to be YES.'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { throw 'OutputDirectory is required for physical execution.' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "OutputDirectory already exists: $output" }

& $executor --plan-dir $planRoot --output-dir $output --confirm-plan-id $manifest.PlanId `
    --confirm-device-motion YES --confirm-zero-target-risk YES --confirm-emergency-stop-ready YES
$executorExitCode = $LASTEXITCODE
$summaryPath = Join-Path $output 'recovery_speed_device_summary.csv'
if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
    $analyzer = Join-Path $PSScriptRoot 'analyze_recovery_speed_device_protocol.ps1'
    & $analyzer -DataRoot $output -RequirePass:($executorExitCode -eq 0) | Out-Host
}
if ($executorExitCode -ne 0) {
    throw "Recovery speed device executor stopped with exit code $executorExitCode; preserved output: $output"
}
