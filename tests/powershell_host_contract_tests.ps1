param(
    [Parameter(Mandatory = $true)]
    [string]$PowerShell7Executable,
    [string]$WorkflowScript = (Join-Path $PSScriptRoot `
        "..\scripts\test_release_transfer_and_aim_manual.ps1")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-HostProbe(
        [string]$PowerShellPath,
        [string]$TestRoot) {
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = (& $PowerShellPath -NoProfile -ExecutionPolicy Bypass `
        -File $WorkflowScript -TestRoot $TestRoot `
        -HostContractOnly 2>&1) -join [Environment]::NewLine
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
    '\', '/')
$probeName = "xen-ps-host-contract-{0}" -f `
    [guid]::NewGuid().ToString("N")
$probeRoot = Join-Path $temporaryParent $probeName
$testRoot = Join-Path $probeRoot "must-not-exist"
$windowsPowerShell = Join-Path $env:SystemRoot `
    "System32\WindowsPowerShell\v1.0\powershell.exe"
$powerShell7 = (Get-Item -LiteralPath $PowerShell7Executable `
    -ErrorAction Stop).FullName

try {
    $desktop = Invoke-HostProbe $windowsPowerShell $testRoot
    $core = Invoke-HostProbe $powerShell7 $testRoot
    if ($desktop.ExitCode -ne 0 -or
        $desktop.Output -notmatch "XEN_PS_HOST_SUPPORTED: Windows PowerShell 5.1" -or
        $core.ExitCode -eq 0 -or
        $core.Output -notmatch `
            "XEN_PS_HOST_UNSUPPORTED: requires Windows PowerShell 5.1" -or
        (Test-Path -LiteralPath $probeRoot)) {
        throw "PowerShell host preflight contract failed. Desktop=[$($desktop.ExitCode)] $($desktop.Output); Core=[$($core.ExitCode)] $($core.Output)"
    }
    Write-Host "PowerShell host preflight contract passed."
} finally {
    $resolvedProbe = [IO.Path]::GetFullPath($probeRoot)
    if ((Split-Path -Parent $resolvedProbe) -ine $temporaryParent -or
        (Split-Path -Leaf $resolvedProbe) -notmatch `
            '^xen-ps-host-contract-[0-9a-f]{32}$') {
        throw "Host probe cleanup lost its exact temporary owner path."
    }
    if (Test-Path -LiteralPath $resolvedProbe -PathType Container) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        if ((Split-Path -Parent $resolvedTestRoot) -ine $resolvedProbe -or
            (Split-Path -Leaf $resolvedTestRoot) -cne "must-not-exist") {
            throw "Host probe child path is not the expected direct child."
        }
        if (Test-Path -LiteralPath $resolvedTestRoot -PathType Container) {
            if (@(Get-ChildItem -LiteralPath $resolvedTestRoot -Force).Count `
                    -ne 0) {
                throw "Host probe left non-empty side effects for inspection."
            }
            [IO.Directory]::Delete($resolvedTestRoot, $false)
        }
        if (@(Get-ChildItem -LiteralPath $resolvedProbe -Force).Count -ne 0) {
            throw "Host probe left unexpected side effects for inspection."
        }
        [IO.Directory]::Delete($resolvedProbe, $false)
    }
}
