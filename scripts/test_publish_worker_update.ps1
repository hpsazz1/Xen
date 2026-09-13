param([string]$TestRoot = (Join-Path $PSScriptRoot '..\cache\worker-update-tests'))
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
try {
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
    $identityPath = Join-Path $buildRoot 'xen-build-identity.json'
    $identity = [ordered]@{ schema = 1; source_root = $sourceRoot; git_commit = $commit; git_dirty = $false; runtime = 'nvidia' }
    Write-UpdateFixture $identityPath ($identity | ConvertTo-Json)
    $baseRoot = Join-Path $runRoot 'base'
    $records = @()
    $routes = @()
    foreach ($runtime in @('nvidia', 'directml', 'openvino')) {
        $relative = "runtimes/$runtime/Xen.exe"
        Write-UpdateFixture (Join-Path $baseRoot $relative) "old-$runtime-worker"
        $routes += [ordered]@{ id = $runtime; executable = $relative; backends = @($runtime) }
    }
    Write-UpdateFixture (Join-Path $baseRoot 'config.ini') '[fixture]'
    Write-UpdateFixture (Join-Path $baseRoot 'cache/model-workspace/settings.json') '{}'
    Write-UpdateFixture (Join-Path $baseRoot 'tools/acceptance/PACKAGE-NOTES.md') 'old package notes'
    Write-UpdateFixture (Join-Path $baseRoot 'tools/acceptance/MANUAL-ACCEPTANCE.md') 'old manual acceptance'
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
    $published = Get-Content -LiteralPath (Join-Path $parameters.OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
    Assert-UpdateTest ($published.git_commit -ceq $commit) '候选提交绑定新 Worker'
    Assert-UpdateTest ($published.worker_update.base_package.git_commit -ceq ('a' * 40)) '保留基包提交'
    Assert-UpdateTest ($published.worker_update.base_package.manifest_sha256 -ieq $baseHash) '保留基包清单身份'
    Assert-UpdateTest (-not $published.worker_update.inherited_payload_hashes_verified) '不虚称继承文件已全量哈希'
    Assert-UpdateTest (-not (Test-Path -LiteralPath (Join-Path $parameters.OutputDirectory 'cache/user-data/keep.txt'))) '不复制用户可变数据'
    Assert-UpdateTest ((Get-FileHash -LiteralPath $baseManifestPath -Algorithm SHA256).Hash -ceq $baseHash) '基包不变'
    foreach ($record in $published.files) {
        $actualHash = (Get-FileHash -LiteralPath (Join-Path $parameters.OutputDirectory $record.path) -Algorithm SHA256).Hash
        Assert-UpdateTest ($actualHash -ieq $record.sha256) "最终清单哈希 $($record.path)"
        if ($record.path -ne 'runtimes/nvidia/Xen.exe') {
            $oldRecord = @($records | Where-Object { $_.path -ceq $record.path })[0]
            Assert-UpdateTest (($record | ConvertTo-Json -Compress) -ceq ($oldRecord | ConvertTo-Json -Compress)) "继承记录原样保留 $($record.path)"
        }
    }
    $existingRejected = $false
    try { & $publisher @parameters } catch { $existingRejected = $true }
    Assert-UpdateTest $existingRejected '已有正式目标拒绝覆盖'
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
    Assert-UpdateTest ((Get-FileHash -LiteralPath (Join-Path $parameters.OutputDirectory 'config.ini')).Hash -ceq (Get-FileHash -LiteralPath $config).Hash) '显式配置替换'
    foreach ($pair in @(@('tools/acceptance/PACKAGE-NOTES.md', $notes), @('tools/acceptance/MANUAL-ACCEPTANCE.md', $manual))) {
        $copyHash = (Get-FileHash -LiteralPath (Join-Path $parameters.OutputDirectory $pair[0])).Hash
        Assert-UpdateTest ($copyHash -ceq (Get-FileHash -LiteralPath $pair[1]).Hash) "显式说明文档替换 $($pair[0])"
        $updatedManifest = Get-Content -LiteralPath (Join-Path $parameters.OutputDirectory 'manifest.json') -Raw | ConvertFrom-Json
        $updatedRecord = @($updatedManifest.files | Where-Object { $_.path -ceq $pair[0] })[0]
        Assert-UpdateTest ($updatedRecord.sha256 -ieq $copyHash) "说明文档 manifest SHA $($pair[0])"
    }
    Assert-UpdateTest (@(Get-ChildItem -LiteralPath $runRoot -Directory -Filter '.incoming-*').Count -eq 0) '无临时目录残留'
    Write-Host "PASS: $script:passed 项单 Worker 继承发布回归。"
} finally {
    Remove-XenOwnedTestDirectory -RootPath $runRoot -BasePath $ownedTest.BasePath `
        -OwnerId $ownedTest.OwnerId -RepositoryRoot (Split-Path -Parent $PSScriptRoot)
}
