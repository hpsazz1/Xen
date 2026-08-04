param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$",
    [string]$DestinationDirectory = "releases"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-RequiredDirectory(
        [string]$Path,
        [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Read-ReleaseManifest([string]$Root) {
    $path = Join-Path $Root "manifest.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "完整发布包缺少 manifest.json：$Root"
    }
    try {
        $manifest = Get-Content -LiteralPath $path -Raw -Encoding utf8 |
            ConvertFrom-Json
    } catch {
        throw "完整发布包 manifest.json 无法解析：$($_.Exception.Message)"
    }
    if ([int]$manifest.schema -ne 1 -or
        [string]$manifest.product -ne "Xen" -or
        [string]$manifest.git_commit -notmatch '^[0-9a-fA-F]{40}$' -or
        @($manifest.runtimes).Count -ne 3 -or
        @($manifest.files).Count -eq 0) {
        throw "完整发布包清单版本、提交或运行时数量无效。"
    }
    return [pscustomobject]@{
        Path = $path
        Value = $manifest
    }
}

function Assert-PackageFiles(
        [string]$Root,
        [object]$Manifest) {
    $declared = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $relative = ([string]$record.path).Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or
            [System.IO.Path]::IsPathRooted($relative) -or
            $relative -match '(^|\\)\.\.(\\|$)' -or
            -not $declared.Add($relative)) {
            throw "完整发布包包含非法或重复路径：$relative"
        }
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "完整发布包缺少清单文件：$relative"
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ([long]$file.Length -ne [long]$record.size -or
            [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
            $hash -ne ([string]$record.sha256).ToUpperInvariant()) {
            throw "完整发布包文件长度或 SHA-256 不一致：$relative"
        }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -Recurse -File)) {
        $relative = $file.FullName.Substring(
            $Root.TrimEnd('\').Length + 1)
        if ($relative -ieq "manifest.json") { continue }
        if (-not $declared.Contains($relative)) {
            throw "完整发布包包含清单外文件：$relative"
        }
    }
}

function Copy-PackageWithProgress(
        [string]$SourceRoot,
        [string]$TargetRoot,
        [object]$Manifest,
        [string]$ManifestPath) {
    New-Item -ItemType Directory -Path $TargetRoot | Out-Null
    $entries = @($Manifest.files | ForEach-Object {
        [pscustomobject]@{
            path = [string]$_.path
            size = [long]$_.size
        }
    })
    $manifestFile = Get-Item -LiteralPath $ManifestPath
    $entries += [pscustomobject]@{
        path = "manifest.json"
        size = [long]$manifestFile.Length
    }
    $totalBytes = [long](($entries | Measure-Object -Property size -Sum).Sum)
    $copiedBytes = [long]0
    $buffer = [byte[]]::new(4MB)
    try {
        foreach ($entry in $entries) {
            $relative = ([string]$entry.path).Replace('/', '\')
            $sourcePath = Join-Path $SourceRoot $relative
            $targetPath = Join-Path $TargetRoot $relative
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
                    [System.IO.FileAccess]::Write, [System.IO.FileShare]::None,
                    4MB, [System.IO.FileOptions]::SequentialScan)
                try {
                    while (($read = $source.Read(
                                $buffer, 0, $buffer.Length)) -gt 0) {
                        $target.Write($buffer, 0, $read)
                        $copiedBytes += $read
                        $percent = if ($totalBytes -eq 0) { 100 } else {
                            [Math]::Min(100, [int](
                                $copiedBytes * 100 / $totalBytes))
                        }
                        Write-Progress -Id 1 -Activity "传输 Xen 完整发布包" `
                            -Status ("{0}  {1:N1}/{2:N1} MiB" -f
                                $relative, ($copiedBytes / 1MB),
                                ($totalBytes / 1MB)) `
                            -PercentComplete $percent
                    }
                    $target.Flush()
                } finally {
                    $target.Dispose()
                }
            } finally {
                $source.Dispose()
            }
        }
    } finally {
        Write-Progress -Id 1 -Activity "传输 Xen 完整发布包" -Completed
    }
}

$package = Resolve-RequiredDirectory $PackagePath "本地完整发布包"
$packageName = Split-Path -Leaf $package
if ($packageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$') {
    throw "完整发布包目录名非法：$packageName"
}
$manifestResult = Read-ReleaseManifest $package
$manifest = $manifestResult.Value
Write-Host "[1/4] 校验本地完整发布包文件与 SHA-256..."
Assert-PackageFiles $package $manifest

$destination = Resolve-RequiredDirectory $DestinationRoot "辅机 XenLab 共享"
$releaseRoot = Join-Path $destination $DestinationDirectory
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
$releaseRoot = Resolve-RequiredDirectory $releaseRoot "辅机发布目录"
$published = Join-Path $releaseRoot $packageName
$incoming = Join-Path $releaseRoot ".incoming-$packageName"
if ((Test-Path -LiteralPath $published) -or
    (Test-Path -LiteralPath $incoming)) {
    throw "辅机正式目录或临时目录已存在，拒绝覆盖：$packageName"
}

try {
    Write-Host "[2/4] 复制到辅机临时目录：$incoming"
    Copy-PackageWithProgress $package $incoming $manifest `
        $manifestResult.Path
    Write-Host "[3/4] 回读辅机文件并复核 SHA-256..."
    Assert-PackageFiles $incoming $manifest
    $localManifestHash = (Get-FileHash -LiteralPath $manifestResult.Path `
        -Algorithm SHA256).Hash
    $remoteManifestHash = (Get-FileHash `
        -LiteralPath (Join-Path $incoming "manifest.json") `
        -Algorithm SHA256).Hash
    if ($localManifestHash -ne $remoteManifestHash) {
        throw "辅机 manifest.json 回读哈希不一致。"
    }
    Write-Host "[4/4] 全部校验通过，原子发布正式目录..."
    Rename-Item -LiteralPath $incoming -NewName $packageName
} catch {
    if (Test-Path -LiteralPath $incoming) {
        Remove-Item -LiteralPath $incoming -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
    throw
}

Assert-PackageFiles $published $manifest
Write-Host "辅机完整发布包传输完成：$published"
Write-Host "  git_commit=$($manifest.git_commit)"
Write-Host "  manifest_sha256=$localManifestHash"
