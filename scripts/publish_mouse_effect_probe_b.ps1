param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$SourceScriptRoot,
    [string]$GitExecutable = "git",
    [Parameter(Mandatory = $true)]
    [string]$PackageOutputRoot,
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$",
    [switch]$SkipRemotePublish
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

function Resolve-RequiredFile(
        [string]$Path,
        [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不存在：$Path"
    }
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -le 0) {
        throw "$Description 为空：$Path"
    }
    return $file.FullName
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.
        ToLowerInvariant()
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    if (Test-Path -LiteralPath $Path) {
        throw "JSON 发布目标已存在：$Path"
    }
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 16) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Get-RepositoryRelativePath(
        [string]$Path,
        [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "发布源码不在仓库内：$fullPath"
    }
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Invoke-GitChecked(
        [string]$Repository,
        [string[]]$Arguments,
        [string]$Description) {
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $GitExecutable -C $Repository @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($exitCode -ne 0) {
        throw "$Description 失败：$($output -join ' ')"
    }
    return @($output)
}

function Assert-TrackedClean([string]$Repository) {
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $GitExecutable -C $Repository diff --quiet --no-ext-diff
        $worktreeExitCode = $LASTEXITCODE
        & $GitExecutable -C $Repository diff --cached --quiet --no-ext-diff
        $indexExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($worktreeExitCode -ne 0) {
        throw "Physical B 正式发布要求工作树没有可跟踪差异"
    }
    if ($indexExitCode -ne 0) {
        throw "Physical B 正式发布要求暂存区没有可跟踪差异"
    }
}

function Assert-OwnedIncomingPath(
        [string]$Path,
        [string]$Parent,
        [string]$Prefix) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    $expectedParentPrefix =
        $fullParent + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $expectedParentPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $fullPath).StartsWith(
            $Prefix, [StringComparison]::Ordinal)) {
        throw "incoming 清理路径越界：$fullPath"
    }
    return $fullPath
}

function Copy-Payload(
        [string]$Source,
        [string]$Name,
        [string]$DestinationRoot,
        [string]$Kind,
        [string]$Commit,
        [Collections.Generic.List[object]]$Files) {
    if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$') {
        throw "Physical B payload 文件名非法：$Name"
    }
    $destination = Join-Path $DestinationRoot $Name
    if (Test-Path -LiteralPath $destination) {
        throw "Physical B payload 目标已存在：$destination"
    }
    [IO.File]::Copy($Source, $destination, $false)
    $sourceHash = Get-FileSha256 $Source
    $destinationHash = Get-FileSha256 $destination
    $file = Get-Item -LiteralPath $destination
    if ($sourceHash -ne $destinationHash -or $file.Length -le 0) {
        throw "Physical B payload 复制后哈希或长度无效：$Name"
    }
    $Files.Add([ordered]@{
        name = $Name
        size = [uint64]$file.Length
        sha256 = $destinationHash
        provenance = [ordered]@{
            kind = $Kind
            git_commit = $Commit
            source = [IO.Path]::GetFullPath($Source)
        }
    })
}

function Assert-Package(
        [string]$Root,
        [object]$Manifest,
        [string]$ExpectedPackageName) {
    if ([int]$Manifest.schema_version -ne 1 -or
        [string]$Manifest.evidence_type -ne
            "mouse_effect_probe_b_tool_package" -or
        [string]$Manifest.package_name -ne $ExpectedPackageName -or
        -not [bool]$Manifest.source_tracked_clean -or
        [bool]$Manifest.physical_run_included -or
        [bool]$Manifest.physical_launch_executed -or
        [int]$Manifest.file_count -ne @($Manifest.files).Count -or
        @($Manifest.files).Count -ne 11) {
        throw "Physical B 工具包 manifest 身份或安全边界无效"
    }
    $declared = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $name = [string]$record.name
        if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or
            -not $declared.Add($name)) {
            throw "Physical B manifest 文件名非法或重复：$name"
        }
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Physical B 工具包缺少 payload：$name"
        }
        $file = Get-Item -LiteralPath $path
        if ([uint64]$file.Length -ne [uint64]$record.size -or
            [string]$record.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            (Get-FileSha256 $path) -ne [string]$record.sha256) {
            throw "Physical B payload 长度或 SHA-256 不一致：$name"
        }
    }
    $actualFiles = @(
        Get-ChildItem -LiteralPath $Root -File | ForEach-Object { $_.Name })
    $actualDirectories = @(Get-ChildItem -LiteralPath $Root -Directory)
    if ($actualDirectories.Count -ne 0 -or
        $actualFiles.Count -ne $declared.Count + 1 -or
        $actualFiles -notcontains "manifest.json") {
        throw "Physical B 工具包包含清单外文件或目录"
    }
    foreach ($name in $actualFiles) {
        if ($name -ne "manifest.json" -and -not $declared.Contains($name)) {
            throw "Physical B 工具包包含未声明文件：$name"
        }
    }
}

$repository = Resolve-RequiredDirectory $RepositoryRoot "仓库根目录"
$sourceRoot = Resolve-RequiredDirectory $SourceScriptRoot "Physical B 脚本目录"
$buildRoot = Resolve-RequiredDirectory $BuildDirectory "NVIDIA 构建目录"
$releaseRoot = Resolve-RequiredDirectory `
    (Join-Path $buildRoot "Release") "NVIDIA Release 目录"
Assert-TrackedClean $repository
$commitOutput = @(Invoke-GitChecked $repository @("rev-parse", "HEAD") `
    "读取 Git commit")
$commit = ([string]$commitOutput[0]).Trim().ToLowerInvariant()
if ($commit -notmatch '^[0-9a-f]{40}$') {
    throw "Physical B Git commit 非法"
}

$scriptNames = @(
    "prepare_mouse_effect_probe_b.ps1",
    "launch_mouse_effect_probe_a.ps1",
    "design_mouse_effect_probe_prbs.py",
    "analyze_mouse_effect_probe_b.py")
$sourceFiles = @()
foreach ($name in $scriptNames) {
    $path = Resolve-RequiredFile (Join-Path $sourceRoot $name) `
        "Physical B source script"
    $relative = Get-RepositoryRelativePath $path $repository
    [void](Invoke-GitChecked $repository `
        @("ls-files", "--error-unmatch", "--", $relative) `
        "确认发布脚本受 Git 跟踪")
    $sourceFiles += [pscustomobject]@{ name = $name; path = $path }
}

$identityPath = Resolve-RequiredFile `
    (Join-Path $buildRoot "xen-build-identity.json") "构建身份"
$identity = Get-Content -LiteralPath $identityPath -Raw -Encoding utf8 |
    ConvertFrom-Json
if ([int]$identity.schema -ne 1 -or
    [string]$identity.git_commit -ne $commit -or
    [string]$identity.runtime -ne "nvidia" -or
    @($identity.components).Count -eq 0) {
    throw "Physical B 构建身份与当前 NVIDIA commit 不一致"
}

$buildNames = @(
    "XenMouseEffectProbe.exe",
    "XenCaptureEvidence.exe",
    "XenMouseEffectProbeSequence.exe",
    "opencv_world4140.dll",
    "Processing.NDI.Lib.x64.dll",
    "Processing.NDI.Lib.Licenses.txt")
$buildFiles = @()
foreach ($name in $buildNames) {
    $buildFiles += [pscustomobject]@{
        name = $name
        path = Resolve-RequiredFile (Join-Path $releaseRoot $name) `
            "Physical B Release payload"
    }
}
$buildFiles += [pscustomobject]@{
    name = "xen-build-identity.json"
    path = $identityPath
}

$packageName = "MouseEffectProbe-B-$($commit.Substring(0, 7))"
$packageParent = [IO.Path]::GetFullPath($PackageOutputRoot)
if (-not (Test-Path -LiteralPath $packageParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $packageParent -Force)
}
$packageParent = Resolve-RequiredDirectory $packageParent "本地工具包目录"
$localFinal = Join-Path $packageParent $packageName
$localPrefix = ".incoming-$packageName-"
$existingLocalIncoming = @(
    Get-ChildItem -LiteralPath $packageParent -Directory |
        Where-Object { $_.Name.StartsWith(
            $localPrefix, [StringComparison]::Ordinal) })
if ((Test-Path -LiteralPath $localFinal) -or
    $existingLocalIncoming.Count -ne 0) {
    throw "Physical B 本地正式包或 incoming 已存在：$packageName"
}
$localIncoming = Join-Path $packageParent (
    "$localPrefix$([guid]::NewGuid().ToString('N'))")
[void](New-Item -ItemType Directory -Path $localIncoming)

$manifest = $null
$manifestHash = ""
try {
    $files = [Collections.Generic.List[object]]::new()
    foreach ($file in $buildFiles) {
        $kind = if ($file.name -eq "xen-build-identity.json") {
            "build_identity"
        } else {
            "release_build_artifact"
        }
        Copy-Payload $file.path $file.name $localIncoming $kind $commit $files
    }
    foreach ($file in $sourceFiles) {
        Copy-Payload $file.path $file.name $localIncoming `
            "source_script" $commit $files
    }
    $manifest = [ordered]@{
        schema_version = 1
        evidence_type = "mouse_effect_probe_b_tool_package"
        package_name = $packageName
        created_at_utc = [DateTime]::UtcNow.ToString("o")
        git_commit = $commit
        source_tracked_clean = $true
        source_untracked_files_excluded = $true
        build_identity_git_dirty = [bool]$identity.git_dirty
        runtime = "nvidia"
        file_count = $files.Count
        physical_run_included = $false
        physical_launch_executed = $false
        launch_requires_user_frontend_action = $true
        cross_run_holdout_included = $false
        files = @($files)
    }
    $localManifest = Join-Path $localIncoming "manifest.json"
    Write-NewUtf8Json $localManifest $manifest
    Assert-Package $localIncoming $manifest $packageName
    Rename-Item -LiteralPath $localIncoming -NewName $packageName
    Assert-Package $localFinal $manifest $packageName
    $manifestHash = Get-FileSha256 (Join-Path $localFinal "manifest.json")
} catch {
    $failure = $_
    if (Test-Path -LiteralPath $localIncoming -PathType Container) {
        $owned = Assert-OwnedIncomingPath `
            $localIncoming $packageParent $localPrefix
        Remove-Item -LiteralPath $owned -Recurse -Force
    }
    throw $failure
}

$published = ""
if (-not $SkipRemotePublish.IsPresent) {
    $destination = Resolve-RequiredDirectory `
        $DestinationRoot "辅机 XenLab 共享"
    $remoteReleaseRoot = Join-Path $destination "releases"
    if (-not (Test-Path -LiteralPath $remoteReleaseRoot -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $remoteReleaseRoot)
    }
    $remoteReleaseRoot = Resolve-RequiredDirectory `
        $remoteReleaseRoot "辅机 releases 目录"
    $published = Join-Path $remoteReleaseRoot $packageName
    $remoteIncomingName = ".incoming-$packageName"
    $remoteIncoming = Join-Path $remoteReleaseRoot $remoteIncomingName
    if ((Test-Path -LiteralPath $published) -or
        (Test-Path -LiteralPath $remoteIncoming)) {
        throw "Physical B 辅机正式包或 incoming 已存在：$packageName"
    }
    try {
        [void](New-Item -ItemType Directory -Path $remoteIncoming)
        foreach ($file in @(Get-ChildItem -LiteralPath $localFinal -File)) {
            [IO.File]::Copy(
                $file.FullName,
                (Join-Path $remoteIncoming $file.Name),
                $false)
        }
        Assert-Package $remoteIncoming $manifest $packageName
        $remoteManifestHash = Get-FileSha256 `
            (Join-Path $remoteIncoming "manifest.json")
        if ($remoteManifestHash -ne $manifestHash) {
            throw "Physical B 辅机 manifest SHA-256 回读不一致"
        }
        Rename-Item -LiteralPath $remoteIncoming -NewName $packageName
    } catch {
        $failure = $_
        if (Test-Path -LiteralPath $remoteIncoming -PathType Container) {
            $owned = Assert-OwnedIncomingPath `
                $remoteIncoming $remoteReleaseRoot $remoteIncomingName
            Remove-Item -LiteralPath $owned -Recurse -Force
        }
        throw $failure
    }
    Assert-Package $published $manifest $packageName
}

Write-Host "Physical B 工具包已原子发布：$localFinal"
Write-Host "package=$packageName"
Write-Host "manifest_sha256=$manifestHash"
Write-Host "payload_files=$($manifest.file_count)"
if ($SkipRemotePublish.IsPresent) {
    Write-Host "remote_publish=skipped"
} else {
    Write-Host "remote_publish=$published"
}
Write-Host "physical_launch_executed=false"
