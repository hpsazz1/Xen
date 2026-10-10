param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $RepositoryRoot 'scripts/path_safety.psm1') -Force
function Import-Functions([string]$Path) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
    foreach ($function in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
        . ([scriptblock]::Create($function.Extent.Text.Replace('function ', 'function global:')))
    }
}
Import-Functions (Join-Path $RepositoryRoot 'scripts/benchmark_runtime.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-publication-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $ort = Join-Path $root 'ort'; $opencv = Join-Path $root 'opencv/build/x64/vc16/lib'
    $crt = Join-Path $root 'VS/VC/Redist/MSVC/14.44/x64/Microsoft.VC143.CRT'
    foreach ($directory in @($ort, $opencv, $crt)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    $dll = Join-Path $crt 'vcruntime140.dll'; [IO.File]::WriteAllText($dll, 'official-crt')
    $cache = Join-Path $root 'CMakeCache.txt'
    @("ONNXRUNTIME_ROOT:PATH=$ort", "OpenCV_DIR:PATH=$opencv", "XEN_MSVC_REDIST_ROOT:PATH=$crt") | Set-Content $cache
    $files = @{ 'vcruntime140.dll' = @{ sha256 = (Get-FileHash $dll).Hash } }
    Assert-RuntimeDllOrigins $files $cache 'cpu' | Out-Null
    $files['vcruntime140.dll'].sha256 = ('0' * 64)
    $rejected = $false
    try { Assert-RuntimeDllOrigins $files $cache 'cpu' | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Changed CRT must be rejected' }
    Write-Host 'F17 official CRT origin PASS'
    Import-Functions (Join-Path $RepositoryRoot 'scripts/benchmark_detector_videos.ps1')
    $csv = Join-Path $root 'report.csv'; $json = Join-Path $root 'report.json'
    $pending = Join-Path $root 'new.csv'
    [IO.File]::WriteAllText($csv, 'old-csv'); [IO.File]::WriteAllText($json, 'old-json')
    [IO.File]::WriteAllText($pending, 'new-csv')
    $script:failTarget = $json
    $script:jsonRenameReached = $false
    function global:Publish-ExistingFileAtomically([string]$Source, [string]$Destination) {
        if ($Destination -eq $script:failTarget) {
            $script:jsonRenameReached = $true
            if ($script:parallelCsvChange) { [IO.File]::WriteAllText($script:csvTarget, 'parallel-csv') }
            throw 'injected JSON rename failure'
        }
        if (Test-Path -LiteralPath $Destination) {
            $backup = $Destination + '.mock-backup-' + [guid]::NewGuid().ToString('N')
            [IO.File]::Replace($Source, $Destination, $backup)
            Remove-Item -LiteralPath $backup -Force
        }
        else { [IO.File]::Move($Source, $Destination) }
    }
    function global:Publish-JsonAtomically($Value, [string]$Path) {
        if ($Path -eq $script:failTarget) { throw 'injected JSON publication failure' }
        [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8))
    }
    try {
        if (Get-Command Publish-BenchmarkReport -ErrorAction SilentlyContinue) {
            Publish-BenchmarkReport $pending $csv @{ report = @{} } $json
        } else {
            Publish-ExistingFileAtomically $pending $csv
            Publish-JsonAtomically @{ report = @{} } $json
        }
        throw 'Expected publication failure'
    } catch { if ($_.Exception.Message -eq 'Expected publication failure') { throw } }
    if (-not $script:jsonRenameReached -or [IO.File]::ReadAllText($csv) -ne 'old-csv' -or [IO.File]::ReadAllText($json) -ne 'old-json') {
        throw 'F18 failed JSON publication left CSV/JSON from different runs'
    }
    Write-Host 'F18 paired publication rollback PASS'
    [IO.File]::WriteAllText($pending, 'new-csv')
    $script:parallelCsvChange = $true; $script:csvTarget = $csv
    $rejected = $false
    try { Publish-BenchmarkReport $pending $csv @{ report = @{} } $json } catch { $rejected = $true; $csvFailure = $_.Exception.Message }
    if (-not $rejected -or [IO.File]::ReadAllText($csv) -ne 'parallel-csv' -or
        @(Get-ChildItem $root -Filter 'report.csv.backup-*').Count -ne 1) {
        throw "F18 recovery must preserve concurrent CSV changes and recovery backup: content=$([IO.File]::ReadAllText($csv)); backups=$(@(Get-ChildItem $root -Filter 'report.csv.backup-*').Count); error=$csvFailure"
    }
    $script:parallelCsvChange = $false
    Write-Host 'F18 recovery preserves concurrent CSV change PASS'
    Import-Functions (Join-Path $RepositoryRoot 'scripts/publish_aim_worker_delta.ps1')
    $local = Join-Path $root 'local'; $remote = Join-Path $root 'remote'
    foreach ($directory in @($local, $remote)) { New-Item -ItemType Directory -Path $directory | Out-Null }
    foreach ($name in @('config.ini', 'manifest.json')) {
        [IO.File]::WriteAllText((Join-Path $local $name), "old-$name")
        [IO.File]::WriteAllText((Join-Path $remote $name), "new-$name")
    }
    @{ files = @(@{ path = 'config.ini'; sha256 = (Get-FileHash (Join-Path $remote 'config.ini')).Hash.ToLowerInvariant() }) } |
        ConvertTo-Json -Depth 8 | Set-Content (Join-Path $remote 'manifest.json')
    $script:failed = $false
    $script:prepareManifest = Join-Path $local 'manifest.json'
    function global:Replace-FileAtomically([string]$Pending, [string]$Target) {
        if ($Target -eq $script:prepareManifest -and -not $script:failed) {
            if ($script:parallelPrepareChange) { [IO.File]::WriteAllText($script:prepareConfig, 'parallel-config') }
            $script:failed = $true; throw 'injected Prepare manifest failure'
        }
        if (Test-Path -LiteralPath $Target) {
            $backup = $Target + '.mock-backup-' + [guid]::NewGuid().ToString('N')
            [IO.File]::Replace($Pending, $Target, $backup)
            Remove-Item -LiteralPath $backup -Force
        }
        else { [IO.File]::Move($Pending, $Target) }
    }
    try {
        if (Get-Command Copy-AimPrepareTransaction -ErrorAction SilentlyContinue) {
            Copy-AimPrepareTransaction $remote $local
        } else {
            Copy-Atomic (Join-Path $remote 'config.ini') (Join-Path $local 'config.ini')
            Copy-Atomic (Join-Path $remote 'manifest.json') (Join-Path $local 'manifest.json')
        }
        throw 'Expected Prepare failure'
    } catch { if ($_.Exception.Message -eq 'Expected Prepare failure') { throw } }
    if (-not $script:failed) { throw 'Prepare regression must reach the injected second-file failure' }
    foreach ($name in @('config.ini', 'manifest.json')) {
        if ([IO.File]::ReadAllText((Join-Path $local $name)) -ne "old-$name") {
            throw 'F15 Prepare failure left local configuration and manifest inconsistent'
        }
    }
    Write-Host 'F15 Prepare pair rollback PASS'
    [IO.File]::WriteAllText((Join-Path $remote 'config.ini'), 'changed-config-after-manifest')
    $rejected = $false
    try { Copy-AimPrepareTransaction $remote $local } catch { $rejected = $true }
    if (-not $rejected -or [IO.File]::ReadAllText((Join-Path $local 'config.ini')) -ne 'old-config.ini') {
        throw 'F15 mismatched Prepare snapshot must be rejected before publishing'
    }
    Write-Host 'F15 Prepare frozen manifest binding PASS'
    [IO.File]::WriteAllText((Join-Path $remote 'config.ini'), 'new-config.ini')
    $script:failed = $false; $script:parallelPrepareChange = $true
    $script:prepareConfig = Join-Path $local 'config.ini'
    $rejected = $false
    try { Copy-AimPrepareTransaction $remote $local } catch { $rejected = $true }
    if (-not $rejected -or [IO.File]::ReadAllText($script:prepareConfig) -ne 'parallel-config' -or
        @(Get-ChildItem $local -Filter '.prepare-copy-*' -Directory).Count -ne 1) {
        throw 'F15 Prepare recovery must preserve concurrent configuration and backup'
    }
    $script:parallelPrepareChange = $false
    Write-Host 'F15 Prepare recovery preserves concurrent config PASS'
    $applyScript = Join-Path $RepositoryRoot 'scripts/apply_worker_delta.ps1'
    $tools = @('tools/invoke_aim_manual_acceptance.ps1', 'tools/aim_report.ps1',
        'tools/aim_control_diagnostics.ps1', 'tools/aim_fixed_scene_analysis.ps1')
    function New-DeltaFixture([string]$Name) {
        $package = Join-Path $root $Name
        $stageName = '.worker-delta-' + [guid]::NewGuid().ToString('N')
        $stage = Join-Path $package $stageName
        $records = @(); $files = @()
        foreach ($relative in $tools) {
            foreach ($base in @($package, $stage)) {
                $path = Join-Path $base $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
                [IO.File]::WriteAllText($path, $(if ($base -eq $package) { "old-$relative" } else { "new-$relative" }))
            }
            $new = Join-Path $stage $relative
            $newHash = (Get-FileHash $new).Hash.ToLowerInvariant()
            $records += @{ path = $relative; sha256 = $newHash; size = (Get-Item $new).Length; runtime = ''; source = 'fixture' }
            $files += @{ path = $relative; old_sha256 = (Get-FileHash (Join-Path $package $relative)).Hash.ToLowerInvariant(); new_sha256 = $newHash }
        }
        [IO.File]::WriteAllText((Join-Path $package 'manifest.json'), 'old-manifest')
        $manifest = @{ schema = 1; product = 'Xen'; git_commit = ('a' * 40); runtimes = @(); files = $records }
        [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8))
        $files += @{ path = 'manifest.json'; old_sha256 = (Get-FileHash (Join-Path $package 'manifest.json')).Hash.ToLowerInvariant();
            new_sha256 = (Get-FileHash (Join-Path $stage 'manifest.json')).Hash.ToLowerInvariant() }
        $protected = @()
        foreach ($relative in @('config.ini', 'cache/model-workspace/settings.json')) {
            $path = Join-Path $package $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
            [IO.File]::WriteAllText($path, 'preserved')
            $protected += @{ path = $relative; sha256 = (Get-FileHash $path).Hash.ToLowerInvariant() }
        }
        @{ schema = 1; runtime = 'nvidia'; aim_tools_only = $true; files = $files; protected_files = $protected } |
            ConvertTo-Json -Depth 8 | Set-Content (Join-Path $stage 'delta.json')
        return @{ PackageRoot = $package; StageName = $stageName }
    }
    function Assert-DeltaRestored($Parameters) {
        foreach ($relative in $tools) {
            if ([IO.File]::ReadAllText((Join-Path $Parameters.PackageRoot $relative)) -ne "old-$relative") {
                throw "F15 rollback failed for $relative"
            }
        }
        if ([IO.File]::ReadAllText((Join-Path $Parameters.PackageRoot 'manifest.json')) -ne 'old-manifest') {
            throw 'F15 manifest rollback failed'
        }
    }
    $parameters = New-DeltaFixture 'lost-ssh-receipt'
    & $applyScript @parameters
    # 远端完成后回执丢失：恢复依赖落盘意图及实际哈希，不能把未知解释为成功。
    & $applyScript @parameters -Rollback
    Assert-DeltaRestored $parameters
    & $applyScript @parameters -Rollback
    Assert-DeltaRestored $parameters
    Write-Host 'F15 lost-receipt recovery and idempotent rollback PASS'
    $parameters = New-DeltaFixture 'manifest-locked'
    $handle = [IO.File]::Open((Join-Path $parameters.PackageRoot 'manifest.json'), [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $rejected = $false
    try { & $applyScript @parameters } catch { $rejected = $true } finally { $handle.Dispose() }
    if (-not $rejected) { throw 'Expected locked manifest rejection' }
    $journal = Get-Content (Join-Path $parameters.PackageRoot "$($parameters.StageName)/rollback.json") -Raw | ConvertFrom-Json
    if (@($journal.entries).Count -ne 5) { throw 'Manifest failure must occur after all four tool replacements' }
    Assert-DeltaRestored $parameters
    Write-Host 'F15 locked manifest rolls back already replaced tools PASS'
    $parameters = New-DeltaFixture 'concurrent-publisher'
    $handle = [IO.File]::Open((Join-Path $parameters.PackageRoot '.worker-delta.lock'), [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $rejected = $false
    try { & $applyScript @parameters } catch { $rejected = $true } finally { $handle.Dispose() }
    if (-not $rejected) { throw 'Expected concurrent publisher rejection' }
    Assert-DeltaRestored $parameters
    Write-Host 'F15 concurrent publisher lock PASS'
    $parameters = New-DeltaFixture 'reparse-lock'
    $outside = Join-Path $root 'outside-lock-target'
    New-Item -ItemType Directory -Path $outside | Out-Null
    $lockPath = Join-Path $parameters.PackageRoot '.worker-delta.lock'
    New-Item -ItemType Junction -Path $lockPath -Target $outside | Out-Null
    $rejection = ''
    try { & $applyScript @parameters } catch { $rejection = $_.Exception.Message }
    if ($rejection -notmatch 'reparse point') { throw 'F15 lock reparse leaf must be rejected by path contract' }
    # 只删除本测试创建的junction本身，保留目标目录供后续核对。
    [IO.Directory]::Delete($lockPath)
    Assert-DeltaRestored $parameters
    $lockPath = "$csv.publish.lock"
    Remove-Item -LiteralPath $lockPath -Force
    New-Item -ItemType Junction -Path $lockPath -Target $outside | Out-Null
    [IO.File]::WriteAllText($pending, 'new-csv')
    $rejection = ''
    try { Publish-BenchmarkReport $pending $csv @{ report = @{} } $json } catch { $rejection = $_.Exception.Message }
    if ($rejection -notmatch 'reparse point') { throw 'F18 lock reparse leaf must be rejected by path contract' }
    [IO.Directory]::Delete($lockPath)
    $lockPath = Join-Path $local '.worker-delta.lock'
    Remove-Item -LiteralPath $lockPath -Force
    New-Item -ItemType Junction -Path $lockPath -Target $outside | Out-Null
    $rejection = ''
    try { Copy-AimPrepareTransaction $remote $local } catch { $rejection = $_.Exception.Message }
    if ($rejection -notmatch 'reparse point') { throw 'F15 Prepare lock reparse leaf must be rejected by path contract' }
    [IO.Directory]::Delete($lockPath)
    Write-Host 'F15/F18 lock reparse leaf rejection PASS'
    $parameters = New-DeltaFixture 'changed-after-apply'
    & $applyScript @parameters
    $changed = Join-Path $parameters.PackageRoot $tools[0]
    [IO.File]::WriteAllText($changed, 'parallel-user-change')
    $rejected = $false
    try { & $applyScript @parameters -Rollback } catch { $rejected = $true }
    if (-not $rejected -or [IO.File]::ReadAllText($changed) -ne 'parallel-user-change') {
        throw 'F15 recovery must preserve concurrent user changes'
    }
    Write-Host 'F15 recovery preserves concurrent user changes PASS'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
