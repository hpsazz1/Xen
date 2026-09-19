param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$StageName,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$root = (Resolve-Path -LiteralPath $PackageRoot).ProviderPath.TrimEnd('\')
$stage = Resolve-XenDirectChildPath $root $StageName '差量暂存'
if ($StageName -notmatch '^\.worker-delta-[0-9a-f]{32}$') { throw '差量暂存名称无效。' }
$packet = Get-Content -LiteralPath (Join-Path $stage 'delta.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($packet.schema -ne 1 -or $packet.runtime -notin @('nvidia', 'directml', 'openvino')) { throw '差量协议无效。' }
$workerRelative = "runtimes/$($packet.runtime)/Xen.exe"
$recoilTools = @("runtimes/$($packet.runtime)/xen_recoil_calibration.exe", "runtimes/$($packet.runtime)/xen_recoil_tuner.exe")
$allowed = @($workerRelative, 'tools/acceptance/WORKER-UPDATE.json',
    'tools/acceptance/PACKAGE-NOTES.md', 'tools/acceptance/MANUAL-ACCEPTANCE.md',
    'tools/source/xen_source_context.exe', 'tools/source/start_source_context_session.ps1', 'XenLauncher.exe', 'manifest.json') + $recoilTools
function Resolve-DeltaFile([string]$Base, [string]$Relative) {
    if ($Relative -cnotin $allowed -and $Relative -cnotin @('config.ini', 'cache/model-workspace/settings.json')) {
        throw '差量文件不在允许集合。'
    }
    $path = [IO.Path]::GetFullPath((Join-Path $Base $Relative.Replace('/', '\')))
    if (-not $path.StartsWith($Base + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '差量路径越界。' }
    Assert-XenNoReparsePathChain $path '差量文件'
    return $path
}
function Get-DeltaHash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-WorkerStopped {
    $names = @('Xen', 'XenLauncher')
    if (@($packet.files | Where-Object { $_.path -ceq 'tools/source/xen_source_context.exe' }).Count -gt 0) {
        $names += 'xen_source_context'
    }
    $selectedRecoilTools = @($recoilTools | Where-Object { $relative = $_; @($packet.files | Where-Object { $_.path -ceq $relative }).Count -gt 0 })
    foreach ($relative in $selectedRecoilTools) { $names += [IO.Path]::GetFileNameWithoutExtension($relative) }
    foreach ($process in @(Get-Process -Name $names -ErrorAction SilentlyContinue)) {
        $processPath = $process.Path
        if (-not $processPath -or $processPath.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'XEN_WORKER_RUNNING: 请先退出该包 Worker/Launcher 及本次更新工具后再更新；不会强制结束进程。'
        }
    }
    # 同时拒绝不能独占打开的目标 Worker，避免未列入进程快照的已加载映像。
    $worker = Resolve-DeltaFile $root $workerRelative
    $handle = [IO.File]::Open($worker, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $handle.Dispose()
    if ($names -contains 'xen_source_context') {
        $tool = Resolve-DeltaFile $root 'tools/source/xen_source_context.exe'
        $handle = [IO.File]::Open($tool, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $handle.Dispose()
    }
    foreach ($relative in $selectedRecoilTools) {
        $tool = Resolve-DeltaFile $root $relative
        $handle = [IO.File]::Open($tool, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $handle.Dispose()
    }
}
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $packet.files) {
    if (-not $seen.Add([string]$entry.path) -or $entry.path -cnotin $allowed -or
        $entry.new_sha256 -notmatch '^[0-9a-f]{64}$' -or
        ($entry.old_sha256 -ne '' -and $entry.old_sha256 -notmatch '^[0-9a-f]{64}$')) { throw '差量文件记录无效。' }
    if ((Get-DeltaHash (Resolve-DeltaFile $stage $entry.path)) -cne $entry.new_sha256 -or
        (Get-DeltaHash (Resolve-DeltaFile $root $entry.path)) -cne $entry.old_sha256) {
        throw "差量新文件或既有基线 SHA 不一致：$($entry.path)"
    }
}
foreach ($required in @($workerRelative, 'manifest.json', 'tools/acceptance/WORKER-UPDATE.json')) {
    if (-not $seen.Contains($required)) { throw '差量缺少必需载荷。' }
}
$manifest = Get-Content -LiteralPath (Join-Path $stage 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (@($manifest.PSObject.Properties.Name).Count -ne 5 -or $manifest.schema -ne 1 -or $manifest.product -cne 'Xen') {
    throw '差量 manifest 不符合生产五字段合同。'
}
foreach ($entry in $packet.files) {
    if ($entry.path -ceq 'manifest.json') { continue }
    $records = @($manifest.files | Where-Object { $_.path -ceq $entry.path })
    if ($records.Count -ne 1 -or $records[0].sha256 -cne $entry.new_sha256 -or
        [long]$records[0].size -ne (Get-Item -LiteralPath (Resolve-DeltaFile $stage $entry.path)).Length) {
        throw '差量载荷与最终清单不一致。'
    }
}
function Assert-ProtectedFiles {
    if (@($packet.protected_files).Count -ne 2) { throw '缺少用户配置保护记录。' }
    $protectedNames = @($packet.protected_files | ForEach-Object { $_.path })
    if (@(Compare-Object ($protectedNames | Sort-Object) @('cache/model-workspace/settings.json', 'config.ini')).Count -ne 0) {
        throw '用户配置保护集合无效。'
    }
    foreach ($entry in $packet.protected_files) {
        if ((Get-DeltaHash (Resolve-DeltaFile $root $entry.path)) -cne $entry.sha256) {
            throw '用户配置或工作区设置在发布期间变化，请重新准备差量。'
        }
    }
}
Assert-ProtectedFiles
Assert-WorkerStopped
if ($CheckOnly) { Write-Host '差量前置检查通过；未替换文件。'; return }
$applied = [Collections.Generic.List[object]]::new()
try {
    # 每个文件在服务器本地原子替换，manifest 最后；失败按逆序恢复本轮备份。
    $ordered = @($packet.files | Where-Object { $_.path -cne 'manifest.json' }) +
        @($packet.files | Where-Object { $_.path -ceq 'manifest.json' })
    foreach ($entry in $ordered) {
        Assert-ProtectedFiles
        Assert-WorkerStopped
        $target = Resolve-DeltaFile $root $entry.path
        $source = Resolve-DeltaFile $stage $entry.path
        if ((Get-DeltaHash $target) -cne $entry.old_sha256) { throw '目标在替换前变化。' }
        $backup = Join-Path $stage ("backup-$([guid]::NewGuid().ToString('N'))")
        if (Test-Path -LiteralPath $target) {
            [IO.File]::Replace($source, $target, $backup)
        } else {
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [IO.File]::Move($source, $target)
        }
        $applied.Add([pscustomobject]@{ target = $target; backup = $backup; existed = ($entry.old_sha256 -ne '') })
        if ((Get-DeltaHash $target) -cne $entry.new_sha256) { throw '差量替换后 SHA 回读失败。' }
    }
    Assert-ProtectedFiles
    Write-Host 'Worker 差量本地替换完成；用户配置及工作区设置保持原字节。'
} catch {
    for ($index = $applied.Count - 1; $index -ge 0; $index--) {
        $entry = $applied[$index]
        if ($entry.existed) {
            $discard = Join-Path $stage ("rollback-discard-$([guid]::NewGuid().ToString('N'))")
            [IO.File]::Replace($entry.backup, $entry.target, $discard)
        }
        else { Remove-Item -LiteralPath $entry.target -Force }
    }
    throw
}
