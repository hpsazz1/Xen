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

$sourceScripts = Split-Path -Parent ([IO.Path]::GetFullPath($PublishScript))
$sourceRepository = Split-Path -Parent $sourceScripts
Import-Module (Join-Path $sourceScripts "path_safety.psm1") -Force
$owned = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $sourceRepository
$resolvedTestRoot = $owned.RootPath

try {

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
        "prepare_mouse_effect_probe_b_command_magnitude.ps1",
        "prepare_mouse_effect_probe_b_composite_phase.ps1",
        "launch_mouse_effect_probe_a.ps1",
        "design_mouse_effect_probe_prbs.py",
        "analyze_mouse_effect_probe_b.py",
        "analyze_mouse_effect_probe_b_holdout.py",
        "analyze_mouse_effect_probe_b_command_magnitude.py",
        "freeze_mouse_effect_probe_b_composite_phase_plan.py",
        "produce_mouse_effect_probe_b_composite_phase_ledgers.py",
        "bind_mouse_effect_probe_b_composite_phase_calibration.py",
        "evaluate_mouse_effect_probe_b_composite_phase.py")) {
    Write-Utf8NoBom (Join-Path $scripts $name) "fixture:$name`n"
}
foreach ($name in @(
        "XenMouseEffectProbe.exe",
        "XenCaptureEvidence.exe",
        "XenMouseEffectProbeSequence.exe",
        "XenMouseEffectProbeCompositeSeal.exe",
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

foreach ($dirtyCase in @(
        @{ Name = "true"; Value = $true },
        @{ Name = "null"; Value = $null },
        @{ Name = "zero"; Value = 0 },
        @{ Name = "string-false"; Value = "false" },
        @{ Name = "empty-string"; Value = "" },
        @{ Name = "missing" })) {
    if ($dirtyCase.ContainsKey("Value")) {
        $identity.git_dirty = $dirtyCase.Value
    } else {
        $identity.Remove("git_dirty")
    }
    Write-Utf8NoBom (Join-Path $build "xen-build-identity.json") `
        (($identity | ConvertTo-Json -Depth 8) + "`n")
    $dirtyIdentityOutput = Join-Path $resolvedTestRoot (
        "dirty-identity-" + $dirtyCase.Name)
    $dirtyIdentityRejected = $false
    try {
        & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
            -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
            -PackageOutputRoot $dirtyIdentityOutput -SkipRemotePublish
    } catch {
        $dirtyIdentityRejected = $_.Exception.Message -like "*构建身份*"
    }
    Assert-True ($dirtyIdentityRejected -and
                 -not (Test-Path -LiteralPath $dirtyIdentityOutput)) `
        "非法构建身份必须在任何发布目录写入前拒绝：$($dirtyCase.Name)"
}
$identity.git_dirty = $false
Write-Utf8NoBom (Join-Path $build "xen-build-identity.json") `
    (($identity | ConvertTo-Json -Depth 8) + "`n")

$junctionDestination = Join-Path $resolvedTestRoot "junction-destination"
$junctionOutside = Join-Path $resolvedTestRoot "junction-outside"
$junctionOutput = Join-Path $resolvedTestRoot "junction-packages"
[void](New-Item -ItemType Directory -Path $junctionDestination)
[void](New-Item -ItemType Directory -Path $junctionOutside)
$junctionSentinel = Join-Path $junctionOutside "keep.txt"
Write-Utf8NoBom $junctionSentinel "保留声明目的根外的原始内容"
$junctionSentinelHash = (Get-FileHash -LiteralPath $junctionSentinel).Hash
$junctionPath = Join-Path $junctionDestination "releases"
[void](New-Item -ItemType Junction -Path $junctionPath -Target $junctionOutside)
try {
    $junctionRejected = $false
    try {
        & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
            -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
            -PackageOutputRoot $junctionOutput `
            -DestinationRoot $junctionDestination
    } catch {
        $junctionRejected = $_.Exception.Message -like "*reparse*"
    }
    Assert-True ($junctionRejected -and
                 -not (Test-Path -LiteralPath $junctionOutput) -and
                 @(Get-ChildItem -LiteralPath $junctionOutside -Force).Count -eq 1 -and
                 (Get-FileHash -LiteralPath $junctionSentinel).Hash -eq $junctionSentinelHash) `
        "目的releases祖先junction必须在任何发布写入前拒绝且保留外部哨兵"
} finally {
    # 只移除本轮明确创建的junction入口，不递归访问其目标。
    [IO.Directory]::Delete($junctionPath, $false)
}

$packageName = "MouseEffectProbe-B-$($commit.Substring(0, 7))"
foreach ($pathCase in @("local-ancestor", "destination-root",
        "destination-ancestor", "remote-incoming", "remote-final")) {
    $caseRoot = Join-Path $resolvedTestRoot $pathCase
    $outsideRoot = Join-Path $caseRoot "outside"
    $caseDestination = Join-Path $caseRoot "destination"
    $caseOutput = Join-Path $caseRoot "packages"
    [void][IO.Directory]::CreateDirectory($outsideRoot)
    [void][IO.Directory]::CreateDirectory((Join-Path $caseDestination "releases"))
    $sentinel = Join-Path $outsideRoot "keep.bin"
    [IO.File]::WriteAllBytes($sentinel, [byte[]](0, 255, 12, 43))
    $link = Join-Path $caseRoot "link"
    switch ($pathCase) {
        "local-ancestor" { $caseOutput = Join-Path $link "not-created" }
        "destination-root" { $caseDestination = $link }
        "destination-ancestor" {
            [void][IO.Directory]::CreateDirectory((Join-Path $outsideRoot "child"))
            $caseDestination = Join-Path $link "child"
        }
        "remote-incoming" {
            $link = Join-Path (Join-Path $caseDestination "releases") ".incoming-$packageName"
        }
        "remote-final" {
            $link = Join-Path (Join-Path $caseDestination "releases") $packageName
        }
    }
    $beforeCount = @(Get-ChildItem -LiteralPath $outsideRoot -Force -Recurse).Count
    [void](New-Item -ItemType Junction -Path $link -Target $outsideRoot)
    try {
        $pathRejected = $false
        try {
            & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
                -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
                -PackageOutputRoot $caseOutput -DestinationRoot $caseDestination
        } catch {
            $pathRejected = $_.Exception.Message -like "*reparse*"
        }
        Assert-True ($pathRejected -and -not (Test-Path -LiteralPath $caseOutput) -and
                     @(Get-ChildItem -LiteralPath $outsideRoot -Force -Recurse).Count -eq $beforeCount -and
                     [Convert]::ToBase64String([IO.File]::ReadAllBytes($sentinel)) -ceq "AP8MKw==") `
            "非法发布路径必须在写入前拒绝并保留原bytes：$pathCase"
    } finally {
        [IO.Directory]::Delete($link, $false)
    }
}

# 第二个payload的共享锁让实际File.Copy失败，验证已取得的incoming被清理。
$failedCopyRoot = Join-Path $resolvedTestRoot "failed-copy-packages"
$lockedSource = [IO.File]::Open((Join-Path $release "XenCaptureEvidence.exe"),
    [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try {
    $copyFailed = $false
    try {
        & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
            -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
            -PackageOutputRoot $failedCopyRoot -SkipRemotePublish
    } catch { $copyFailed = $true }
    Assert-True ($copyFailed -and (Test-Path -LiteralPath $failedCopyRoot -PathType Container) -and
                 @(Get-ChildItem -LiteralPath $failedCopyRoot -Force).Count -eq 0) `
        "实际复制失败必须清理本轮incoming且不发布正式包"
} finally {
    $lockedSource.Dispose()
}

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
             [bool]$manifest.command_magnitude_primary_tooling_included -and
             -not [bool]$manifest.command_magnitude_holdout_prepare_included -and
             -not [bool]$manifest.command_magnitude_run_included -and
             [bool]$manifest.composite_phase_tooling_included -and
             -not [bool]$manifest.composite_phase_run_included -and
             [int]$manifest.file_count -eq 21 -and
             @($manifest.files).Count -eq 21) `
    "Physical B manifest 身份、clean/Launch 边界或文件数错误"
$manifestNames = @($manifest.files | ForEach-Object { [string]$_.name })
foreach ($requiredName in @(
        "prepare_mouse_effect_probe_b_holdout.ps1",
        "analyze_mouse_effect_probe_b_holdout.py",
        "prepare_mouse_effect_probe_b_command_magnitude.ps1",
        "analyze_mouse_effect_probe_b_command_magnitude.py",
        "prepare_mouse_effect_probe_b_composite_phase.ps1",
        "XenMouseEffectProbeCompositeSeal.exe",
        "freeze_mouse_effect_probe_b_composite_phase_plan.py",
        "produce_mouse_effect_probe_b_composite_phase_ledgers.py",
        "bind_mouse_effect_probe_b_composite_phase_calibration.py",
        "evaluate_mouse_effect_probe_b_composite_phase.py")) {
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

$localBefore = (Get-FileHash -LiteralPath $localManifestPath).Hash
$localOverwriteRejected = $false
try {
    & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
        -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
        -PackageOutputRoot $packages -SkipRemotePublish
} catch { $localOverwriteRejected = $_.Exception.Message -like "*已存在*" }
Assert-True ($localOverwriteRejected -and
             (Get-FileHash -LiteralPath $localManifestPath).Hash -eq $localBefore) `
    "仅本地发布也必须拒绝覆盖并保留既有manifest"
$remoteBefore = (Get-FileHash -LiteralPath $remoteManifestPath).Hash
$remoteOnlyOutput = Join-Path $resolvedTestRoot "remote-only-packages"
$remoteOverwriteRejected = $false
try {
    & $PublishScript -BuildDirectory $build -RepositoryRoot $repository `
        -SourceScriptRoot $scripts -GitExecutable $GitExecutable `
        -PackageOutputRoot $remoteOnlyOutput -DestinationRoot $destination
} catch { $remoteOverwriteRejected = $_.Exception.Message -like "*已存在*" }
Assert-True ($remoteOverwriteRejected -and
             -not (Test-Path -LiteralPath $remoteOnlyOutput) -and
             (Get-FileHash -LiteralPath $remoteManifestPath).Hash -eq $remoteBefore) `
    "仅辅机模拟包已存在时须在本地写入前拒绝并保留既有manifest"

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
} finally {
    Remove-XenOwnedTestDirectory -RootPath $owned.RootPath `
        -BasePath $owned.BasePath -RepositoryRoot $sourceRepository `
        -OwnerId $owned.OwnerId
}
