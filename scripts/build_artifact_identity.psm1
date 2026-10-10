Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1')

function Assert-XenLinkedArtifactIdentity {
    param([Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$Runtime,
        [Parameter(Mandatory = $true)][string]$Commit,
        [string]$Configuration = 'Release')

    $stampPath = $Path + '.identity.json'
    Assert-XenNoReparsePathChain $Path 'linked artifact' -RequireExistingLeaf
    Assert-XenNoReparsePathChain $stampPath 'linked artifact identity' -RequireExistingLeaf
    $stamp = Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $required = @('schema', 'source_root', 'git_commit', 'git_dirty', 'runtime',
        'configuration', 'artifact', 'size', 'sha256')
    if (@($required | Where-Object { $_ -cnotin $stamp.PSObject.Properties.Name }).Count -ne 0 -or
        $stamp.schema -ne 1 -or $stamp.git_dirty -isnot [bool] -or $stamp.git_dirty -or
        $stamp.git_commit -cne $Commit -or $stamp.runtime -cne $Runtime -or
        $stamp.configuration -cne $Configuration -or
        [IO.Path]::GetFullPath([string]$stamp.source_root).TrimEnd('\', '/') -ine
            [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\', '/') -or
        $stamp.artifact -cne [IO.Path]::GetFileName($Path) -or
        [long]$stamp.size -ne (Get-Item -LiteralPath $Path).Length -or
        $stamp.sha256 -cne (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw "Linked artifact identity does not match current clean source and payload: $Path"
    }
    return $stamp
}

Export-ModuleMember -Function Assert-XenLinkedArtifactIdentity
