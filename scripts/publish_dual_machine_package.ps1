param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot `
        "..\build-matrix-final-ndi"),
    [string]$ModelPath = (Join-Path $PSScriptRoot `
        "..\build-matrix-final-cpu\Release\models\14wv11.onnx"),
    [ValidateSet("Release")]
    [string]$Configuration = "Release",
    [string]$PackageOutputRoot = (Join-Path $PSScriptRoot `
        "..\cache\dual-machine-packages"),
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$",
    [string]$PackageId = "",
    [switch]$SkipBuild,
    [switch]$SkipPublish
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description,
        [ValidateSet("Container", "Leaf")][string]$PathType
    )
    if (-not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Get-FileEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        relative_path = ""
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
    }
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $prefix = $rootPath + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "文件不在包目录内：$fullPath"
    }
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Assert-PublishedPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][object]$Manifest
    )
    foreach ($record in @($Manifest.files)) {
        $path = Join-Path $PackageRoot ([string]$record.relative_path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "发布目标缺少文件：$path"
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ([long]$file.Length -ne [long]$record.length -or
            $hash -ne [string]$record.sha256) {
            throw "发布目标文件长度或 SHA-256 不一致：$path"
        }
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$BuildDirectory = Resolve-RequiredPath `
    $BuildDirectory "NDI 构建目录" "Container"
$ModelPath = Resolve-RequiredPath $ModelPath "固定测试模型" "Leaf"
$PackageOutputRoot = [System.IO.Path]::GetFullPath($PackageOutputRoot)

$gitCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $gitCommit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "无法读取有效 Git 提交。"
}
$gitStatus = @(& git -C $repositoryRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw "读取 Git 工作树状态失败。" }
if ($gitStatus.Count -ne 0) {
    throw "正式双机包只允许从干净工作树生成：$($gitStatus -join '; ')"
}

if (-not $SkipBuild) {
    & cmake --build $BuildDirectory --config $Configuration `
        --target xen_benchmark --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "XenBenchmark Release 构建失败，退出码：$LASTEXITCODE"
    }
}

$outputDirectory = Resolve-RequiredPath `
    (Join-Path $BuildDirectory $Configuration) `
    "Release 输出目录" "Container"
$executable = Resolve-RequiredPath `
    (Join-Path $outputDirectory "XenBenchmark.exe") `
    "XenBenchmark" "Leaf"
$deploymentPath = Resolve-RequiredPath `
    (Join-Path $outputDirectory "xen-runtime-deployment.json") `
    "运行库部署报告" "Leaf"
$deployment = Get-Content -LiteralPath $deploymentPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([int]$deployment.schema -ne 1 -or
    [string]$deployment.configuration -ne $Configuration) {
    throw "运行库部署报告 schema 或配置无效。"
}
$declaredOutput = [System.IO.Path]::GetFullPath(
    [string]$deployment.output_directory)
if (-not $declaredOutput.Equals(
        $outputDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "运行库部署报告绑定了其他输出目录：$declaredOutput"
}
foreach ($record in @($deployment.files)) {
    $source = Resolve-RequiredPath `
        ([string]$record.source) "运行库来源" "Leaf"
    $deployed = Resolve-RequiredPath `
        (Join-Path $outputDirectory ([string]$record.name)) `
        "已部署运行库" "Leaf"
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $deployedHash = (Get-FileHash -LiteralPath $deployed `
        -Algorithm SHA256).Hash
    if ($sourceHash -ne [string]$record.sha256 -or
        $deployedHash -ne [string]$record.sha256) {
        throw "运行库来源、部署报告与输出哈希不一致：$($record.name)"
    }
}

if ([string]::IsNullOrWhiteSpace($PackageId)) {
    $PackageId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"),
        $gitCommit.Substring(0, 8)
}
if ($PackageId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') {
    throw "PackageId 只能包含字母、数字、点、下划线和连字符。"
}

New-Item -ItemType Directory -Path $PackageOutputRoot -Force | Out-Null
$finalPackageRoot = Join-Path $PackageOutputRoot $PackageId
$packageRoot = Join-Path $PackageOutputRoot ".incoming-$PackageId"
if ((Test-Path -LiteralPath $finalPackageRoot) -or
    (Test-Path -LiteralPath $packageRoot)) {
    throw "本地正式包或临时目录已存在，拒绝覆盖：$PackageId"
}
$releaseRoot = Join-Path $packageRoot $Configuration
$modelRoot = Join-Path $releaseRoot "models"
$scriptRoot = Join-Path $packageRoot "scripts"
New-Item -ItemType Directory -Path $modelRoot -Force | Out-Null
New-Item -ItemType Directory -Path $scriptRoot -Force | Out-Null

Copy-Item -LiteralPath $executable -Destination $releaseRoot
Copy-Item -LiteralPath $deploymentPath -Destination $releaseRoot
foreach ($record in @($deployment.files)) {
    Copy-Item -LiteralPath (Join-Path $outputDirectory ([string]$record.name)) `
        -Destination $releaseRoot
}
$packagedModel = Join-Path $modelRoot ([System.IO.Path]::GetFileName($ModelPath))
Copy-Item -LiteralPath $ModelPath -Destination $packagedModel
foreach ($name in @(
        "benchmark_runtime.ps1",
        "benchmark_network_receiver.ps1",
        "invoke_dual_machine_receiver.ps1")) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) `
        -Destination $scriptRoot
}

$files = @()
foreach ($file in @(Get-ChildItem -LiteralPath $releaseRoot, $scriptRoot `
        -Recurse -File | Sort-Object FullName)) {
    $evidence = Get-FileEvidence $file.FullName
    $evidence.relative_path = Get-RelativePath $file.FullName $packageRoot
    $files += $evidence
}
$modelEvidence = Get-FileEvidence $packagedModel
$modelEvidence.relative_path = Get-RelativePath $packagedModel $packageRoot
$manifest = [ordered]@{
    schema = 1
    complete = $true
    package_type = "xen-dual-machine-receiver"
    package_id = $PackageId
    created_utc = [DateTime]::UtcNow.ToString("o")
    configuration = $Configuration
    allowed_backends = @("cpu")
    allowed_capture_backends = @("xudp_jpeg", "udp_mjpeg", "ndi")
    source = [ordered]@{
        git_commit = $gitCommit
        git_dirty = $false
        build_directory = $BuildDirectory
        deployment_report_sha256 = (Get-FileHash `
            -LiteralPath $deploymentPath -Algorithm SHA256).Hash
    }
    network = [ordered]@{
        host_address = "192.168.3.10"
        auxiliary_address = "192.168.3.20"
        listen_url = "udp://0.0.0.0:5000"
        physical_output_allowed = $false
    }
    model = $modelEvidence
    files = $files
}
$manifestPath = Join-Path $packageRoot "package-manifest.json"
$manifestText = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText(
    $manifestPath, $manifestText, [System.Text.UTF8Encoding]::new($false))
Assert-PublishedPackage $packageRoot $manifest
Rename-Item -LiteralPath $packageRoot -NewName $PackageId
$packageRoot = $finalPackageRoot
$manifestPath = Join-Path $packageRoot "package-manifest.json"

$publishedPath = ""
if (-not $SkipPublish) {
    $DestinationRoot = Resolve-RequiredPath `
        $DestinationRoot "辅机 XenLab SMB 共享" "Container"
    $packagesRoot = Resolve-RequiredPath `
        (Join-Path $DestinationRoot "packages") `
        "辅机 packages 目录" "Container"
    $publishedPath = Join-Path $packagesRoot $PackageId
    $incomingPath = Join-Path $packagesRoot ".incoming-$PackageId"
    if ((Test-Path -LiteralPath $publishedPath) -or
        (Test-Path -LiteralPath $incomingPath)) {
        throw "辅机目标包或临时目录已存在，拒绝覆盖：$PackageId"
    }
    try {
        Copy-Item -LiteralPath $packageRoot -Destination $incomingPath `
            -Recurse
        Assert-PublishedPackage $incomingPath $manifest
        $remoteManifest = Get-Content `
            -LiteralPath (Join-Path $incomingPath "package-manifest.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$remoteManifest.package_id -ne $PackageId) {
            throw "辅机临时包清单与本轮 PackageId 不一致。"
        }
        Rename-Item -LiteralPath $incomingPath -NewName $PackageId
    } catch {
        if (Test-Path -LiteralPath $incomingPath) {
            Remove-Item -LiteralPath $incomingPath -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
        throw
    }
    Assert-PublishedPackage $publishedPath $manifest
}

Write-Host "双机接收包已生成：$packageRoot"
Write-Host "  package_id=$PackageId"
Write-Host "  git_commit=$gitCommit"
Write-Host "  files=$($files.Count)"
Write-Host "  manifest=$manifestPath"
if (-not $SkipPublish) {
    Write-Host "辅机发布完成：$publishedPath"
}
