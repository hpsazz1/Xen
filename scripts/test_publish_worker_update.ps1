param([string]$TestRoot = (Join-Path $PSScriptRoot '..\cache\worker-update-tests'), [string]$ManifestValidator = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot -RepositoryRoot (Split-Path -Parent $PSScriptRoot)
$runRoot = $ownedTest.RootPath
$publisher = Join-Path $PSScriptRoot 'publish_worker_update.ps1'
$script:passed = 0
function Assert-UpdateTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:passed++
}
function Write-UpdateFixture([string]$Path, [string]$Value) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}
function Assert-UpdateReject([hashtable]$Parameters, [string]$Message) {
    $rejected = $false
    try { & $publisher @Parameters } catch { $rejected = $true }
    Assert-UpdateTest $rejected $Message
    Assert-UpdateTest (-not (Test-Path -LiteralPath $Parameters.OutputDirectory)) "$Message 未产生正式目录"
}
function Assert-ProductionManifest([string]$PackagePath, [switch]$MutableFilesMayDiffer) {
    $document = Get-Content -LiteralPath (Join-Path $PackagePath 'manifest.json') -Raw | ConvertFrom-Json
    Assert-UpdateTest (@($document.PSObject.Properties.Name).Count -eq 5) '生产 manifest 顶层严格五字段'
    foreach ($record in $document.files) {
        if ($MutableFilesMayDiffer -and $record.path -in @('config.ini', 'cache/model-workspace/settings.json')) { continue }
        $actualHash = (Get-FileHash -LiteralPath (Join-Path $PackagePath $record.path) -Algorithm SHA256).Hash
        Assert-UpdateTest ($actualHash -ieq $record.sha256) "最终清单哈希 $($record.path)"
    }
    if ($ManifestValidator) {
        & $ManifestValidator --validate-package $PackagePath
        Assert-UpdateTest ($LASTEXITCODE -eq 0) '生产 load_release_manifest 接受发布包'
    }
}
try {
    if ($ManifestValidator) {
        # 旧测试程序会忽略未知参数并返回成功；先用不存在的包证明只读校验模式真正生效。
        $savedErrorPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $validatorOutput = @(& $ManifestValidator --validate-package (Join-Path $runRoot 'missing-package') 2>&1)
            $validatorExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = $savedErrorPreference }
        Assert-UpdateTest ($validatorExit -ne 0) '生产校验器拒绝不存在包，禁止旧程序忽略参数假绿'
    }
    $sourceRoot = Join-Path $runRoot 'source'
    New-Item -ItemType Directory -Path $sourceRoot | Out-Null
    $git = (Get-Command git -ErrorAction Stop).Source
    & $git -C $sourceRoot init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'fixture git init failed' }
    Write-UpdateFixture (Join-Path $sourceRoot 'fixture.txt') 'worker source'
    & $git -C $sourceRoot add fixture.txt
    & $git -C $sourceRoot -c user.name=XenTest -c user.email=xen-test@example.invalid commit --quiet -m '发布夹具'
    if ($LASTEXITCODE -ne 0) { throw 'fixture git commit failed' }
    $commit = (& $git -C $sourceRoot rev-parse HEAD).Trim()
    $buildRoot = Join-Path $runRoot 'build'
    Write-UpdateFixture (Join-Path $buildRoot 'Release\Xen.exe') 'updated-worker-fixture'
    Write-UpdateFixture (Join-Path $buildRoot 'Release\xen_source_context.exe') 'updated-source-context-fixture'
    Write-UpdateFixture (Join-Path $buildRoot 'Release\XenLauncher.exe') 'new-launcher-with-current-config'
    $identityPath = Join-Path $buildRoot 'xen-build-identity.json'
    $identity = [ordered]@{ schema = 1; source_root = $sourceRoot; git_commit = $commit; git_dirty = $false; runtime = 'nvidia' }
    Write-UpdateFixture $identityPath ($identity | ConvertTo-Json)
    $baseRoot = Join-Path $runRoot 'base'
    $records = @()
    $routes = @()
    foreach ($runtime in @('nvidia', 'directml', 'openvino')) {
        $relative = "runtimes/$runtime/Xen.exe"
        Write-UpdateFixture (Join-Path $baseRoot $relative) "old-$runtime-worker"
        $backends = if ($runtime -eq 'nvidia') { @('cpu', 'cuda', 'tensorrt') } else { @($runtime) }
        $routes += [ordered]@{ id = $runtime; executable = $relative; backends = @($backends) }
    }
    Write-UpdateFixture (Join-Path $baseRoot 'XenLauncher.exe') 'old-launcher-with-old-config'
    Write-UpdateFixture (Join-Path $baseRoot 'config.ini') '[fixture]'
    Write-UpdateFixture (Join-Path $baseRoot 'cache/model-workspace/settings.json') '{}'
    Write-UpdateFixture (Join-Path $baseRoot 'tools/acceptance/PACKAGE-NOTES.md') 'old package notes'
    Write-UpdateFixture (Join-Path $baseRoot 'tools/acceptance/MANUAL-ACCEPTANCE.md') 'old manual acceptance'
    Write-UpdateFixture (Join-Path $baseRoot 'tools/source/xen_source_context.exe') 'old source context'
    foreach ($file in Get-ChildItem -LiteralPath $baseRoot -Recurse -File) {
        $relative = $file.FullName.Substring($baseRoot.Length + 1).Replace('\', '/')
        $runtime = if ($relative -match '^runtimes/([^/]+)/') { $Matches[1] } else { '' }
        $records += [ordered]@{ path = $relative; runtime = $runtime; size = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); source = "original/$relative" }
    }
    $baseManifest = [ordered]@{ schema = 1; product = 'Xen'; git_commit = ('a' * 40); runtimes = $routes; files = $records }
    $baseManifestPath = Join-Path $baseRoot 'manifest.json'
    $baseJson = $baseManifest | ConvertTo-Json -Depth 10
    Write-UpdateFixture $baseManifestPath $baseJson
    Write-UpdateFixture (Join-Path $baseRoot 'cache/user-data/keep.txt') 'must remain only in base'
    $baseHash = (Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash
    $parameters = @{ BasePackagePath = $baseRoot; BuildDirectory = $buildRoot; Runtime = 'nvidia'
        RepositoryRoot = $sourceRoot; GitExecutable = $git; OutputDirectory = (Join-Path $runRoot 'updated') }
    & $publisher @parameters
    Assert-ProductionManifest $parameters.OutputDirectory
    $published = Get-Content -LiteralPath (Join-Path $parameters.OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
    $evidenceRelative = 'tools/acceptance/WORKER-UPDATE.json'
    $evidence = Get-Content -LiteralPath (Join-Path $parameters.OutputDirectory $evidenceRelative) -Raw | ConvertFrom-Json
    Assert-UpdateTest ($published.git_commit -ceq $commit) '候选提交绑定新 Worker'
    Assert-UpdateTest ($evidence.base_package.git_commit -ceq ('a' * 40)) '保留基包提交'
    Assert-UpdateTest ($evidence.base_package.manifest_sha256 -ieq $baseHash) '保留基包清单身份'
    Assert-UpdateTest (-not $evidence.inherited_payload_hashes_verified) '不虚称继承文件已全量哈希'
    Assert-UpdateTest (-not (Test-Path -LiteralPath (Join-Path $parameters.OutputDirectory 'cache/user-data/keep.txt'))) '不复制用户可变数据'
    Assert-UpdateTest ((Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash -ceq $baseHash) '基包不变'
    foreach ($record in $published.files) {
        if ($record.path -notin @('runtimes/nvidia/Xen.exe', $evidenceRelative)) {
            $oldRecord = @($records | Where-Object { $_.path -ceq $record.path })[0]
            Assert-UpdateTest (($record | ConvertTo-Json -Compress) -ceq ($oldRecord | ConvertTo-Json -Compress)) "继承记录原样保留 $($record.path)"
        }
    }
    $launcherParameters = $parameters.Clone()
    $launcherParameters.IncludeLauncher = $true
    $launcherParameters.OutputDirectory = Join-Path $runRoot 'launcher-update'
    & $publisher @launcherParameters
    Assert-ProductionManifest $launcherParameters.OutputDirectory
    Assert-UpdateTest ((Get-Content -LiteralPath (Join-Path $launcherParameters.OutputDirectory 'XenLauncher.exe') -Raw) -ceq 'new-launcher-with-current-config') '配置消费者启动器随Worker更新'
    $existingRejected = $false
    try { & $publisher @parameters } catch { $existingRejected = $true }
    Assert-UpdateTest $existingRejected '已有正式目标拒绝覆盖'
    $repeatParameters = $parameters.Clone()
    $repeatParameters.BasePackagePath = $parameters.OutputDirectory
    $repeatParameters.OutputDirectory = Join-Path $runRoot 'repeated-update'
    & $publisher @repeatParameters
    Assert-ProductionManifest $repeatParameters.OutputDirectory
    $repeatManifest = Get-Content -LiteralPath (Join-Path $repeatParameters.OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
    Assert-UpdateTest (@($repeatManifest.files | Where-Object { $_.path -ceq $evidenceRelative }).Count -eq 1) '重复更新来源载荷无重复记录'
    $legacy = $baseJson | ConvertFrom-Json
    $legacy | Add-Member NoteProperty worker_update $evidence
    Write-UpdateFixture $baseManifestPath ($legacy | ConvertTo-Json -Depth 20)
    $legacyParameters = $parameters.Clone()
    $legacyParameters.OutputDirectory = Join-Path $runRoot 'legacy-recovered'
    & $publisher @legacyParameters
    Assert-ProductionManifest $legacyParameters.OutputDirectory
    Write-UpdateFixture $baseManifestPath $baseJson
    $parameters.OutputDirectory = Join-Path $runRoot 'dirty'
    Write-UpdateFixture (Join-Path $sourceRoot 'untracked.txt') 'dirty'
    Assert-UpdateReject $parameters '脏源码拒绝'
    Remove-Item -LiteralPath (Join-Path $sourceRoot 'untracked.txt')
    $identity.git_dirty = $true
    Write-UpdateFixture $identityPath ($identity | ConvertTo-Json)
    Assert-UpdateReject $parameters '脏构建身份拒绝'
    $identity.git_dirty = $false
    $identity.source_root = $runRoot
    Write-UpdateFixture $identityPath ($identity | ConvertTo-Json)
    Assert-UpdateReject $parameters '错误构建源码根拒绝'
    $identity.source_root = $sourceRoot
    Write-UpdateFixture $identityPath ($identity | ConvertTo-Json)
    $unknownField = $baseJson | ConvertFrom-Json
    $unknownField | Add-Member NoteProperty unrelated_metadata @{}
    Write-UpdateFixture $baseManifestPath ($unknownField | ConvertTo-Json -Depth 20)
    Assert-UpdateReject $parameters '非 worker_update 的额外顶层字段拒绝'
    $unknownField | Add-Member NoteProperty worker_update $evidence
    Write-UpdateFixture $baseManifestPath ($unknownField | ConvertTo-Json -Depth 20)
    Assert-UpdateReject $parameters '超过六个顶层字段拒绝'
    foreach ($unsafe in @('../outside.txt', 'runtimes/nvidia/Xen.exe:stream', 'cache/CON', 'cache/trailing.', 'cache//empty')) {
        $bad = $baseJson | ConvertFrom-Json
        $bad.files[0].path = $unsafe
        Write-UpdateFixture $baseManifestPath ($bad | ConvertTo-Json -Depth 10)
        Assert-UpdateReject $parameters "不安全清单路径 $unsafe"
    }
    $duplicate = $baseJson | ConvertFrom-Json
    $duplicate.files += $duplicate.files[0]
    Write-UpdateFixture $baseManifestPath ($duplicate | ConvertTo-Json -Depth 10)
    Assert-UpdateReject $parameters '重复清单路径拒绝'
    Write-UpdateFixture $baseManifestPath $baseJson
    $parameters.OutputDirectory = Join-Path $baseRoot 'nested'
    Assert-UpdateReject $parameters '目标嵌入基包拒绝'
    $parameters.OutputDirectory = Join-Path $runRoot 'config-update'
    $config = Join-Path $runRoot 'replacement.ini'
    Write-UpdateFixture $config 'new non-secret fixture config'
    $parameters.ConfigPath = $config
    $notes = Join-Path $runRoot 'new-notes.md'
    $manual = Join-Path $runRoot 'new-manual.md'
    Write-UpdateFixture $notes 'new package notes'
    Write-UpdateFixture $manual 'new manual acceptance'
    $parameters.PackageNotesPath = $notes
    $parameters.ManualAcceptancePath = $manual
    & $publisher @parameters
    Assert-ProductionManifest $parameters.OutputDirectory
    Assert-UpdateTest ((Get-FileHash -LiteralPath (Join-Path $parameters.OutputDirectory 'config.ini')).Hash -ceq (Get-FileHash -LiteralPath $config).Hash) '显式配置替换'
    foreach ($pair in @(@('tools/acceptance/PACKAGE-NOTES.md', $notes), @('tools/acceptance/MANUAL-ACCEPTANCE.md', $manual))) {
        $copyHash = (Get-FileHash -LiteralPath (Join-Path $parameters.OutputDirectory $pair[0])).Hash
        Assert-UpdateTest ($copyHash -ceq (Get-FileHash -LiteralPath $pair[1]).Hash) "显式说明文档替换 $($pair[0])"
        $updatedManifest = Get-Content -LiteralPath (Join-Path $parameters.OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
        $updatedRecord = @($updatedManifest.files | Where-Object { $_.path -ceq $pair[0] })[0]
        Assert-UpdateTest ($updatedRecord.sha256 -ieq $copyHash) "说明文档 manifest SHA $($pair[0])"
    }
    Assert-UpdateTest (@(Get-ChildItem -LiteralPath $runRoot -Directory -Filter '.incoming-*').Count -eq 0) '无临时目录残留'
    # 仅在小型本地夹具验证差量；不连接 SSH、不启动或终止任何程序。
    $deltaName = ".worker-delta-$([guid]::NewGuid().ToString('N'))"
    $deltaOutput = Join-Path $runRoot $deltaName
    $deltaParameters = $parameters.Clone()
    foreach ($key in @('ConfigPath', 'PackageNotesPath', 'ManualAcceptancePath')) { $deltaParameters.Remove($key) }
    $deltaParameters.OutputDirectory = $deltaOutput
    $deltaParameters.ChangesOnly = $true
    $deltaParameters.SourceContextExecutable = Join-Path $buildRoot 'Release\xen_source_context.exe'
    $invalidTool = $deltaParameters.Clone()
    $invalidTool.OutputDirectory = Join-Path $runRoot 'wrong-source-tool'
    $invalidTool.SourceContextExecutable = Join-Path $buildRoot 'Release\Xen.exe'
    Assert-UpdateReject $invalidTool '拒绝任意源工具文件映射'
    Write-UpdateFixture (Join-Path $baseRoot 'config.ini') 'user changed configuration after original publication'
    Write-UpdateFixture (Join-Path $baseRoot 'cache/model-workspace/settings.json') '{"user_changed":true}'
    & $publisher @deltaParameters
    Assert-UpdateTest (@(Get-ChildItem -LiteralPath $deltaOutput -Recurse -File).Count -eq 4) '差量只生成 Worker、选中桥接工具、来源证据和清单'
    Assert-UpdateTest (-not (Test-Path -LiteralPath (Join-Path $deltaOutput 'config.ini'))) '差量不复制配置'
    $deltaStage = Join-Path $baseRoot $deltaName
    [IO.Directory]::Move($deltaOutput, $deltaStage)
    $deltaEntries = @()
    foreach ($relative in @('runtimes/nvidia/Xen.exe', 'tools/source/xen_source_context.exe', 'tools/acceptance/WORKER-UPDATE.json', 'manifest.json')) {
        $oldPath = Join-Path $baseRoot $relative
        $oldHash = if (Test-Path -LiteralPath $oldPath) { (Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant() } else { '' }
        $deltaEntries += [ordered]@{ path = $relative; old_sha256 = $oldHash
            new_sha256 = (Get-FileHash -LiteralPath (Join-Path $deltaStage $relative)).Hash.ToLowerInvariant() }
    }
    $protected = @()
    foreach ($relative in @('config.ini', 'cache/model-workspace/settings.json')) {
        $protected += [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath (Join-Path $baseRoot $relative)).Hash.ToLowerInvariant() }
    }
    [ordered]@{ schema = 1; runtime = 'nvidia'; files = $deltaEntries; protected_files = $protected } |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $deltaStage 'delta.json') -Encoding UTF8
    $apply = Join-Path $PSScriptRoot 'apply_worker_delta.ps1'
    function Get-Process { param($Name, $ErrorAction); [pscustomobject]@{ Path = $null } }
    $runningRejected = $false
    try { & $apply -PackageRoot $baseRoot -StageName $deltaName -CheckOnly } catch {
        $runningRejected = $_.Exception.Message -match 'XEN_WORKER_RUNNING'
    }
    Assert-UpdateTest $runningRejected '路径不可读的 Xen 进程保守拒绝'
    function Get-Process { param($Name, $ErrorAction); return @() }
    $lock = [IO.File]::Open((Join-Path $baseRoot 'runtimes/nvidia/Xen.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $lockedRejected = $false
    try { & $apply -PackageRoot $baseRoot -StageName $deltaName -CheckOnly } catch { $lockedRejected = $true }
    finally { $lock.Dispose() }
    Assert-UpdateTest $lockedRejected '已锁定 Worker 拒绝替换'
    Assert-UpdateTest ((Get-FileHash -LiteralPath $baseManifestPath).Hash -ceq $baseHash) '拒绝时旧清单不变'
    $savedDelta = @{}
    foreach ($entry in $deltaEntries) {
        $savedDelta[$entry.path] = [IO.File]::ReadAllBytes((Join-Path $deltaStage $entry.path))
    }
    $manifestLock = [IO.File]::Open($baseManifestPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $rollbackObserved = $false
    try { & $apply -PackageRoot $baseRoot -StageName $deltaName } catch { $rollbackObserved = $true }
    finally { $manifestLock.Dispose() }
    Assert-UpdateTest $rollbackObserved 'manifest 最后替换失败可被观察'
    foreach ($entry in $deltaEntries) {
        $target = Join-Path $baseRoot $entry.path
        $actual = if (Test-Path -LiteralPath $target) { (Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() } else { '' }
        Assert-UpdateTest ($actual -ceq $entry.old_sha256) "失败后变化文件恢复旧身份 $($entry.path)"
        [IO.File]::WriteAllBytes((Join-Path $deltaStage $entry.path), $savedDelta[$entry.path])
    }
    & $apply -PackageRoot $baseRoot -StageName $deltaName -CheckOnly
    & $apply -PackageRoot $baseRoot -StageName $deltaName
    Remove-Item Function:Get-Process
    Assert-ProductionManifest $baseRoot -MutableFilesMayDiffer
    Assert-UpdateTest ((Get-Content -LiteralPath (Join-Path $baseRoot 'tools/source/xen_source_context.exe') -Raw) -ceq 'updated-source-context-fixture') '选中桥接工具已更新'
    $deltaEvidence = Get-Content -LiteralPath (Join-Path $baseRoot 'tools/acceptance/WORKER-UPDATE.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $toolIdentity = @($deltaEvidence.updated_components | Where-Object { $_.path -ceq 'tools/source/xen_source_context.exe' })
    Assert-UpdateTest ($toolIdentity.Count -eq 1 -and $toolIdentity[0].git_commit -ceq $commit) '桥接工具绑定同提交身份'
    foreach ($entry in $protected) {
        Assert-UpdateTest ((Get-FileHash -LiteralPath (Join-Path $baseRoot $entry.path)).Hash.ToLowerInvariant() -ceq $entry.sha256) '差量后用户配置原字节保留'
    }
    foreach ($runtime in @('directml', 'openvino')) {
        Assert-UpdateTest ((Get-Content -LiteralPath (Join-Path $baseRoot "runtimes/$runtime/Xen.exe") -Raw) -ceq "old-$runtime-worker") '其他 Worker 不变'
    }
    Write-Host "PASS: $script:passed 项单 Worker 继承发布回归。"
} finally {
    Remove-XenOwnedTestDirectory -RootPath $runRoot -BasePath $ownedTest.BasePath `
        -OwnerId $ownedTest.OwnerId -RepositoryRoot (Split-Path -Parent $PSScriptRoot)
}
