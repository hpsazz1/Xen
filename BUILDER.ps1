[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("DML", "CUDA", "")]
    [string]$Backend = "",
    [switch]$Help,
    [switch]$NonInteractive,
    [object]$OpenCvAlreadyBuilt = $null,
    [object]$DownloadOrUpdateNeeded = $null,
    [switch]$UseLatestPackages,
    [switch]$OpenBrowserForDownloads,
    [switch]$SkipOpenCvBuild,
    [string]$BuildDir = "",
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel", "Debug", "")]
    [string]$Configuration = "",
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Select-Backend {
    param([switch]$NonInteractive)

    if ($NonInteractive) {
        throw "Backend is required in non-interactive mode. Use -Backend DML or -Backend CUDA."
    }

    Write-Host "Select build backend"
    Write-Host "  1) DML"
    Write-Host "  2) CUDA"

    while ($true) {
        $choice = Read-Host "Choose DML or CUDA"
        if ($null -eq $choice) {
            throw "No backend was selected."
        }
        switch -Regex ($choice.Trim()) {
            "^(1|d|dml)$" { return "DML" }
            "^(2|c|cuda)$" { return "CUDA" }
        }
        Write-Host "Please enter DML, CUDA, 1, or 2."
    }
}

if ($Help -or ($BuildArgs -contains "--help") -or ($BuildArgs -contains "/?")) {
    Write-Host "Usage:"
    Write-Host "  .\BUILDER.bat"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\BUILDER.ps1 -Backend DML"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\BUILDER.ps1 -Backend CUDA"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\BUILDER.ps1 -Backend CUDA -OpenCvAlreadyBuilt false -DownloadOrUpdateNeeded true"
    Write-Host ""
    Write-Host "Any extra arguments are passed to tools/build_dml.ps1 or tools/build_cuda.ps1."
    exit 0
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Backend)) {
    $Backend = Select-Backend -NonInteractive:$NonInteractive
}

$scriptName = if ($Backend -eq "CUDA") {
    "tools\build_cuda.ps1"
}
else {
    "tools\build_dml.ps1"
}

$scriptPath = Join-Path $repoRoot $scriptName
if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "Build script not found: $scriptPath"
}

Write-Host "[BUILDER] Running $Backend build..."
$forwardedArgs = @()
if ($NonInteractive) { $forwardedArgs += "-NonInteractive" }
if ($PSBoundParameters.ContainsKey("OpenCvAlreadyBuilt")) {
    $forwardedArgs += @("-OpenCvAlreadyBuilt", [string]$OpenCvAlreadyBuilt)
}
if ($PSBoundParameters.ContainsKey("DownloadOrUpdateNeeded")) {
    $forwardedArgs += @("-DownloadOrUpdateNeeded", [string]$DownloadOrUpdateNeeded)
}
if ($UseLatestPackages) { $forwardedArgs += "-UseLatestPackages" }
if ($Backend -eq "CUDA" -and $OpenBrowserForDownloads) { $forwardedArgs += "-OpenBrowserForDownloads" }
if ($Backend -eq "CUDA" -and $SkipOpenCvBuild) { $forwardedArgs += "-SkipOpenCvBuild" }
if (-not [string]::IsNullOrWhiteSpace($BuildDir)) {
    $forwardedArgs += @("-BuildDir", $BuildDir)
}
if (-not [string]::IsNullOrWhiteSpace($Configuration)) {
    $forwardedArgs += @("-Configuration", $Configuration)
}
if ($DryRun) { $forwardedArgs += "-DryRun" }

& powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @forwardedArgs @BuildArgs
exit $LASTEXITCODE
