param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Import-Module (Join-Path $RepositoryRoot 'scripts/path_safety.psm1') -Force
$source = Join-Path $RepositoryRoot 'scripts/publish_aim_worker_delta.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw '生产脚本语法错误' }
foreach ($name in @('Read-Json', 'Replace-FileAtomically', 'Copy-Atomic', 'Copy-AimPrepareTransaction')) {
    $node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $false)
    if ($null -eq $node) { throw "缺少真实生产函数：$name" }
    . ([scriptblock]::Create($node.Extent.Text))
}
$root = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('xen-prepare-commit-' + [guid]::NewGuid().ToString('N'))))
[IO.Directory]::CreateDirectory($root) | Out-Null
$ownedPrefix = $root + [IO.Path]::DirectorySeparatorChar
function Remove-Item {
    param([string]$LiteralPath, [switch]$Force, [switch]$Recurse)
    $resolved = [IO.Path]::GetFullPath($LiteralPath)
    if (-not $resolved.StartsWith($ownedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw '测试拒绝清理临时根以外的路径' }
    $state = $script:cleanupFault
    if ([IO.Path]::GetFileName($resolved).StartsWith($state.Target + '.backup-', [StringComparison]::Ordinal)) {
        ++$state.Hits
        $target = Join-Path $state.Local $state.Target
        $actual = [IO.File]::ReadAllText($target)
        if ($state.Hits -eq 1 -and $actual -ne $state.NewText) { throw '注入点必须在真实File.Replace提交之后' }
        if ($state.Case -eq 'concurrent-change' -and $state.Hits -eq 1) { [IO.File]::WriteAllText($target, '用户并发修改') }
        if ($state.Hits -eq 1 -or ($state.Case -eq 'rollback-cleanup' -and $state.Hits -eq 2)) { throw '注入：真实替换后的备份清理失败' }
    }
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $resolved -Force:$Force -Recurse:$Recurse
}
$failures = 0
try {
    foreach ($case in @('healthy', 'config-cleanup', 'manifest-cleanup', 'rollback-cleanup', 'concurrent-change')) {
        $local = Join-Path $root "$case/local"
        $remote = Join-Path $root "$case/source"
        [IO.Directory]::CreateDirectory($local) | Out-Null
        [IO.Directory]::CreateDirectory($remote) | Out-Null
        [IO.File]::WriteAllText((Join-Path $local 'config.ini'), '旧配置')
        [IO.File]::WriteAllText((Join-Path $local 'manifest.json'), '旧manifest')
        [IO.File]::WriteAllText((Join-Path $remote 'config.ini'), '新配置')
        $hash = (Get-FileHash -LiteralPath (Join-Path $remote 'config.ini')).Hash
        $manifest = @{ files = @(@{ path = 'config.ini'; sha256 = $hash }) } | ConvertTo-Json -Depth 4
        [IO.File]::WriteAllText((Join-Path $remote 'manifest.json'), $manifest)
        $target = if ($case -eq 'healthy') { 'never-injected' } elseif ($case -eq 'manifest-cleanup') { 'manifest.json' } else { 'config.ini' }
        $script:cleanupFault = @{ Case = $case; Local = $local; Target = $target; Hits = 0; NewText = if ($target -eq 'manifest.json') { $manifest } else { '新配置' } }
        $errorText = ''
        try { Copy-AimPrepareTransaction $remote $local } catch { $errorText = $_.Exception.Message }
        $actualConfig = [IO.File]::ReadAllText((Join-Path $local 'config.ini'))
        $actualManifest = [IO.File]::ReadAllText((Join-Path $local 'manifest.json'))
        $stages = @(Get-ChildItem -LiteralPath $local -Directory -Filter '.prepare-copy-*')
        try {
            if ($case -eq 'healthy') {
                if ($errorText -or $actualConfig -ne '新配置' -or $actualManifest -ne $manifest -or $stages.Count -ne 0) { throw '正常成对提交失败' }
            } elseif ($case -in @('config-cleanup', 'manifest-cleanup')) {
                if (-not $errorText -or $script:cleanupFault.Hits -lt 1 -or $actualConfig -ne '旧配置' -or $actualManifest -ne '旧manifest' -or $stages.Count -ne 0) { throw '提交后清理异常没有恢复旧配对' }
            } else {
                $expectedConfig = if ($case -eq 'concurrent-change') { '用户并发修改' } else { '旧配置' }
                if (-not $errorText -or $actualConfig -ne $expectedConfig -or $actualManifest -ne '旧manifest' -or $stages.Count -ne 1) { throw '回滚未知/并发变化未保留恢复阶段' }
                foreach ($name in @('config.ini', 'manifest.json')) {
                    $expected = if ($name -eq 'config.ini') { '旧配置' } else { '旧manifest' }
                    if ([IO.File]::ReadAllText((Join-Path $stages[0].FullName "$name.backup")) -ne $expected) { throw '原始成对恢复材料不完整' }
                }
            }
            $lockProbe = [IO.File]::Open((Join-Path $local '.worker-delta.lock'), [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $lockProbe.Dispose()
            Write-Host "${case}: PASS hits=$($script:cleanupFault.Hits)"
        } catch {
            ++$failures
            Write-Host "${case}: FAIL $($_.Exception.Message) config=$actualConfig stages=$($stages.Count) hits=$($script:cleanupFault.Hits)"
            Write-Host "生产事务返回：$errorText"
        }
    }
    if ($failures) { throw "$failures 个真实Prepare提交状态案例失败" }
} finally { Write-Host "本机配对事务夹具保留：$root" }
