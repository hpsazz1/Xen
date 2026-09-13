param(
    [Parameter(Mandatory = $true)][string]$BasePackagePath,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('nvidia', 'directml', 'openvino')][string]$Runtime,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$GitExecutable = 'git',
    [string]$ConfigPath = '',
    [string]$WorkspaceSettingsPath = '',
    [string]$PackageNotesPath = '',
    [string]$ManualAcceptancePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force

function Read-UpdateJson([string]$Path) {
    Assert-XenNoReparsePathChain $Path 'JSON 输入' -RequireExistingLeaf
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Resolve-UpdateFile([string]$Path) {
    Assert-XenNoReparsePathChain $Path '发布输入' -RequireExistingLeaf
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or $item.Length -le 0) { throw '发布输入必须是非空普通文件。' }
    return $item.FullName
}

function Resolve-UpdatePayload([string]$Root, [string]$Relative) {
    # 拒绝 Windows 别名、ADS、设备名和路径归一化歧义；不枚举可变数据目录。
    if ([string]::IsNullOrWhiteSpace($Relative) -or
        [IO.Path]::IsPathRooted($Relative) -or $Relative -match '[<>:"|?*\x00-\x1f]') {
        throw '清单包含非法相对路径。'
    }
    foreach ($part in ($Relative -split '[/\\]')) {
        if ([string]::IsNullOrWhiteSpace($part) -or $part -in @('.', '..') -or
            $part -match '[. ]$' -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') {
            throw '清单包含非法路径段。'
        }
    }
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative.Replace('/', '\')))
    if (-not $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw '清单路径越出包根。'
    }
    Assert-XenNoReparsePathChain $path '清单载荷'
    return $path
}

$baseRoot = (Resolve-Path -LiteralPath $BasePackagePath).ProviderPath.TrimEnd('\')
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath.TrimEnd('\')
$sourceRoot = (Resolve-Path -LiteralPath $RepositoryRoot).ProviderPath.TrimEnd('\')
foreach ($root in @($baseRoot, $buildRoot, $sourceRoot)) {
    Assert-XenNoReparsePathChain $root '输入目录' -RequireExistingLeaf
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw '输入根必须为目录。' }
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
$outputParent = Split-Path -Parent $outputPath
$outputName = Split-Path -Leaf $outputPath
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw '发布父目录必须已存在。'
}
$outputPath = Resolve-XenDirectChildPath $outputParent $outputName '新发布目录'
if ($outputPath -ieq $baseRoot -or
    $outputPath.StartsWith($baseRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $outputPath)) { throw '新发布目录必须不存在且不得位于基包中。' }

$commit = (& $GitExecutable -C $sourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') { throw '无法核对源码提交。' }
$dirty = @(& $GitExecutable -C $sourceRoot status --porcelain)
if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) { throw '发布构建源码必须干净。' }
$identityPath = Join-Path $buildRoot 'xen-build-identity.json'
$identity = Read-UpdateJson $identityPath
$identityHash = (Get-FileHash -LiteralPath $identityPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($identity.schema -ne 1 -or $identity.git_dirty -isnot [bool] -or
    $identity.git_dirty -or $identity.git_commit -cne $commit -or
    $identity.runtime -cne $Runtime -or
    [IO.Path]::GetFullPath([string]$identity.source_root).TrimEnd('\') -ine $sourceRoot) {
    throw 'Worker 构建身份与干净源码、提交或运行时不一致。'
}
$workerPath = Resolve-UpdateFile (Join-Path $buildRoot 'Release\Xen.exe')
$workerHash = (Get-FileHash -LiteralPath $workerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$workerLength = (Get-Item -LiteralPath $workerPath).Length
$manifestPath = Resolve-UpdateFile (Join-Path $baseRoot 'manifest.json')
$baseManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = Read-UpdateJson $manifestPath
if ($manifest.schema -ne 1 -or $manifest.product -cne 'Xen' -or
    $manifest.git_commit -notmatch '^[0-9a-fA-F]{40}$' -or
    @($manifest.runtimes).Count -ne 3 -or @($manifest.files).Count -eq 0) {
    throw '基包不是有效的完整统一包。'
}
$runtimeIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($route in $manifest.runtimes) {
    if ($route.id -notin @('nvidia', 'directml', 'openvino') -or
        -not $runtimeIds.Add([string]$route.id) -or
        $route.executable -cne "runtimes/$($route.id)/Xen.exe") { throw '基包运行时路由无效。' }
}
$records = @{}
foreach ($record in $manifest.files) {
    $relative = ([string]$record.path).Replace('\', '/')
    if ($relative -ieq 'manifest.json' -or $records.ContainsKey($relative) -or
        $record.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or [long]$record.size -lt 0) {
        throw '基包清单包含重复或无效文件记录。'
    }
    $path = Resolve-UpdatePayload $baseRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -ne [long]$record.size) {
        throw "基包显式载荷缺失或长度变化：$relative"
    }
    $records[$relative] = $record
}
foreach ($route in $manifest.runtimes) {
    if (-not $records.ContainsKey([string]$route.executable) -or
        $records[[string]$route.executable].runtime -cne $route.id) { throw '基包 Worker 归属无效。' }
}
$workerRelative = "runtimes/$Runtime/Xen.exe"
$overrides = @{ $workerRelative = $workerPath }
if ($ConfigPath) { $overrides['config.ini'] = Resolve-UpdateFile $ConfigPath }
if ($WorkspaceSettingsPath) {
    $overrides['cache/model-workspace/settings.json'] = Resolve-UpdateFile $WorkspaceSettingsPath
    $null = Read-UpdateJson $overrides['cache/model-workspace/settings.json']
}
if ($PackageNotesPath) {
    $overrides['tools/acceptance/PACKAGE-NOTES.md'] = Resolve-UpdateFile $PackageNotesPath
}
if ($ManualAcceptancePath) {
    $overrides['tools/acceptance/MANUAL-ACCEPTANCE.md'] = Resolve-UpdateFile $ManualAcceptancePath
}
foreach ($relative in $overrides.Keys) {
    if (-not $records.ContainsKey($relative)) { throw "替换文件不在基包清单内：$relative" }
}

$incomingName = ".incoming-$outputName-$([guid]::NewGuid().ToString('N'))"
$incoming = Resolve-XenDirectChildPath $outputParent $incomingName '本轮暂存目录'
$ownedIncoming = $false
try {
    New-Item -ItemType Directory -Path $incoming -ErrorAction Stop | Out-Null
    $ownedIncoming = $true
    $totalBytes = [long](($manifest.files | Measure-Object -Property size -Sum).Sum)
    $copiedBytes = [long]0
    foreach ($record in $manifest.files) {
        $relative = ([string]$record.path).Replace('\', '/')
        $source = Resolve-UpdatePayload $baseRoot $relative
        if ($overrides.ContainsKey($relative)) { $source = $overrides[$relative] }
        $destination = Resolve-UpdatePayload $incoming $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -ErrorAction Stop
        $length = (Get-Item -LiteralPath $destination).Length
        if ($overrides.ContainsKey($relative)) {
            $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash) {
                throw "替换载荷回读不一致：$relative"
            }
            if ($relative -eq $workerRelative -and
                ($sourceHash -cne $workerHash -or $length -ne $workerLength)) { throw 'Worker 在发布期间变化。' }
            $record.size = [long]$length
            $record.sha256 = $sourceHash
            $record.source = if ($relative -eq $workerRelative) { "$source@$commit" } else { $source }
        } elseif ($length -ne [long]$record.size) { throw "继承载荷复制长度错误：$relative" }
        $copiedBytes += $length
        Write-Progress -Activity '继承统一包显式载荷' -Status "$copiedBytes / $totalBytes 字节" `
            -PercentComplete ([Math]::Min(100, [int](100.0 * $copiedBytes / [Math]::Max(1.0, [double]$totalBytes))))
    }
    if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $baseManifestHash) {
        throw '基包 manifest 在继承期间变化。'
    }
    $finalCommit = (& $GitExecutable -C $sourceRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $finalCommit -cne $commit) { throw '源码提交在发布期间变化。' }
    $finalDirty = @(& $GitExecutable -C $sourceRoot status --porcelain)
    if ($LASTEXITCODE -ne 0 -or $finalDirty.Count -ne 0 -or
        (Get-FileHash -LiteralPath $identityPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $identityHash) {
        throw '源码或构建身份在发布期间变化。'
    }
    $baseIdentity = [ordered]@{
        path = $baseRoot; git_commit = [string]$manifest.git_commit; manifest_sha256 = $baseManifestHash
    }
    $manifest.git_commit = $commit.ToLowerInvariant()
    $manifest | Add-Member -Force NoteProperty worker_update ([ordered]@{
        schema = 1; base_package = $baseIdentity
        inherited_files_identity = 'base_package.manifest_sha256'
        updated_components = @([ordered]@{
            runtime = $Runtime; path = $workerRelative; git_commit = $commit.ToLowerInvariant()
            sha256 = $workerHash; build_identity_sha256 = $identityHash
        })
        overridden_files = @($overrides.Keys | Sort-Object)
        inherited_payload_hashes_verified = $false
        next_validation = 'transfer_release_bundle.ps1 完整跨机清单校验'
    })
    $manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $incoming 'manifest.json') -Encoding UTF8
    $null = Read-UpdateJson (Join-Path $incoming 'manifest.json')
    $null = Resolve-XenDirectChildPath $outputParent $incomingName '改名前暂存目录'
    $null = Resolve-XenDirectChildPath $outputParent $outputName '改名前正式目录'
    if (Test-Path -LiteralPath $outputPath) { throw '正式目录在发布期间出现，拒绝覆盖。' }
    Rename-Item -LiteralPath $incoming -NewName $outputName
    $ownedIncoming = $false
    Write-Host "单 Worker 继承包已准备：$outputPath"
    Write-Host '未验证继承载荷全量 SHA；正式传输前必须由 transfer_release_bundle.ps1 完整校验。'
} finally {
    Write-Progress -Activity '继承统一包显式载荷' -Completed
    if ($ownedIncoming -and (Test-Path -LiteralPath $incoming)) {
        $verified = Resolve-XenDirectChildPath $outputParent $incomingName '失败暂存清理'
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue($verified)
        while ($pending.Count -gt 0) {
            foreach ($entry in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
                if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                    throw '失败暂存出现链接，拒绝递归清理。'
                }
                if ($entry.PSIsContainer) { $pending.Enqueue($entry.FullName) }
            }
        }
        Remove-Item -LiteralPath $verified -Recurse -Force
    }
}
