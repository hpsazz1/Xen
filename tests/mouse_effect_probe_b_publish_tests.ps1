param(
    [Parameter(Mandatory = $true)]
    [string]$PublishScript,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot,
    [string]$GitExecutable = "git"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Invoke-Git([string]$Repository, [string[]]$Arguments) {
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $GitExecutable -C $Repository @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($exitCode -ne 0) {
        throw "测试 Git 命令失败：$($Arguments -join ' ')；$($output -join ' ')"
    }
    return @($output)
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

if (-not (Test-Path -LiteralPath $PublishScript -PathType Leaf)) {
    throw "Physical B 发布脚本不存在：$PublishScript"
}

$resolvedTestRoot = [IO.Path]::GetFullPath($TestRoot)
$testParent = Split-Path -Parent $resolvedTestRoot
$testLeaf = Split-Path -Leaf $resolvedTestRoot
if ([string]::IsNullOrWhiteSpace($testParent) -or
    $testLeaf -notlike "mouse-effect-probe-b-publish-tests*") {
    throw "测试根目录不符合专用前缀：$resolvedTestRoot"
}
if (Test-Path -LiteralPath $resolvedTestRoot) {
    $existing = (Resolve-Path -LiteralPath $resolvedTestRoot).ProviderPath
    if (-not [string]::Equals(
            $existing, $resolvedTestRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "测试清理路径发生变化：$existing"
    }
    Remove-Item -LiteralPath $existing -Recurse -Force
}
[void](New-Item -ItemType Directory -Path $resolvedTestRoot)

$repository = Join-Path $resolvedTestRoot "repository"
$scripts = Join-Path $repository "scripts"
$build = Join-Path $resolvedTestRoot "build"
$release = Join-Path $build "Release"
$packages = Join-Path $resolvedTestRoot "packages"
$destination = Join-Path $resolvedTestRoot "remote"
[void](New-Item -ItemType Directory -Path $scripts -Force)
[void](New-Item -ItemType Directory -Path $release -Force)
[void](New-Item -ItemType Directory -Path (Join-Path $destination "releases") -Force)

foreach ($name in @(
        "prepare_mouse_effect_probe_b.ps1",
        "prepare_mouse_effect_probe_b_holdout.ps1",
        "launch_mouse_effect_probe_a.ps1",
        "design_mouse_effect_probe_prbs.py",
        "analyze_mouse_effect_probe_b.py",
        "analyze_mouse_effect_probe_b_holdout.py")) {
    Write-Utf8NoBom (Join-Path $scripts $name) "fixture:$name`n"
}
foreach ($name in @(
        "XenMouseEffectProbe.exe",
        "XenCaptureEvidence.exe",
        "XenMouseEffectProbeSequence.exe",
        "opencv_world4140.dll",
        "Processing.NDI.Lib.x64.dll",
        "Processing.NDI.Lib.Licenses.txt")) {
    Write-Utf8NoBom (Join-Path $release $name) "fixture:$name`n"
}

$previousErrorAction = $ErrorActionPreference
try {
    $ErrorActionPreference = "Continue"
    & $GitExecutable init $repository 2>&1 | Out-Null
    $gitInitExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorAction
}
if ($gitInitExitCode -ne 0) { throw "测试仓库 git init 失败" }
[void](Invoke-Git $repository @("config", "user.name", "Xen Test"))
[void](Invoke-Git $repository @("config", "user.email", "xen-test@example.invalid"))
[void](Invoke-Git $repository @("add", "scripts"))
[void](Invoke-Git $repository @("commit", "-m", "测试 Physical B 发布"))
$commitOutput = @(Invoke-Git $repository @("rev-parse", "HEAD"))
$commit = ([string]$commitOutput[0]).Trim()
$identity = [ordered]@{
    schema = 1
    source_root = $repository.Replace('\', '/')
    git_commit = $commit
    git_dirty = $false
    runtime = "nvidia"
    components = @("ndi", "opencv")
}
Write-Utf8NoBom (Join-Path $build "xen-build-identity.json") `
    (($identity | ConvertTo-Json -Depth 8) + "`n")

& $PublishScript `
    -BuildDirectory $build `
    -RepositoryRoot $repository `
    -SourceScriptRoot $scripts `
    -GitExecutable $GitExecutable `
    -PackageOutputRoot $packages `
    -DestinationRoot $destination
if ($LASTEXITCODE -ne 0) {
    throw "Physical B 发布集成测试失败，ExitCode=$LASTEXITCODE"
}

$packageName = "MouseEffectProbe-B-$($commit.Substring(0, 7))"
$localPackage = Join-Path $packages $packageName
$remotePackage = Join-Path (Join-Path $destination "releases") $packageName
$localManifestPath = Join-Path $localPackage "manifest.json"
$remoteManifestPath = Join-Path $remotePackage "manifest.json"
Assert-True ((Test-Path -LiteralPath $localManifestPath -PathType Leaf) -and
             (Test-Path -LiteralPath $remoteManifestPath -PathType Leaf)) `
    "本地与辅机模拟目录必须同时原子发布 manifest"
$manifest = Get-Content -LiteralPath $localManifestPath -Raw -Encoding utf8 |
    ConvertFrom-Json
Assert-True ([int]$manifest.schema_version -eq 1 -and
             [string]$manifest.evidence_type -eq
                "mouse_effect_probe_b_tool_package" -and
             [string]$manifest.package_name -eq $packageName -and
             [string]$manifest.git_commit -eq $commit -and
             [bool]$manifest.source_tracked_clean -and
             -not [bool]$manifest.physical_run_included -and
             -not [bool]$manifest.physical_launch_executed -and
             [bool]$manifest.cross_run_holdout_tooling_included -and
             -not [bool]$manifest.cross_run_holdout_included -and
             [int]$manifest.file_count -eq 13 -and
             @($manifest.files).Count -eq 13) `
    "Physical B manifest 身份、clean/Launch 边界或文件数错误"
$manifestNames = @($manifest.files | ForEach-Object { [string]$_.name })
foreach ($requiredName in @(
        "prepare_mouse_effect_probe_b_holdout.ps1",
        "analyze_mouse_effect_probe_b_holdout.py")) {
    Assert-True ($manifestNames -contains $requiredName) `
        "Physical B 发布包缺少 Holdout 工具：$requiredName"
}
Assert-True ((Get-FileHash -LiteralPath $localManifestPath -Algorithm SHA256).Hash -eq
             (Get-FileHash -LiteralPath $remoteManifestPath -Algorithm SHA256).Hash) `
    "本地与辅机模拟 manifest SHA-256 必须一致"
Assert-True (@(Get-ChildItem -LiteralPath $packages -Directory |
        Where-Object { $_.Name -like ".incoming-*" }).Count -eq 0 -and
             @(Get-ChildItem -LiteralPath (Join-Path $destination "releases") -Directory |
        Where-Object { $_.Name -like ".incoming-*" }).Count -eq 0) `
    "发布成功后不得残留 incoming"

$overwriteRejected = $false
try {
    & $PublishScript `
        -BuildDirectory $build `
        -RepositoryRoot $repository `
        -SourceScriptRoot $scripts `
        -GitExecutable $GitExecutable `
        -PackageOutputRoot $packages `
        -DestinationRoot $destination
} catch {
    $overwriteRejected = $_.Exception.Message -like "*已存在*"
}
Assert-True $overwriteRejected "既有本地/辅机包必须拒绝覆盖"

Write-Utf8NoBom (Join-Path $scripts "prepare_mouse_effect_probe_b.ps1") `
    "tracked dirty`n"
$dirtyRejected = $false
try {
    & $PublishScript `
        -BuildDirectory $build `
        -RepositoryRoot $repository `
        -SourceScriptRoot $scripts `
        -GitExecutable $GitExecutable `
        -PackageOutputRoot (Join-Path $resolvedTestRoot "dirty-packages") `
        -DestinationRoot $destination `
        -SkipRemotePublish
} catch {
    $dirtyRejected = $_.Exception.Message -like "*可跟踪差异*"
}
Assert-True $dirtyRejected "存在可跟踪差异时必须在复制前 fail closed"

Write-Host "Mouse Effect Probe Physical B 原子发布测试全部通过。"
