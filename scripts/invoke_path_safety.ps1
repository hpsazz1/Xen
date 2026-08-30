param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('New', 'Remove')]
    [string]$Action,
    [Parameter(Mandatory = $true)]
    [string]$BasePath,
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [string]$RootPath = "",
    [string]$OwnerId = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot "path_safety.psm1") -Force

if ($Action -eq 'New') {
    $owned = New-XenOwnedTestDirectory -BasePath $BasePath `
        -RepositoryRoot $RepositoryRoot
    [Console]::Out.WriteLine("{0}`t{1}", $owned.RootPath, $owned.OwnerId)
    exit 0
}

if ([string]::IsNullOrWhiteSpace($RootPath) -or
    [string]::IsNullOrWhiteSpace($OwnerId)) {
    throw "Remove requires RootPath and OwnerId."
}
Remove-XenOwnedTestDirectory -RootPath $RootPath -BasePath $BasePath `
    -RepositoryRoot $RepositoryRoot -OwnerId $OwnerId
