param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('nvidia', 'directml', 'openvino')][string]$Runtime,
    [Parameter(Mandatory = $true)][string]$DestinationRoot,
    [Parameter(Mandatory = $true)][string]$RemotePackageRoot,
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$GitExecutable = 'git',
    [string]$PackageNotesPath = '',
    [string]$ManualAcceptancePath = '',
    [string]$SourceContextExecutable = '',
    [string]$SshIdentityFile = (Join-Path $env:USERPROFILE '.ssh\xen_foxos_ed25519'),
    [string]$KnownHostsFile = (Join-Path $env:USERPROFILE '.ssh\known_hosts'),
    [string]$SshUser = 'XenDeploy',
    [string]$SshHost = '192.168.3.20'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$localRoot = (Resolve-Path -LiteralPath $PackageRoot).ProviderPath.TrimEnd('\')
$remoteRoot = (Resolve-Path -LiteralPath $DestinationRoot).ProviderPath.TrimEnd('\')
$packageName = Split-Path -Leaf $localRoot
if ($SshHost -notmatch '^[a-zA-Z0-9.-]+$' -or $SshUser -notmatch '^[a-zA-Z0-9_-]+$' -or
    $remoteRoot -ine "\\$SshHost\XenLab`$\releases\$packageName" -or
    [IO.Path]::GetFullPath($RemotePackageRoot).TrimEnd('\') -ine "C:\XenLab\releases\$packageName") {
    throw '差量发布的主辅机包名、UNC 与辅机本地映射不一致。'
}
foreach ($path in @($localRoot, $remoteRoot, $SshIdentityFile, $KnownHostsFile)) {
    Assert-XenNoReparsePathChain $path '差量发布输入' -RequireExistingLeaf
}
function Get-PublishHash([string]$Path) {
    Assert-XenNoReparsePathChain $Path '变化文件'
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$baseHash = Get-PublishHash (Join-Path $localRoot 'manifest.json')
if (-not $baseHash -or $baseHash -cne (Get-PublishHash (Join-Path $remoteRoot 'manifest.json'))) {
    throw '主辅机 manifest 基线不一致，拒绝原目录差量。'
}
$stageName = ".worker-delta-$([guid]::NewGuid().ToString('N'))"
$generated = Resolve-XenDirectChildPath (Split-Path -Parent $localRoot) $stageName '生成差量'
$localStage = Resolve-XenDirectChildPath $localRoot $stageName '主机差量暂存'
$remoteStage = Resolve-XenDirectChildPath $remoteRoot $stageName '辅机差量暂存'
$ownedStages = [Collections.Generic.List[object]]::new()
$applyStarted = $false
$completed = $false
function Write-PublishPacket([string]$Stage, [string]$Target, [string[]]$RelativeFiles) {
    $files = @()
    foreach ($relative in $RelativeFiles) {
        $oldRecords = @($baseManifest.files | Where-Object { $_.path -ceq $relative })
        $oldHash = if ($relative -ceq 'manifest.json') { $baseHash }
            elseif ($oldRecords.Count -eq 1) { [string]$oldRecords[0].sha256 } else { '' }
        $files += [ordered]@{ path = $relative
            old_sha256 = $oldHash
            new_sha256 = Get-PublishHash (Join-Path $Stage $relative) }
    }
    $protected = @()
    foreach ($relative in @('config.ini', 'cache/model-workspace/settings.json')) {
        $protected += [ordered]@{ path = $relative; sha256 = Get-PublishHash (Join-Path $Target $relative) }
    }
    [ordered]@{ schema = 1; runtime = $Runtime; files = $files; protected_files = $protected } |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Stage 'delta.json') -Encoding UTF8
}
function Invoke-RemoteApply([bool]$CheckOnly) {
    $helper = Join-Path $RemotePackageRoot "$stageName\apply_worker_delta.ps1"
    $command = "& '$($helper.Replace("'", "''"))' -PackageRoot '$($RemotePackageRoot.Replace("'", "''"))' -StageName '$stageName'"
    if ($CheckOnly) { $command += ' -CheckOnly' }
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=yes `
        -o "UserKnownHostsFile=$KnownHostsFile" "$SshUser@$SshHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded
    if ($LASTEXITCODE -ne 0) { throw "辅机差量检查/替换失败，SSH 退出码 $LASTEXITCODE；未强杀或启动任何进程。" }
}
try {
    & (Join-Path $PSScriptRoot 'publish_worker_update.ps1') -BasePackagePath $localRoot `
        -BuildDirectory $BuildDirectory -Runtime $Runtime -OutputDirectory $generated `
        -RepositoryRoot $RepositoryRoot -GitExecutable $GitExecutable -ChangesOnly `
        -PackageNotesPath $PackageNotesPath -ManualAcceptancePath $ManualAcceptancePath `
        -SourceContextExecutable $SourceContextExecutable
    $ownedStages.Add([pscustomobject]@{ parent = (Split-Path -Parent $localRoot); name = $stageName })
    $null = Resolve-XenDirectChildPath $localRoot $stageName '移入主机包前暂存'
    [IO.Directory]::Move($generated, $localStage)
    $ownedStages.Add([pscustomobject]@{ parent = $localRoot; name = $stageName })
    $relativeFiles = @("runtimes/$Runtime/Xen.exe", 'tools/acceptance/WORKER-UPDATE.json', 'manifest.json')
    if ($PackageNotesPath) { $relativeFiles += 'tools/acceptance/PACKAGE-NOTES.md' }
    if ($ManualAcceptancePath) { $relativeFiles += 'tools/acceptance/MANUAL-ACCEPTANCE.md' }
    if ($SourceContextExecutable) { $relativeFiles += 'tools/source/xen_source_context.exe' }
    $baseManifest = Get-Content -LiteralPath (Join-Path $localRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($relative in $relativeFiles) {
        if ($relative -ceq 'manifest.json') { continue }
        $oldRecords = @($baseManifest.files | Where-Object { $_.path -ceq $relative })
        if ($oldRecords.Count -gt 1) { throw '基包变化文件记录重复。' }
        $expected = if ($oldRecords.Count -eq 1) { [string]$oldRecords[0].sha256 } else { '' }
        foreach ($root in @($localRoot, $remoteRoot)) {
            if ((Get-PublishHash (Join-Path $root $relative)) -cne $expected) {
                throw "变化文件与发布基线不一致：$relative"
            }
        }
    }
    foreach ($helper in @('apply_worker_delta.ps1', 'path_safety.psm1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $helper) -Destination (Join-Path $localStage $helper)
    }
    Write-PublishPacket $localStage $localRoot $relativeFiles
    & (Join-Path $localStage 'apply_worker_delta.ps1') -PackageRoot $localRoot -StageName $stageName -CheckOnly
    New-Item -ItemType Directory -Path $remoteStage | Out-Null
    $ownedStages.Add([pscustomobject]@{ parent = $remoteRoot; name = $stageName })
    $copyFiles = $relativeFiles + @('apply_worker_delta.ps1', 'path_safety.psm1')
    $copied = [long]0
    foreach ($relative in $copyFiles) {
        $source = Join-Path $localStage $relative
        $target = Join-Path $remoteStage $relative
        Assert-XenNoReparsePathChain $target 'SMB 暂存载荷'
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
        if ((Get-PublishHash $source) -cne (Get-PublishHash $target)) { throw 'SMB 差量暂存回读失败。' }
        $copied += (Get-Item -LiteralPath $source).Length
        Write-Progress -Activity 'SMB 传输变化 Worker 与说明' -Status "$copied 字节"
    }
    if ((Get-PublishHash (Join-Path $remoteRoot 'manifest.json')) -cne $baseHash) { throw '辅机 manifest 在准备期间变化。' }
    Write-PublishPacket $remoteStage $remoteRoot $relativeFiles
    Invoke-RemoteApply $true
    # 先服务器本地替换；确认完成后才同步本机固定包。
    $applyStarted = $true
    Invoke-RemoteApply $false
    & (Join-Path $localStage 'apply_worker_delta.ps1') -PackageRoot $localRoot -StageName $stageName
    foreach ($relative in $relativeFiles) {
        if ((Get-PublishHash (Join-Path $localRoot $relative)) -cne (Get-PublishHash (Join-Path $remoteRoot $relative))) {
            throw "主辅机变化文件最终 SHA 不一致：$relative"
        }
    }
    $completed = $true
    Write-Host '主辅机 Worker 差量更新完成；未复制未变化 Worker、模型、DLL，未覆盖用户配置。'
} finally {
    Write-Progress -Activity 'SMB 传输变化 Worker 与说明' -Completed
    foreach ($owned in $ownedStages) {
        if ($applyStarted -and -not $completed) {
            Write-Warning '差量应用未完整确认，保留本轮暂存和备份供核对，不删除恢复证据。'
            break
        }
        $path = Resolve-XenDirectChildPath $owned.parent $owned.name '本轮差量暂存清理'
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue($path)
        while ($pending.Count -gt 0) {
            foreach ($entry in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
                if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw '暂存出现链接，拒绝递归清理。' }
                if ($entry.PSIsContainer) { $pending.Enqueue($entry.FullName) }
            }
        }
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
