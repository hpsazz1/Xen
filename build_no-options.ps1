[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet("DML", "CUDA", "")]
    [string]$Backend = "",
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Help,
    [switch]$NonInteractive,
    [switch]$DryRun
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

if ($Help) {
    Write-Host "Usage:"
    Write-Host "  .\build_no-options.bat"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build_no-options.ps1 -Backend DML"
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\build_no-options.ps1 -Backend CUDA"
    Write-Host ""
    Write-Host "Runs cmake --build against an existing build tree. Dependencies and OpenCV must already be prepared."
    exit 0
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $repoRoot "tools\build_common.ps1")

if ([string]::IsNullOrWhiteSpace($Backend)) {
    $Backend = Select-Backend -NonInteractive:$NonInteractive
}

$buildDir = if ($Backend -eq "CUDA") {
    "build\cuda"
}
else {
    "build\dml"
}

$buildPath = Join-Path $repoRoot $buildDir
if (-not (Test-Path -LiteralPath $buildPath)) {
    throw "Build tree not found: $buildPath. Run BUILDER first to prepare dependencies and configure this backend."
}

Write-Host "[build_no-options] Running $Backend main-program build..."

$buildArgs = @(
    "--build", $buildPath,
    "--config", $Configuration,
    "--target", "ai",
    "--parallel"
)

if ($DryRun) {
    Write-Host ">> cmake $($buildArgs -join ' ')"
    exit 0
}

Import-VisualStudioEnvironment
& "cmake" @buildArgs
exit $LASTEXITCODE
