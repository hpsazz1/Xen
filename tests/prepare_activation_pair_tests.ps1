param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$source = Join-Path $RepositoryRoot 'scripts/invoke_aim_manual_acceptance.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-prepare-activation-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$ownedPrefix = [IO.Path]::GetFullPath($root) + [IO.Path]::DirectorySeparatorChar
function Remove-Item {
    param([string]$LiteralPath, [switch]$Force, [switch]$Recurse)
    $resolved = [IO.Path]::GetFullPath($LiteralPath)
    if (-not $resolved.StartsWith($ownedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw '测试拒绝删除夹具外路径' }
    $state = $cleanupFault
    if ([IO.Path]::GetDirectoryName($resolved) -eq $state.Package -and
        [IO.Path]::GetFileName($resolved).StartsWith($state.Target + '.backup-', [StringComparison]::Ordinal)) {
        ++$state.Hits
        if ($state.Hits -eq 1) {
            $target = Join-Path $state.Package $state.Target
            if ((Get-FileHash $target).Hash -ceq $state.OldHash) { throw '未到达实际替换提交后的清理点' }
            if ($state.Case -eq 'concurrent-change') { [IO.File]::WriteAllText($target, 'concurrent user config') }
            if ($state.Case -eq 'rollback-locked') {
                $state.Handle = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            }
            throw 'Injected cleanup error after actual file replacement.'
        }
    }
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $resolved -Force:$Force -Recurse:$Recurse
}
$failures = 0
function Assert-Condition([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Invoke-Prepare([string]$Package, [string]$Run) {
    & $source -PackageRoot $Package -RunDirectory $Run -Mode Prepare -Scenario Static -Profile tracking | Out-Null
}
foreach ($case in @('healthy', 'manifest-locked', 'package-locked', 'missing-config-record',
        'config-cleanup', 'manifest-cleanup', 'rollback-locked', 'concurrent-change')) {
    $package = Join-Path $root "$case/package"
    $run = Join-Path $root "$case/run"
    [IO.Directory]::CreateDirectory($package) | Out-Null
    $records = @()
    foreach ($relative in @('XenLauncher.exe', 'config.ini', 'runtimes/nvidia/Xen.exe',
            'tools/invoke_aim_manual_acceptance.ps1', 'tools/aim_report.ps1',
            'tools/aim_control_diagnostics.ps1', 'tools/aim_fixed_scene_analysis.ps1', 'models/fixture.onnx')) {
        $path = Join-Path $package $relative
        [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
        [IO.File]::WriteAllText($path, "synthetic non-executable fixture: $relative")
        $records += [ordered]@{ path = $relative; size = [long](Get-Item $path).Length;
            sha256 = (Get-FileHash $path).Hash.ToLowerInvariant(); source = 'synthetic' }
    }
    if ($case -eq 'missing-config-record') { $records = @($records | Where-Object { $_.path -ne 'config.ini' }) }
    $manifestPath = Join-Path $package 'manifest.json'
    [IO.File]::WriteAllText($manifestPath, (@{schema=1;product='Xen';git_commit=('a'*40);
        runtimes=@('nvidia','directml','openvino');files=$records} | ConvertTo-Json -Depth 8))
    $configPath = Join-Path $package 'config.ini'
    $oldConfig = (Get-FileHash $configPath).Hash
    $oldManifest = (Get-FileHash $manifestPath).Hash
    $target = if ($case -eq 'manifest-cleanup') { 'manifest.json' } elseif ($case -in @('config-cleanup','rollback-locked','concurrent-change')) { 'config.ini' } else { 'never-injected' }
    $cleanupFault = @{ Case=$case; Package=$package; Target=$target; Hits=0; Handle=$null;
        OldHash=if ($target -eq 'manifest.json') { $oldManifest } else { $oldConfig } }
    $handle = $null; $errorText = ''
    try {
        if ($case -eq 'manifest-locked') {
            $handle = [IO.File]::Open($manifestPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        } elseif ($case -eq 'package-locked') {
            $handle = [IO.File]::Open((Join-Path $package '.worker-delta.lock'), [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        }
        try { Invoke-Prepare $package $run } catch { $errorText = $_.Exception.Message }
        if ($case -eq 'healthy') {
            Assert-Condition (-not $errorText -and (Test-Path (Join-Path $run 'task.json'))) '健康Prepare未产生完整任务'
            $manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
            $record = @($manifest.files | Where-Object { $_.path -eq 'config.ini' })
            Assert-Condition ($record.Count -eq 1 -and $record[0].sha256 -ieq (Get-FileHash $configPath).Hash) '健康配置与manifest不匹配'
        } elseif ($case -in @('rollback-locked', 'concurrent-change')) {
            Assert-Condition ($errorText -match 'rollback not verified; retain') '未知恢复结果未明确报错'
            Assert-Condition (-not (Test-Path (Join-Path $run 'task.json'))) '未知恢复仍产生完整任务'
            Assert-Condition ((Get-FileHash $manifestPath).Hash -ceq $oldManifest) '未提交manifest被改变'
            if ($case -eq 'concurrent-change') {
                Assert-Condition ([IO.File]::ReadAllText($configPath) -ceq 'concurrent user config') '用户并发字节被覆盖'
            } else { Assert-Condition ((Get-FileHash $configPath).Hash -cne $oldConfig) '回滚占用没有真实阻止恢复' }
            $stages = @(Get-ChildItem -LiteralPath $package -Directory -Filter '.prepare-activate-*')
            Assert-Condition ($stages.Count -eq 1) '未知恢复没有保留唯一阶段'
            Assert-Condition ((Get-FileHash (Join-Path $stages[0].FullName 'config.ini.backup')).Hash -ceq $oldConfig) '旧config恢复材料缺失'
            Assert-Condition ((Get-FileHash (Join-Path $stages[0].FullName 'manifest.json.backup')).Hash -ceq $oldManifest) '旧manifest恢复材料缺失'
        } else {
            Assert-Condition (-not [string]::IsNullOrWhiteSpace($errorText)) '应拒绝的Prepare没有失败'
            Assert-Condition (-not (Test-Path (Join-Path $run 'task.json'))) '失败Prepare仍产生完整任务'
            Assert-Condition ((Get-FileHash $configPath).Hash -ceq $oldConfig) '失败Prepare未恢复旧config'
            Assert-Condition ((Get-FileHash $manifestPath).Hash -ceq $oldManifest) '失败Prepare未保留旧manifest'
        }
        if ($case -notin @('rollback-locked', 'concurrent-change')) {
            Assert-Condition (@(Get-ChildItem -LiteralPath $package -Directory -Filter '.prepare-activate-*').Count -eq 0) '已完成恢复仍残留事务阶段'
        }
        if ($target -ne 'never-injected') { Assert-Condition ($cleanupFault.Hits -eq 1) '提交后故障点未执行一次' }
        Write-Host "${case}: PASS"
    } catch {
        ++$failures
        Write-Host "${case}: FAIL $($_.Exception.Message); production error=$errorText"
    } finally {
        if ($null -ne $handle) { $handle.Dispose() }
        if ($null -ne $cleanupFault.Handle) { $cleanupFault.Handle.Dispose() }
    }
    $lockProbe = [IO.File]::Open((Join-Path $package '.worker-delta.lock'), [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $lockProbe.Dispose()
    if ($case -in @('manifest-locked', 'package-locked')) {
        try {
            $retry = Join-Path $root "$case/retry"
            Invoke-Prepare $package $retry
            Assert-Condition (Test-Path (Join-Path $retry 'task.json')) '解除占用后的Prepare未恢复'
            $lockProbe = [IO.File]::Open((Join-Path $package '.worker-delta.lock'), [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $lockProbe.Dispose()
            Write-Host "${case}-retry: PASS"
        } catch { ++$failures; Write-Host "${case}-retry: FAIL $($_.Exception.Message)" }
    }
}
Write-Host "仅合成文件、全程Prepare；夹具保留：$root"
if ($failures) { throw "$failures 个真实Prepare激活事务案例失败" }
