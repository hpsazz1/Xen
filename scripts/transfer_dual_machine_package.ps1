param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$"
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Assert-PackageFiles {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object]$Manifest
    )
    $reportedPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $relativePath = ([string]$record.relative_path).Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relativePath) -or
            [System.IO.Path]::IsPathRooted($relativePath) -or
            $relativePath -match '(^|/)\.\.(/|$)' -or
            -not $reportedPaths.Add($relativePath)) {
            throw "包清单包含非法或重复相对路径：$relativePath"
        }
        $path = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "包缺少清单文件：$path"
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ([long]$file.Length -ne [long]$record.length -or
            [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
            $hash -ne [string]$record.sha256) {
            throw "包文件长度或 SHA-256 不一致：$path"
        }
    }
    if ($reportedPaths.Count -eq 0) {
        throw "包清单没有文件记录。"
    }
}

function Copy-PackageWithProgress {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$TargetRoot,
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    New-Item -ItemType Directory -Path $TargetRoot -ErrorAction Stop |
        Out-Null
    $entries = @($Manifest.files)
    $manifestFile = Get-Item -LiteralPath $ManifestPath
    $entries += [pscustomobject]@{
        relative_path = "package-manifest.json"
        length = [long]$manifestFile.Length
    }
    $totalBytes = [long](@($entries | Measure-Object -Property length -Sum).
        Sum)
    $copiedBytes = [long]0
    $buffer = [byte[]]::new(4MB)
    try {
        foreach ($entry in $entries) {
            $relativePath = [string]$entry.relative_path
            $sourcePath = Join-Path $SourceRoot $relativePath
            $targetPath = Join-Path $TargetRoot $relativePath
            $targetDirectory = Split-Path -Parent $targetPath
            New-Item -ItemType Directory -Path $targetDirectory -Force |
                Out-Null

            $source = [System.IO.FileStream]::new(
                $sourcePath, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read,
                4MB, [System.IO.FileOptions]::SequentialScan)
            try {
                $target = [System.IO.FileStream]::new(
                    $targetPath, [System.IO.FileMode]::CreateNew,
                    [System.IO.FileAccess]::Write,
                    [System.IO.FileShare]::None, 4MB,
                    [System.IO.FileOptions]::SequentialScan)
                try {
                    while (($read = $source.Read(
                                $buffer, 0, $buffer.Length)) -gt 0) {
                        $target.Write($buffer, 0, $read)
                        $copiedBytes += $read
                        $percent = if ($totalBytes -eq 0) {
                            100
                        } else {
                            [Math]::Min(100, [int](
                                $copiedBytes * 100 / $totalBytes))
                        }
                        $status = "{0}  {1:N1}/{2:N1} MiB" -f `
                            $relativePath, ($copiedBytes / 1MB),
                            ($totalBytes / 1MB)
                        Write-Progress -Id 1 `
                            -Activity "传输 Xen 双机测试包" `
                            -Status $status -PercentComplete $percent
                    }
                    $target.Flush()
                } finally {
                    $target.Dispose()
                }
            } finally {
                $source.Dispose()
            }
            [System.IO.File]::SetLastWriteTimeUtc(
                $targetPath,
                (Get-Item -LiteralPath $sourcePath).LastWriteTimeUtc)
        }
    } finally {
        Write-Progress -Id 1 -Activity "传输 Xen 双机测试包" `
            -Completed
    }
}

$PackagePath = Resolve-RequiredDirectory $PackagePath "本地双机包"
$manifestPath = Join-Path $PackagePath "package-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "本地双机包缺少 package-manifest.json：$PackagePath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$packageId = [string]$manifest.package_id
if ([int]$manifest.schema -ne 1 -or -not [bool]$manifest.complete -or
    [string]$manifest.package_type -ne "xen-dual-machine-receiver" -or
    $packageId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') {
    throw "本地双机包清单无效：$manifestPath"
}
if ([System.IO.Path]::GetFileName($PackagePath) -ne $packageId) {
    throw "包目录名与清单 PackageId 不一致。"
}
Write-Host "[1/4] 校验本地包文件与 SHA-256..."
Assert-PackageFiles $PackagePath $manifest

$DestinationRoot = Resolve-RequiredDirectory `
    $DestinationRoot "辅机 XenLab SMB 共享"
$packagesRoot = Resolve-RequiredDirectory `
    (Join-Path $DestinationRoot "packages") "辅机 packages 目录"
$publishedPath = Join-Path $packagesRoot $packageId
$incomingPath = Join-Path $packagesRoot ".incoming-$packageId"
if ((Test-Path -LiteralPath $publishedPath) -or
    (Test-Path -LiteralPath $incomingPath)) {
    throw "辅机目标包或临时目录已存在，拒绝覆盖：$packageId"
}

try {
    Write-Host "[2/4] 复制到辅机临时目录：$incomingPath"
    Copy-PackageWithProgress `
        $PackagePath $incomingPath $manifest $manifestPath
    Write-Host "[3/4] 回读辅机文件并复核 SHA-256..."
    Assert-PackageFiles $incomingPath $manifest
    $remoteManifestPath = Join-Path $incomingPath "package-manifest.json"
    $remoteManifestHash = (Get-FileHash -LiteralPath $remoteManifestPath `
        -Algorithm SHA256).Hash
    $localManifestHash = (Get-FileHash -LiteralPath $manifestPath `
        -Algorithm SHA256).Hash
    if ($remoteManifestHash -ne $localManifestHash) {
        throw "辅机包清单 SHA-256 与本地不一致。"
    }
    Write-Host "[4/4] 校验通过，原子发布正式目录..."
    Rename-Item -LiteralPath $incomingPath -NewName $packageId
} catch {
    if (Test-Path -LiteralPath $incomingPath) {
        Remove-Item -LiteralPath $incomingPath -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
    throw
}
Assert-PackageFiles $publishedPath $manifest

Write-Host "辅机双机包传输完成：$publishedPath"
Write-Host "  package_id=$packageId"
Write-Host "  manifest_sha256=$localManifestHash"
