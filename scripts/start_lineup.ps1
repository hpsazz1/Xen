param(
    [string]$XenRoot = (Join-Path $PSScriptRoot '..'),
    [string]$ConfigPath,
    [string]$Executable = (Join-Path $PSScriptRoot '../build/lineup/Release/XenLineup.exe'),
    [string]$DataDirectory = (Join-Path $PSScriptRoot '../lineup-data'),
    [string]$BindAddress = '127.0.0.1',
    [int]$Port = 8879,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# XenRoot points at the existing Xen package. Never copy, generate or rewrite its INI.
if (-not $ConfigPath) { $ConfigPath = Join-Path $XenRoot 'config.ini' }
$config = [IO.Path]::GetFullPath($ConfigPath)
$binary = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Lineup executable missing: $binary. Build with scripts/build_lineup.ps1."
}
$version = & $binary --version
if ($LASTEXITCODE -ne 0 -or $version -ne 'lineup-increment-20261005-2') {
    throw 'Lineup executable does not support this shared-config launcher; rebuild with scripts/build_lineup.ps1.'
}
Write-Host "XenLineup $version | shared INI: $config | GSI: shared (read-only)"
$arguments = @('--config', $config, '--data', [IO.Path]::GetFullPath($DataDirectory),
    '--bind', $BindAddress, '--port', "$Port")
if ($CheckOnly) { $arguments += '--check-config' }
& $binary @arguments
if ($LASTEXITCODE -ne 0) { throw "Lineup returned $LASTEXITCODE; see the configuration/startup reason above." }
