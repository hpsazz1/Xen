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
    [string[]]$LicenseEvidence,
    [string]$ConfigPath = "",
    [string]$WorkspaceSettingsPath = "",
    [string[]]$ToolFiles = @(),
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$GitExecutable = "git",
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot "path_safety.psm1") -Force

$msvcRuntimeNames = @(
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll")
$runtimeToolNames = @("xen_recoil_calibration.exe", "xen_recoil_tuner.exe")
$sourceToolNames = @(
    "xen_source_context.exe", "XenClockSource.exe", "XenSender.exe",
    "XenCaptureEvidence.exe", "XenAutoStopCapture.exe")
$repositoryPayload = [ordered]@{
    "tools/model-data/model_data_pipeline.py" = "scripts/model_data_pipeline.py"
    "tools/model-data/model_training_environment.py" = "scripts/model_training_environment.py"
    "tools/model-data/model_training_requirements.txt" = "scripts/model_training_requirements.txt"
    "tools/recoil/import_recoil_profiles.py" = "scripts/import_recoil_profiles.py"
    "tools/recoil/build_recoil_dataset.py" = "scripts/build_recoil_dataset.py"
    "assets/recoil/legacy_manifest.json" = "assets/recoil/legacy_manifest.json"
    "assets/recoil/README.md" = "assets/recoil/README.md"
}

function Resolve-ExistingPath([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ExistingNonEmptyFile([string]$Path, [string]$Description) {
    $resolved = Resolve-ExistingPath $Path $Description
    $item = Get-Item -LiteralPath $resolved -Force
    if ($item -isnot [IO.FileInfo] -or $item.Length -eq 0) {
        throw "$Description must be an ordinary non-empty file: $Path"
    }
    return $item.FullName
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
    if (-not $identity.PSObject.Properties["components"]) {
        throw "Build identity has no components contract: $identityPath"
    }
    $components = [System.Collections.Generic.List[string]]::new()
    $componentIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($component in @($identity.components)) {
        if ($component -isnot [string]) {
            throw "Build identity component id is not a string: $identityPath"
        }
        $componentId = [string]$component
        if ($componentId -cne $componentId.Trim() -or
            $componentId -notmatch '^[a-z0-9][a-z0-9._-]*$') {
            throw "Build identity contains an invalid component id: $identityPath"
        }
        if (-not $componentIds.Add($componentId)) {
            throw "Build identity contains a duplicate component id: $componentId"
        }
        $components.Add($componentId.ToLowerInvariant())
    }
    if ($components.Count -eq 0) {
        throw "Build identity components contract is empty: $identityPath"
    }
    $releaseDirectory = Join-Path $BuildDirectory "Release"
    $worker = Resolve-ExistingNonEmptyFile (Join-Path $releaseDirectory "Xen.exe") "必需 Worker"
    $launcher = Resolve-ExistingNonEmptyFile (Join-Path $releaseDirectory "XenLauncher.exe") "必需 Launcher"
    # UI从当前Worker目录定位校准工具；每个runtime必须随包携带同构建产物。
    $runtimeTools = @{}
    foreach ($name in $runtimeToolNames) {
        $runtimeTools[$name] = Resolve-ExistingNonEmptyFile `
            (Join-Path $releaseDirectory $name) "必需运行时工具"
    }
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
    if ([IO.Path]::GetFullPath([string]$report.output_directory) -ine
        [IO.Path]::GetFullPath($releaseDirectory)) {
        throw "运行库部署报告绑定了其他输出目录：$deployment"
    }
    $deploymentNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($report.files)) {
        $fileName = [string]$file.name
        $deployed = Resolve-XenDirectChildPath `
            -RootPath $releaseDirectory `
            -Name $fileName `
            -Description "Deployment report file name"
        if (-not $deploymentNames.Add($fileName)) {
            throw "Deployment report contains a duplicate basename: $fileName"
        }
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
        RuntimeTools = $runtimeTools
        DeploymentPath = $deployment
        Deployment = $report
        Components = @($components)
    }
}

function Assert-ReleaseLayout([object]$Build) {
    $path = Resolve-ExistingNonEmptyFile `
        (Join-Path $Build.ReleaseDirectory "xen-release-layout.json") "发布依赖分组"
    $layout = Read-Json $path "发布依赖分组"
    if ($layout.schema -ne 1 -or $layout.configuration -ne "Release" -or
        $Build.Components -notcontains "msvc-runtime") {
        throw "发布依赖分组或 MSVC component 无效：$path"
    }
    $crtRoot = Resolve-ExistingPath ([string]$layout.msvc_runtime.source_directory) "官方 CRT 目录"
    if ($crtRoot -notmatch '(?i)[\\/]VC[\\/]Redist[\\/]MSVC[\\/][^\\/]+[\\/]x64[\\/]Microsoft\.VC14[0-9]\.CRT$') {
        throw "CRT 来源必须为官方 VS x64 redist 目录：$crtRoot"
    }
    $license = Resolve-ExistingNonEmptyFile ([string]$layout.msvc_runtime.license_file) "官方 CRT REDIST 证据"
    $instance = $crtRoot
    foreach ($unused in 1..6) { $instance = Split-Path -Parent $instance }
    $licenseRoot = (Join-Path $instance "Licenses") + [IO.Path]::DirectorySeparatorChar
    if (-not $license.StartsWith($licenseRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $license) -ine "Redist.txt") {
        throw "CRT REDIST 证据不属于同一 VS 实例：$license"
    }
    $records = @{}
    foreach ($file in @($Build.Deployment.files)) { $records[[string]$file.name] = $file }
    $groups = @{}
    foreach ($group in @("launcher", "source")) {
        $selected = [System.Collections.Generic.List[object]]::new()
        $names = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($source in @($layout.groups.$group)) {
            if ($source -isnot [string]) { throw "发布依赖分组来源必须是路径字符串：$group" }
            $name = Split-Path -Leaf $source
            $null = Resolve-XenDirectChildPath -RootPath $Build.ReleaseDirectory `
                -Name $name -Description "Release layout file name"
            if (-not $names.Add($name) -or -not $records.ContainsKey($name)) {
                throw "发布依赖分组重复或未在部署报告授权：$group/$name"
            }
            $record = $records[$name]
            if ([IO.Path]::GetFullPath($source) -ine [IO.Path]::GetFullPath([string]$record.source)) {
                throw "发布依赖分组来源与部署报告不一致：$group/$name"
            }
            $isCrt = $msvcRuntimeNames -contains $name
            if (($group -eq "launcher" -and -not $isCrt) -or
                ($group -eq "source" -and -not $isCrt -and
                    $name -notmatch '^(?i:opencv_world[0-9]+\.dll|opencv_videoio_ffmpeg[0-9_]+\.dll|Processing\.NDI\.Lib\.(x64\.dll|Licenses\.txt))$')) {
                throw "发布依赖分组夹带 Provider 或未批准文件：$group/$name"
            }
            if ($isCrt -and [IO.Path]::GetFullPath((Split-Path -Parent $source)) -ine $crtRoot) {
                throw "CRT 文件来源不属于已登记的官方目录：$source"
            }
            if ($isCrt) {
                $originalCrt = Resolve-ExistingNonEmptyFile $source "官方 CRT 文件"
                if ((Get-FileHash -Algorithm SHA256 -LiteralPath $originalCrt).Hash -ne [string]$record.sha256) {
                    throw "官方 CRT 来源与部署报告哈希不一致：$source"
                }
            }
            $selected.Add($record)
        }
        foreach ($name in $msvcRuntimeNames) {
            if (-not $names.Contains($name)) { throw "发布依赖分组缺少必需 CRT：$group/$name" }
        }
        if ($group -eq "source") {
            if (@($names | Where-Object { $_ -match '^opencv_world[0-9]+\.dll$' }).Count -ne 1) {
                throw "源工具依赖分组必须包含唯一 OpenCV World"
            }
            if ($Build.Components -contains "ndi") {
                foreach ($name in @("Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")) {
                    if (-not $names.Contains($name)) { throw "源工具缺少 NDI 部署闭包：$name" }
                }
            }
        }
        $groups[$group] = @($selected)
    }
    return [pscustomobject]@{ Path = $path; Groups = $groups; CrtLicense = $license }
}

function Resolve-LicenseEvidenceClosure(
        [string[]]$Entries,
        [string[]]$RequiredComponents) {
    if (@($Entries).Count -eq 0) {
        throw "Release requires license evidence for every build component."
    }
    $required = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($componentId in $RequiredComponents) {
        $null = $required.Add($componentId)
    }
    $covered = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $evidencePaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $evidenceKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $resolvedEvidence = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in @($Entries)) {
        $separator = if ($null -eq $entry) { -1 } else { $entry.IndexOf('=') }
        if ($separator -le 0 -or $separator -eq ($entry.Length - 1)) {
            throw "License evidence must use component_id=path: $entry"
        }
        $componentId = $entry.Substring(0, $separator)
        $path = $entry.Substring($separator + 1)
        if ($componentId -cne $componentId.Trim() -or
            $componentId -notmatch '^[a-z0-9][a-z0-9._-]*$') {
            throw "License evidence contains an invalid component id: $componentId"
        }
        if (-not $required.Contains($componentId)) {
            throw "License evidence names an unknown component: $componentId"
        }
        $resolved = Resolve-ExistingNonEmptyFile $path `
            "License evidence for component $componentId"
        $evidenceKey = "$componentId`0$resolved"
        if (-not $evidenceKeys.Add($evidenceKey) -or
            -not $evidencePaths.Add($resolved)) {
            throw "License evidence contains a duplicate file mapping: $entry"
        }
        $null = $covered.Add($componentId)
        $resolvedEvidence.Add([pscustomobject]@{
            ComponentId = $componentId.ToLowerInvariant()
            Path = $resolved
        })
    }
    $missing = @($required | Where-Object { -not $covered.Contains($_) } |
        Sort-Object)
    if ($missing.Count -ne 0) {
        throw "Build components are missing license evidence: $($missing -join ', ')"
    }
    return @($resolvedEvidence | Sort-Object ComponentId, Path)
}

function Copy-VerifiedFile(
        [string]$Source,
        [string]$RelativePath,
        [string]$Runtime,
        [string]$Origin,
        [string]$IncomingRoot,
        [System.Collections.Generic.List[object]]$ManifestFiles,
        [string]$ComponentId = "") {
    if (@($ManifestFiles | Where-Object { $_.path -ieq $RelativePath.Replace('\', '/') }).Count) {
        throw "发布文件路径重复：$RelativePath"
    }
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
    $entry = [ordered]@{
        path = $RelativePath.Replace('\', '/')
        runtime = $Runtime
        size = [UInt64]$item.Length
        sha256 = $destinationHash.ToLowerInvariant()
        source = $Origin
    }
    if ($ComponentId) {
        $entry["component_id"] = $ComponentId
    }
    $ManifestFiles.Add($entry)
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

$model = Resolve-ExistingNonEmptyFile $ModelPath "发布模型"
$workspaceSettings = ""
if ($WorkspaceSettingsPath) {
    $workspaceSettings = Resolve-ExistingNonEmptyFile $WorkspaceSettingsPath "Workspace 设置"
    try {
        $settingsText = Get-Content -LiteralPath $workspaceSettings -Encoding utf8 -Raw
        $settings = $settingsText | ConvertFrom-Json
    } catch {
        # 设置可含外部路径与用户数据；不把解析器的原始内容诊断写入发布日志。
        throw "Workspace 设置不是有效 JSON：$workspaceSettings"
    }
    if (-not $settingsText.TrimStart().StartsWith("{", [StringComparison]::Ordinal) -or
        $settings -isnot [pscustomobject] -or
        -not $settings.PSObject.Properties["schema_version"] -or
        ($settings.schema_version -isnot [int] -and $settings.schema_version -isnot [long]) -or
        $settings.schema_version -ne 1) {
        throw "Workspace 设置要求对象及 schema_version=1：$workspaceSettings"
    }
}
$tools = @($ToolFiles | ForEach-Object {
    $tool = Resolve-ExistingNonEmptyFile $_ "附加验收工具"
    if ([IO.Path]::GetExtension($tool) -notin @(".ps1", ".psm1", ".psd1", ".py", ".json", ".md", ".txt")) {
        throw "附加验收工具只接受脚本/资料，原生工具必须登记依赖闭包：$tool"
    }
    $tool
})
$builds = @(
    Assert-BuildIdentity (Resolve-ExistingPath $NvidiaBuildDirectory "NVIDIA 构建目录") "nvidia" $commit
    Assert-BuildIdentity (Resolve-ExistingPath $DirectMlBuildDirectory "DirectML 构建目录") "directml" $commit
    Assert-BuildIdentity (Resolve-ExistingPath $OpenVinoBuildDirectory "OpenVINO 构建目录") "openvino" $commit
)
$layouts = @($builds | ForEach-Object { Assert-ReleaseLayout $_ })
$sourceTools = @{}
foreach ($name in $sourceToolNames) {
    $sourceTools[$name] = Resolve-ExistingNonEmptyFile `
        (Join-Path $builds[0].ReleaseDirectory $name) "必需源工具"
}
$payloadFiles = @{}
foreach ($entry in $repositoryPayload.GetEnumerator()) {
    $source = Resolve-ExistingNonEmptyFile (Join-Path $repository $entry.Value) "版本内脚本/资源"
    $built = Resolve-ExistingNonEmptyFile (Join-Path $builds[0].ReleaseDirectory $entry.Key) "构建脚本/资源"
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $built).Hash) {
        throw "构建脚本/资源与当前版本不同：$($entry.Key)"
    }
    $payloadFiles[$entry.Key] = $built
}
$requiredComponents = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($build in $builds) {
    foreach ($componentId in @($build.Components)) {
        $null = $requiredComponents.Add($componentId)
    }
}
$licenses = @(Resolve-LicenseEvidenceClosure $LicenseEvidence `
    @($requiredComponents))
foreach ($layout in $layouts) {
    if (@($licenses | Where-Object {
            $_.ComponentId -eq "msvc-runtime" -and $_.Path -ieq $layout.CrtLicense
        }).Count -eq 0) {
        throw "msvc-runtime 缺少本次官方 CRT 对应的 REDIST 许可证据：$($layout.CrtLicense)"
    }
}

$outputParent = Split-Path -Parent ([IO.Path]::GetFullPath($OutputDirectory))
$outputName = Split-Path -Leaf ([IO.Path]::GetFullPath($OutputDirectory))
if (-not $outputParent -or -not $outputName) {
    throw "发布输出目录非法：$OutputDirectory"
}
Assert-XenNoReparsePathChain $outputParent "release output parent"
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
$output = Join-Path $outputParent $outputName
if (Test-Path -LiteralPath $output) {
    throw "正式发布目录已存在，拒绝覆盖：$output"
}
$incoming = Join-Path $outputParent (
    ".{0}.incoming-{1}" -f $outputName, [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $incoming | Out-Null

try {
    foreach ($directory in @(
            "models", "logs", "cache", "licenses", "runtimes", "tools")) {
        New-Item -ItemType Directory -Path (Join-Path $incoming $directory) |
            Out-Null
    }
    $manifestFiles = [System.Collections.Generic.List[object]]::new()
    Copy-VerifiedFile $builds[0].Launcher "XenLauncher.exe" "" `
        $builds[0].Launcher $incoming $manifestFiles
    foreach ($file in $layouts[0].Groups.launcher) {
        $source = Join-Path $builds[0].ReleaseDirectory ([string]$file.name)
        Copy-VerifiedFile $source ([string]$file.name) "" `
            ([string]$file.source) $incoming $manifestFiles "msvc-runtime"
    }

    foreach ($build in $builds) {
        $runtimePrefix = "runtimes/$($build.Runtime)"
        Copy-VerifiedFile $build.Worker "$runtimePrefix/Xen.exe" `
            $build.Runtime $build.Worker $incoming $manifestFiles
        foreach ($name in $runtimeToolNames) {
            $tool = $build.RuntimeTools[$name]
            Copy-VerifiedFile $tool "$runtimePrefix/$name" `
                $build.Runtime $tool $incoming $manifestFiles
        }
        Copy-VerifiedFile $build.DeploymentPath `
            "$runtimePrefix/xen-runtime-deployment.json" `
            $build.Runtime $build.DeploymentPath $incoming $manifestFiles
        foreach ($file in @($build.Deployment.files)) {
            $source = Resolve-XenDirectChildPath `
                -RootPath $build.ReleaseDirectory `
                -Name ([string]$file.name) `
                -Description "Deployment report file name"
            Copy-VerifiedFile $source "$runtimePrefix/$($file.name)" `
                $build.Runtime ([string]$file.source) $incoming $manifestFiles
        }
    }

    foreach ($name in $sourceToolNames) {
        $tool = $sourceTools[$name]
        Copy-VerifiedFile $tool "tools/source/$name" "" $tool $incoming $manifestFiles
    }
    foreach ($file in $layouts[0].Groups.source) {
        $source = Join-Path $builds[0].ReleaseDirectory ([string]$file.name)
        Copy-VerifiedFile $source "tools/source/$($file.name)" "" `
            ([string]$file.source) $incoming $manifestFiles
    }
    foreach ($relativePath in $repositoryPayload.Keys) {
        Copy-VerifiedFile $payloadFiles[$relativePath] $relativePath "" `
            (Join-Path $repository $repositoryPayload[$relativePath]) $incoming $manifestFiles
    }

    Copy-VerifiedFile $model ("models/" + (Split-Path -Leaf $model)) "" `
        $model $incoming $manifestFiles
    if ($ConfigPath) {
        $config = Resolve-ExistingPath $ConfigPath "发布配置"
        Copy-VerifiedFile $config "config.ini" "" $config $incoming $manifestFiles
    }
    if ($workspaceSettings) {
        Copy-VerifiedFile $workspaceSettings "cache/model-workspace/settings.json" "" `
            $workspaceSettings $incoming $manifestFiles
    }
    $toolNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($tool in $tools) {
        $toolName = Split-Path -Leaf $tool
        if (-not $toolNames.Add($toolName)) {
            throw "发布工具文件名重复：$toolName"
        }
        Copy-VerifiedFile $tool "tools/acceptance/$toolName" "" $tool `
            $incoming $manifestFiles
    }
    $licenseIndex = 0
    foreach ($license in $licenses) {
        ++$licenseIndex
        $licenseName = "{0:D2}-{1}-{2}" -f $licenseIndex, `
            $license.ComponentId, (Split-Path -Leaf $license.Path)
        $relativePath = "licenses/$licenseName"
        Copy-VerifiedFile $license.Path $relativePath "" `
            $license.Path $incoming $manifestFiles $license.ComponentId
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
        if ([IO.Path]::GetFullPath((Split-Path -Parent $incoming)) -ine
            [IO.Path]::GetFullPath($outputParent)) {
            throw "拒绝清理越出发布父目录的 incoming：$incoming"
        }
        Assert-XenNoReparsePathChain $incoming "owned release incoming" -RequireExistingLeaf
        Remove-Item -LiteralPath $incoming -Recurse -Force
    }
    throw
}
