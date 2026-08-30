param(
    [string]$PathSafetyScript = (Join-Path $PSScriptRoot `
        "..\scripts\invoke_path_safety.ps1"),
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot "..")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$currentPowerShell = if ($PSVersionTable.PSEdition -eq "Desktop") {
    Join-Path $PSHOME "powershell.exe"
} else {
    Join-Path $PSHOME "pwsh.exe"
}
if (-not (Test-Path -LiteralPath $currentPowerShell -PathType Leaf)) {
    throw "Cannot resolve the current PowerShell host: $currentPowerShell"
}

function Invoke-PathSafety(
        [string]$Action,
        [string]$BasePath,
        [string]$RootPath = "",
        [string]$OwnerId = "") {
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", $PathSafetyScript,
        "-Action", $Action,
        "-BasePath", $BasePath,
        "-RepositoryRoot", $RepositoryRoot)
    if ($RootPath) {
        $arguments += @("-RootPath", $RootPath)
    }
    if ($OwnerId) {
        $arguments += @("-OwnerId", $OwnerId)
    }
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = (& $currentPowerShell @arguments 2>&1) -join `
        [Environment]::NewLine
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function New-TestJunction(
        [string]$Path,
        [string]$Target) {
    New-Item -ItemType Junction -Path $Path -Value $Target `
        -ErrorAction Stop | Out-Null
    $attributes = [IO.File]::GetAttributes($Path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        throw "Local junction fixture was not created as a reparse point: $Path"
    }
}

function Remove-LocalJunctionFixture(
        [string]$LinkPath,
        [string]$TargetPath,
        [string]$ExpectedParent) {
    $parent = [IO.Path]::GetFullPath($ExpectedParent)
    $link = [IO.Path]::GetFullPath($LinkPath)
    $target = [IO.Path]::GetFullPath($TargetPath)
    if ((Split-Path -Parent $link) -ine $parent -or
        (Split-Path -Parent $target) -ine $parent -or
        (Split-Path -Leaf $link) -notmatch '^junction-[a-z-]+$' -or
        (Split-Path -Leaf $target) -notmatch '^junction-[a-z-]+-target$') {
        throw "Junction fixture cleanup escaped its GUID-owned parent."
    }
    if (Test-Path -LiteralPath $link) {
        $attributes = [IO.File]::GetAttributes($link)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            throw "Refusing to remove a non-junction fixture path: $link"
        }
        [IO.Directory]::Delete($link, $false)
    }
    if (Test-Path -LiteralPath $target -PathType Container) {
        $entries = @(Get-ChildItem -LiteralPath $target -Force -Recurse |
            Sort-Object { $_.FullName.Length } -Descending)
        foreach ($entry in $entries) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) `
                    -ne 0) {
                throw "Refusing to traverse a nested reparse fixture: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                [IO.Directory]::Delete($entry.FullName, $false)
            } else {
                [IO.File]::Delete($entry.FullName)
            }
        }
        [IO.Directory]::Delete($target, $false)
    }
}

$repository = [IO.Path]::GetFullPath($RepositoryRoot)
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
    '\', '/')
$baseName = "xen-path-safety-base-{0}" -f `
    [guid]::NewGuid().ToString("N")
$base = Join-Path $temporaryParent $baseName
[IO.Directory]::CreateDirectory($base) | Out-Null
$nonOwned = ""
$keep = ""

try {
    $driveRoot = [IO.Path]::GetPathRoot($repository)
    foreach ($forbidden in @(
            $driveRoot,
            '\\xen-path-safety.invalid\share\',
            ("\\?\{0}" -f $driveRoot),
            ("\\.\{0}" -f $driveRoot),
            $repository)) {
        $result = Invoke-PathSafety "New" $forbidden
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch "reject") {
            throw "Path owner seam did not reject a dangerous test root: $forbidden; $($result.Output)"
        }
    }

    $nonOwned = Join-Path $base "existing-non-owned"
    [IO.Directory]::CreateDirectory($nonOwned) | Out-Null
    $keep = Join-Path $nonOwned "keep.txt"
    [IO.File]::WriteAllText($keep, "keep")
    $result = Invoke-PathSafety "Remove" $base $nonOwned `
        ([guid]::NewGuid().ToString("N"))
    if ($result.ExitCode -eq 0 -or
        -not (Test-Path -LiteralPath $keep -PathType Leaf) -or
        $result.Output -notmatch "Owned test root") {
        throw "Existing non-owned directory was not rejected and preserved: $($result.Output)"
    }

    $junctionFailures = [System.Collections.Generic.List[string]]::new()

    $baseTarget = Join-Path $base "junction-base-target"
    $baseLink = Join-Path $base "junction-base"
    [IO.Directory]::CreateDirectory($baseTarget) | Out-Null
    New-TestJunction $baseLink $baseTarget
    try {
        $result = Invoke-PathSafety "New" $baseLink
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch "reparse") {
            $junctionFailures.Add(
                "existing base junction was accepted: $($result.Output)")
        }
    } finally {
        Remove-LocalJunctionFixture $baseLink $baseTarget $base
    }

    $ancestorTarget = Join-Path $base "junction-ancestor-target"
    $ancestorLink = Join-Path $base "junction-ancestor"
    [IO.Directory]::CreateDirectory($ancestorTarget) | Out-Null
    New-TestJunction $ancestorLink $ancestorTarget
    try {
        $unsafeBase = Join-Path $ancestorLink "test-base"
        $result = Invoke-PathSafety "New" $unsafeBase
        $physicalBase = Join-Path $ancestorTarget "test-base"
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch "reparse" -or
            (Test-Path -LiteralPath $physicalBase)) {
            $junctionFailures.Add(
                "ancestor junction was not rejected before base creation: $($result.Output)")
        }
    } finally {
        Remove-LocalJunctionFixture $ancestorLink $ancestorTarget $base
    }

    $removeTarget = Join-Path $base "junction-remove-target"
    $removeLink = Join-Path $base "junction-remove"
    $physicalRemoveBase = Join-Path $removeTarget "test-base"
    [IO.Directory]::CreateDirectory($physicalRemoveBase) | Out-Null
    $removeOwnerId = [guid]::NewGuid().ToString("N")
    $removeLeaf = "xen-test-$removeOwnerId"
    $physicalRemoveRoot = Join-Path $physicalRemoveBase $removeLeaf
    [IO.Directory]::CreateDirectory($physicalRemoveRoot) | Out-Null
    New-TestJunction $removeLink $removeTarget
    $lexicalRemoveBase = Join-Path $removeLink "test-base"
    $lexicalRemoveRoot = Join-Path $lexicalRemoveBase $removeLeaf
    $removeMarker = Join-Path $physicalRemoveRoot "keep.txt"
    [IO.File]::WriteAllText($removeMarker, "must-stay")
    [ordered]@{
        schema = 1
        owner_id = $removeOwnerId
        base_path = [IO.Path]::GetFullPath($lexicalRemoveBase)
        root_path = [IO.Path]::GetFullPath($lexicalRemoveRoot)
    } | ConvertTo-Json -Compress | Set-Content -LiteralPath `
        (Join-Path $physicalRemoveRoot ".xen-test-owner.json") -Encoding utf8
    try {
        $result = Invoke-PathSafety "Remove" $lexicalRemoveBase `
            $lexicalRemoveRoot $removeOwnerId
        if ($result.ExitCode -eq 0 -or
            $result.Output -notmatch "reparse" -or
            -not (Test-Path -LiteralPath $removeMarker -PathType Leaf)) {
            $junctionFailures.Add(
                "ancestor junction allowed recursive owner removal: $($result.Output)")
        }
    } finally {
        Remove-LocalJunctionFixture $removeLink $removeTarget $base
    }

    if ($junctionFailures.Count -ne 0) {
        throw "Reparse ancestor contract failed: $($junctionFailures -join ' | ')"
    }

    $result = Invoke-PathSafety "New" $base
    if ($result.ExitCode -ne 0) {
        throw "Failed to create a GUID-owned test directory: $($result.Output)"
    }
    $created = @($result.Output.Trim() -split "`t")
    if ($created.Count -ne 2) {
        throw "Path safety New returned an invalid owner record: $($result.Output)"
    }
    $owned = $created[0]
    $ownerId = $created[1]
    if ((Split-Path -Parent $owned) -ine [IO.Path]::GetFullPath($base) -or
        (Split-Path -Leaf $owned) -notmatch '^xen-test-[0-9a-f]{32}$' -or
        -not (Test-Path -LiteralPath `
            (Join-Path $owned ".xen-test-owner.json") -PathType Leaf)) {
        throw "New test directory violates the direct-child, GUID, or sentinel contract: $owned"
    }
    $result = Invoke-PathSafety "Remove" $base $owned $ownerId
    if ($result.ExitCode -ne 0 -or (Test-Path -LiteralPath $owned)) {
        throw "Valid GUID-owned test directory was not removed exactly: $($result.Output)"
    }

    Write-Host "Test-directory owner path contract passed."
} finally {
    $resolvedBase = [IO.Path]::GetFullPath($base)
    if ((Split-Path -Parent $resolvedBase) -ine $temporaryParent -or
        (Split-Path -Leaf $resolvedBase) -notmatch `
            '^xen-path-safety-base-[0-9a-f]{32}$') {
        throw "Test base cleanup lost its owner constraint: $resolvedBase"
    }
    if ($keep -and (Test-Path -LiteralPath $keep -PathType Leaf)) {
        [IO.File]::Delete($keep)
    }
    if ($nonOwned -and
        (Test-Path -LiteralPath $nonOwned -PathType Container)) {
        [IO.Directory]::Delete($nonOwned, $false)
    }
    if (Test-Path -LiteralPath $resolvedBase -PathType Container) {
        [IO.Directory]::Delete($resolvedBase, $false)
    }
}
