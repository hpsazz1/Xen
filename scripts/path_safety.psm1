Set-StrictMode -Version Latest

function Get-XenNormalizedPath([string]$Path, [string]$Description) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description is empty."
    }
    if ($Path.StartsWith('\\?\', [StringComparison]::Ordinal) -or
        $Path.StartsWith('\\.\', [StringComparison]::Ordinal)) {
        throw "$Description uses a device or extended path and is rejected: $Path"
    }
    try {
        return [IO.Path]::GetFullPath($Path)
    } catch {
        throw "$Description cannot be normalized: $Path; $($_.Exception.Message)"
    }
}

function Get-XenComparablePath([string]$Path) {
    return $Path.TrimEnd('\', '/')
}

function Get-XenPathAttributeState(
        [string]$Path,
        [string]$Description) {
    try {
        return [pscustomobject]@{
            Exists = $true
            Attributes = [IO.File]::GetAttributes($Path)
        }
    } catch {
        $cause = $_.Exception
        while ($null -ne $cause.InnerException) {
            $cause = $cause.InnerException
        }
        if ($cause -is [IO.FileNotFoundException] -or
            $cause -is [IO.DirectoryNotFoundException]) {
            return [pscustomobject]@{
                Exists = $false
                Attributes = [IO.FileAttributes]0
            }
        }
        throw "Rejected $Description because path attributes cannot be verified: $Path; $($cause.Message)"
    }
}

function Assert-XenNoReparsePathChain(
        [string]$Path,
        [string]$Description,
        [switch]$RequireExistingLeaf) {
    $normalized = Get-XenNormalizedPath $Path $Description
    $volumeRoot = [IO.Path]::GetPathRoot($normalized)
    if ([string]::IsNullOrWhiteSpace($volumeRoot)) {
        throw "Rejected $Description because its volume root is unknown: $normalized"
    }

    $current = $normalized
    $isLeaf = $true
    while ($true) {
        $state = Get-XenPathAttributeState $current $Description
        if ($isLeaf -and $RequireExistingLeaf -and -not $state.Exists) {
            throw "Rejected $Description because the path does not exist: $current"
        }
        if ($state.Exists -and
            ($state.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Rejected a reparse point in the $Description path chain: $current"
        }
        if ((Get-XenComparablePath $current) -ieq
            (Get-XenComparablePath $volumeRoot)) {
            break
        }
        try {
            $parent = [IO.Path]::GetDirectoryName($current)
        } catch {
            throw "Rejected $Description because its parent cannot be resolved: $current; $($_.Exception.Message)"
        }
        if ([string]::IsNullOrWhiteSpace($parent) -or
            (Get-XenComparablePath $parent) -ieq
                (Get-XenComparablePath $current)) {
            throw "Rejected $Description because its path chain is incomplete: $normalized"
        }
        $current = $parent
        $isLeaf = $false
    }
}

function Assert-XenSafeTestBase(
        [string]$BasePath,
        [string]$RepositoryRoot) {
    $base = Get-XenNormalizedPath $BasePath "Test base path"
    $volumeRoot = [IO.Path]::GetPathRoot($base)
    if ([string]::IsNullOrWhiteSpace($volumeRoot) -or
        (Get-XenComparablePath $base) -ieq
            (Get-XenComparablePath $volumeRoot)) {
        throw "Rejected a filesystem or share root as the test base: $base"
    }
    $repository = Get-XenNormalizedPath $RepositoryRoot "Repository root"
    if ((Get-XenComparablePath $base) -ieq
        (Get-XenComparablePath $repository)) {
        throw "Rejected the repository root as the test base: $base"
    }
    Assert-XenNoReparsePathChain $base "test base"
    if (Test-Path -LiteralPath $base) {
        $item = Get-Item -LiteralPath $base -Force
        if (-not $item.PSIsContainer) {
            throw "Test base is not a directory: $base"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Rejected a reparse-point test base: $base"
        }
    }
    return $base
}

function Assert-XenNoReparsePoints([string]$RootPath) {
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($RootPath)
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($current)) {
            $attributes = [IO.File]::GetAttributes($entry)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Rejected a reparse point inside an owned test directory: $entry"
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                $pending.Enqueue($entry)
            }
        }
    }
}

function New-XenOwnedTestDirectory(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot) {
    $base = Assert-XenSafeTestBase $BasePath $RepositoryRoot
    [IO.Directory]::CreateDirectory($base) | Out-Null
    Assert-XenNoReparsePathChain $base "test base after creation" `
        -RequireExistingLeaf

    do {
        $ownerId = [guid]::NewGuid().ToString("N")
        $root = Join-Path $base "xen-test-$ownerId"
    } while (Test-Path -LiteralPath $root)

    [IO.Directory]::CreateDirectory($root) | Out-Null
    Assert-XenNoReparsePathChain $root "owned test root after creation" `
        -RequireExistingLeaf
    $sentinel = Join-Path $root ".xen-test-owner.json"
    try {
        $document = [ordered]@{
            schema = 1
            owner_id = $ownerId
            base_path = $base
            root_path = $root
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText(
            $sentinel, $document, [Text.UTF8Encoding]::new($false))
    } catch {
        [IO.Directory]::Delete($root, $false)
        throw
    }
    Assert-XenNoReparsePathChain $root "owned test root after owner write" `
        -RequireExistingLeaf
    return [pscustomobject]@{
        RootPath = $root
        BasePath = $base
        OwnerId = $ownerId
        SentinelPath = $sentinel
    }
}

function Remove-XenOwnedTestDirectory(
        [Parameter(Mandatory = $true)]
        [string]$RootPath,
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]
        [string]$OwnerId) {
    $base = Assert-XenSafeTestBase $BasePath $RepositoryRoot
    $root = Get-XenNormalizedPath $RootPath "Owned test root"
    if ((Split-Path -Parent $root) -ine $base -or
        (Split-Path -Leaf $root) -notmatch '^xen-test-([0-9a-f]{32})$' -or
        $Matches[1] -ine $OwnerId -or
        $OwnerId -notmatch '^[0-9a-f]{32}$') {
        throw "Owned test root is not a direct GUID child of its declared base: $root"
    }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Owned test root does not exist: $root"
    }
    $rootItem = Get-Item -LiteralPath $root -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Rejected a reparse-point owned test root: $root"
    }
    $sentinel = Join-Path $root ".xen-test-owner.json"
    if (-not (Test-Path -LiteralPath $sentinel -PathType Leaf)) {
        throw "Owned test root has no owner sentinel: $root"
    }
    $sentinelItem = Get-Item -LiteralPath $sentinel -Force
    if (($sentinelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Rejected a reparse-point owner sentinel: $sentinel"
    }
    try {
        $owner = Get-Content -LiteralPath $sentinel -Raw -Encoding utf8 |
            ConvertFrom-Json
    } catch {
        throw "Owned test root has an invalid owner sentinel: $sentinel"
    }
    if ([int]$owner.schema -ne 1 -or
        [string]$owner.owner_id -ine $OwnerId -or
        (Get-XenComparablePath ([string]$owner.base_path)) -ine
            (Get-XenComparablePath $base) -or
        (Get-XenComparablePath ([string]$owner.root_path)) -ine
            (Get-XenComparablePath $root)) {
        throw "Owned test root sentinel does not match this run: $sentinel"
    }
    Assert-XenNoReparsePoints $root
    Assert-XenNoReparsePathChain $base "test base before removal" `
        -RequireExistingLeaf
    Assert-XenNoReparsePathChain $root "owned test root before removal" `
        -RequireExistingLeaf
    Remove-Item -LiteralPath $root -Recurse -Force
}

function Resolve-XenDirectChildPath(
        [Parameter(Mandatory = $true)]
        [string]$RootPath,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Description) {
    $root = Get-XenNormalizedPath $RootPath "$Description root"
    Assert-XenNoReparsePathChain $root "$Description root" `
        -RequireExistingLeaf
    if ([string]::IsNullOrWhiteSpace($Name) -or
        $Name -eq '.' -or $Name -eq '..' -or
        [IO.Path]::IsPathRooted($Name) -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
        $Name.Contains('\') -or $Name.Contains('/') -or
        $Name.EndsWith('.', [StringComparison]::Ordinal) -or
        $Name.EndsWith(' ', [StringComparison]::Ordinal) -or
        [IO.Path]::GetFileName($Name) -cne $Name -or
        $Name -match '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$') {
        throw "$Description must be one safe basename: $Name"
    }
    $child = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $Name))
    if ((Split-Path -Parent $child) -ine $root) {
        throw "$Description escapes its declared root: $Name"
    }
    Assert-XenNoReparsePathChain $child "$Description child"
    return $child
}

Export-ModuleMember -Function @(
    'New-XenOwnedTestDirectory',
    'Remove-XenOwnedTestDirectory',
    'Resolve-XenDirectChildPath')
