param(
    [Parameter(Mandatory = $true)]
    [string]$NvidiaBuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$DirectMlBuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$OpenVinoBuildDirectory,
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string[]]$LicenseFiles,
    [string]$ConfigPath = "",
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$GitExecutable = "git",
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-ExistingPath([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-Json([string]$Path, [string]$Description) {
    try {
        return Get-Content -LiteralPath $Path -Encoding utf8 -Raw |
            ConvertFrom-Json
    } catch {
        throw "$Description 不是有效 JSON：$Path；$($_.Exception.Message)"
    }
}

function Assert-BuildIdentity(
        [string]$BuildDirectory,
        [string]$ExpectedRuntime,
        [string]$ExpectedCommit) {
    $identityPath = Join-Path $BuildDirectory "xen-build-identity.json"
    $identity = Read-Json $identityPath "构建身份"
    if ($identity.schema -ne 1 -or $identity.git_dirty -ne $false -or
        $identity.git_commit -ne $ExpectedCommit -or
        $identity.runtime -ne $ExpectedRuntime) {
        throw "构建身份不符合正式组包要求：$identityPath"
    }
    $releaseDirectory = Join-Path $BuildDirectory "Release"
    $worker = Join-Path $releaseDirectory "Xen.exe"
    $launcher = Join-Path $releaseDirectory "XenLauncher.exe"
    $deployment = Join-Path $releaseDirectory "xen-runtime-deployment.json"
    foreach ($required in @($worker, $launcher, $deployment)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "构建缺少正式发布产物：$required"
        }
    }
    $report = Read-Json $deployment "运行库部署报告"
    if ($report.schema -ne 1 -or $report.configuration -ne "Release") {
        throw "运行库部署报告不是 Release schema 1：$deployment"
    }
    foreach ($file in @($report.files)) {
        $deployed = Join-Path $releaseDirectory ([string]$file.name)
        if (-not (Test-Path -LiteralPath $deployed -PathType Leaf)) {
            throw "部署报告授权文件缺失：$deployed"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $deployed).Hash
        if ($actual -ne ([string]$file.sha256).ToUpperInvariant()) {
            throw "部署报告哈希与输出不一致：$deployed"
        }
    }
    $names = @($report.files | ForEach-Object {
        ([string]$_.name).ToLowerInvariant()
    })
    if ($ExpectedRuntime -eq "nvidia") {
        foreach ($requiredName in @(
                "onnxruntime.dll",
                "onnxruntime_providers_cuda.dll",
                "onnxruntime_providers_tensorrt.dll")) {
            if ($names -notcontains $requiredName) {
                throw "NVIDIA 运行时缺少授权能力文件：$requiredName"
            }
        }
        if (@($names | Where-Object { $_ -like "nvinfer_*.dll" }).Count -eq 0) {
            throw "NVIDIA 运行时缺少 TensorRT 核心 DLL"
        }
    } elseif ($ExpectedRuntime -eq "directml") {
        foreach ($requiredName in @("onnxruntime.dll", "directml.dll")) {
            if ($names -notcontains $requiredName) {
                throw "DirectML 运行时缺少授权能力文件：$requiredName"
            }
        }
    } elseif ($ExpectedRuntime -eq "openvino") {
        foreach ($requiredName in @(
                "onnxruntime.dll",
                "onnxruntime_providers_openvino.dll",
                "openvino.dll")) {
            if ($names -notcontains $requiredName) {
                throw "OpenVINO 运行时缺少授权能力文件：$requiredName"
            }
        }
    }
    if ($ExpectedRuntime -ne "nvidia" -and
        @($names | Where-Object {
            $_ -eq "onnxruntime_providers_cuda.dll" -or
            $_ -eq "onnxruntime_providers_tensorrt.dll" -or
            $_ -like "nvinfer_*.dll"
        }).Count -ne 0) {
        throw "$ExpectedRuntime 运行时夹带 NVIDIA ORT/TensorRT 文件"
    }
    if ($ExpectedRuntime -ne "openvino" -and
        $names -contains "onnxruntime_providers_openvino.dll") {
        throw "$ExpectedRuntime 运行时夹带 OpenVINO Provider"
    }
    return [pscustomobject]@{
        Runtime = $ExpectedRuntime
        ReleaseDirectory = $releaseDirectory
        Worker = $worker
        Launcher = $launcher
        DeploymentPath = $deployment
        Deployment = $report
    }
}

function Copy-VerifiedFile(
        [string]$Source,
        [string]$RelativePath,
        [string]$Runtime,
        [string]$Origin,
        [string]$IncomingRoot,
        [System.Collections.Generic.List[object]]$ManifestFiles) {
    $destination = Join-Path $IncomingRoot $RelativePath
    $parent = Split-Path -Parent $destination
    if ($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $destination
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash
    $destinationHash =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "组包复制后哈希不一致：$RelativePath"
    }
    $item = Get-Item -LiteralPath $destination
    $ManifestFiles.Add([ordered]@{
        path = $RelativePath.Replace('\', '/')
        runtime = $Runtime
        size = [UInt64]$item.Length
        sha256 = $destinationHash.ToLowerInvariant()
        source = $Origin
    })
}

$repository = Resolve-ExistingPath $RepositoryRoot "仓库根目录"
$status = & $GitExecutable -C $repository status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0 -or @($status).Count -ne 0) {
    throw "正式组包要求仓库无可跟踪差异和未跟踪文件"
}
$commit = (& $GitExecutable -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "无法读取正式组包 Git commit"
}

$model = Resolve-ExistingPath $ModelPath "发布模型"
$licenses = @($LicenseFiles | ForEach-Object {
    Resolve-ExistingPath $_ "许可证文件"
})
if ($licenses.Count -eq 0) {
    throw "正式发布必须至少提供一份许可证文件"
}
$builds = @(
    Assert-BuildIdentity (Resolve-ExistingPath $NvidiaBuildDirectory "NVIDIA 构建目录") "nvidia" $commit
    Assert-BuildIdentity (Resolve-ExistingPath $DirectMlBuildDirectory "DirectML 构建目录") "directml" $commit
    Assert-BuildIdentity (Resolve-ExistingPath $OpenVinoBuildDirectory "OpenVINO 构建目录") "openvino" $commit
)

$outputParent = Split-Path -Parent ([IO.Path]::GetFullPath($OutputDirectory))
$outputName = Split-Path -Leaf ([IO.Path]::GetFullPath($OutputDirectory))
if (-not $outputParent -or -not $outputName) {
    throw "发布输出目录非法：$OutputDirectory"
}
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
$output = Join-Path $outputParent $outputName
if (Test-Path -LiteralPath $output) {
    throw "正式发布目录已存在，拒绝覆盖：$output"
}
$incoming = Join-Path $outputParent (
    ".{0}.incoming-{1}" -f $outputName, [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $incoming | Out-Null

try {
    foreach ($directory in @("models", "logs", "cache", "licenses", "runtimes")) {
        New-Item -ItemType Directory -Path (Join-Path $incoming $directory) |
            Out-Null
    }
    $manifestFiles = [System.Collections.Generic.List[object]]::new()
    Copy-VerifiedFile $builds[0].Launcher "XenLauncher.exe" "" `
        $builds[0].Launcher $incoming $manifestFiles

    foreach ($build in $builds) {
        $runtimePrefix = "runtimes/$($build.Runtime)"
        Copy-VerifiedFile $build.Worker "$runtimePrefix/Xen.exe" `
            $build.Runtime $build.Worker $incoming $manifestFiles
        Copy-VerifiedFile $build.DeploymentPath `
            "$runtimePrefix/xen-runtime-deployment.json" `
            $build.Runtime $build.DeploymentPath $incoming $manifestFiles
        foreach ($file in @($build.Deployment.files)) {
            $source = Join-Path $build.ReleaseDirectory ([string]$file.name)
            Copy-VerifiedFile $source "$runtimePrefix/$($file.name)" `
                $build.Runtime ([string]$file.source) $incoming $manifestFiles
        }
    }

    Copy-VerifiedFile $model ("models/" + (Split-Path -Leaf $model)) "" `
        $model $incoming $manifestFiles
    if ($ConfigPath) {
        $config = Resolve-ExistingPath $ConfigPath "发布配置"
        Copy-VerifiedFile $config "config.ini" "" $config $incoming $manifestFiles
    }
    $licenseIndex = 0
    foreach ($license in $licenses) {
        ++$licenseIndex
        $licenseName = "{0:D2}-{1}" -f $licenseIndex, (Split-Path -Leaf $license)
        Copy-VerifiedFile $license "licenses/$licenseName" "" `
            $license $incoming $manifestFiles
    }

    $manifest = [ordered]@{
        schema = 1
        product = "Xen"
        git_commit = $commit.ToLowerInvariant()
        runtimes = @(
            [ordered]@{ id = "nvidia"; executable = "runtimes/nvidia/Xen.exe"; backends = @("cpu", "cuda", "tensorrt") }
            [ordered]@{ id = "directml"; executable = "runtimes/directml/Xen.exe"; backends = @("directml") }
            [ordered]@{ id = "openvino"; executable = "runtimes/openvino/Xen.exe"; backends = @("openvino") }
        )
        files = @($manifestFiles)
    }
    $manifestPending = Join-Path $incoming "manifest.json.pending"
    $manifestPath = Join-Path $incoming "manifest.json"
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestPending -Encoding utf8
    Move-Item -LiteralPath $manifestPending -Destination $manifestPath

    Move-Item -LiteralPath $incoming -Destination $output
    Write-Host "统一发布包已原子发布：$output"
    Write-Host "Git commit：$commit"
    Write-Host "清单文件数：$($manifestFiles.Count)"
} catch {
    if (Test-Path -LiteralPath $incoming) {
        Remove-Item -LiteralPath $incoming -Recurse -Force
    }
    throw
}
