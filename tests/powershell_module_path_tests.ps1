param(
    [Parameter(Mandatory = $true)]
    [string]$CMakeCommand,
    [Parameter(Mandatory = $true)]
    [string]$CTestCommand,
    [Parameter(Mandatory = $true)]
    [string]$PowerShell7Executable,
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$Configuration
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($env:XEN_PSMODULEPATH_PROBE -ceq "1") {
    if ($PSVersionTable.PSEdition -cne "Desktop" -or
        $PSVersionTable.PSVersion.Major -ne 5) {
        throw "Module-path probe must run under Windows PowerShell 5.1."
    }
    Import-Module Microsoft.PowerShell.Utility -Force -ErrorAction Stop
    $utilityModule = Get-Module -Name Microsoft.PowerShell.Utility
    if ($null -eq $utilityModule) {
        throw "Microsoft.PowerShell.Utility did not publish module metadata."
    }
    $expectedModuleRoot = [IO.Path]::GetFullPath(
        (Join-Path $PSHOME "Modules")).TrimEnd('\', '/') + `
        [IO.Path]::DirectorySeparatorChar
    $actualModulePath = [IO.Path]::GetFullPath($utilityModule.Path)
    if (-not $actualModulePath.StartsWith(
            $expectedModuleRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Windows PowerShell loaded Utility outside its PSHOME Modules: $actualModulePath"
    }
    $stream = [IO.MemoryStream]::new()
    try {
        $hash = Get-FileHash -InputStream $stream -Algorithm SHA256 `
            -ErrorAction Stop
        if ($hash.Hash -cne
            "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855") {
            throw "Microsoft.PowerShell.Utility returned an unexpected SHA-256."
        }
    } finally {
        $stream.Dispose()
    }
    Write-Host "Windows PowerShell module-path probe passed."
    return
}

$powerShell7 = (Get-Item -LiteralPath $PowerShell7Executable `
    -ErrorAction Stop).FullName
$powerShell7Home = (& $powerShell7 -NoProfile -Command '$PSHOME').Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($powerShell7Home)) {
    throw "Cannot resolve the PowerShell 7 module directory."
}
$powerShell7Modules = Join-Path $powerShell7Home "Modules"
if (-not (Test-Path -LiteralPath $powerShell7Modules -PathType Container)) {
    throw "PowerShell 7 module directory does not exist: $powerShell7Modules"
}

$originalModulePath = [Environment]::GetEnvironmentVariable(
    "PSModulePath", "Process")
$output = @(& $CMakeCommand -E env `
    "PSModulePath=$powerShell7Modules" `
    "XEN_PSMODULEPATH_PROBE=1" `
    -- $CTestCommand --test-dir $BuildDirectory -C $Configuration `
    -R "^powershell_module_path_tests$" --output-on-failure 2>&1)
$exitCode = $LASTEXITCODE
$currentModulePath = [Environment]::GetEnvironmentVariable(
    "PSModulePath", "Process")
if ($currentModulePath -cne $originalModulePath) {
    throw "Polluted child CTest changed the parent PSModulePath."
}
if ($exitCode -ne 0) {
    throw "Polluted child CTest failed module-path closure:`n$($output -join [Environment]::NewLine)"
}

Write-Host "PowerShell module-path closure passed without parent mutation."
