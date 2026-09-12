param(
    [Parameter(Mandatory = $true)]
    [string]$PublishScript,
    [Parameter(Mandatory = $true)]
    [string]$GitExecutable,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot "..\scripts\path_safety.psm1") -Force

function Write-Utf8([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Set-Content -LiteralPath $Path -Value $Content -Encoding utf8
}

function Invoke-Publisher([hashtable]$Arguments) {
    try {
        $output = @(& $PublishScript @Arguments *>&1) -join `
            [Environment]::NewLine
        return [pscustomobject]@{ ExitCode = 0; Output = $output }
    } catch {
        return [pscustomobject]@{
            ExitCode = 1
            Output = ($_ | Out-String)
        }
    }
}

function New-FakeBuild(
        [string]$Root,
        [string]$Runtime,
        [string]$Commit,
        [string[]]$RuntimeFiles,
        [string[]]$Components,
        [bool]$IncludeCalibration = $true) {
    $release = Join-Path $Root "Release"
    New-Item -ItemType Directory -Path $release -Force | Out-Null
    Write-Utf8 (Join-Path $release "Xen.exe") "worker-$Runtime"
    Write-Utf8 (Join-Path $release "XenLauncher.exe") "launcher"
    # 这里只验证发布脚本合同，文本 EXE 仅存在于 owner 隔离夹具；正式组包使用真实构建。
    foreach ($name in @("xen_recoil_tuner.exe", "xen_source_context.exe", "XenClockSource.exe",
            "XenSender.exe", "XenCaptureEvidence.exe", "XenAutoStopCapture.exe")) {
        Write-Utf8 (Join-Path $release $name) "tool-$Runtime-$name"
    }
    if ($IncludeCalibration) {
        Write-Utf8 (Join-Path $release "xen_recoil_calibration.exe") "calibration-$Runtime"
    }
    $files = @()
    $allRuntimeFiles = @($RuntimeFiles + $fixtureCrtNames + $fixtureSourceDependencies | Sort-Object -Unique)
    foreach ($name in $allRuntimeFiles) {
        $path = Join-Path $release $name
        $isCrt = $fixtureCrtNames -contains $name
        $source = if ($isCrt) { Join-Path $fixtureCrtRoot $name } else { Join-Path $root "sdk/$Runtime/$name" }
        $content = if ($isCrt) { "fixture-crt-$name" } else { "runtime-$Runtime-$name" }
        Write-Utf8 $source $content
        Write-Utf8 $path $content
        $files += [ordered]@{
            name = $name
            source = $source
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        }
    }
    [ordered]@{
        schema = 1
        configuration = "Release"
        output_directory = $release
        authorized_manifest = "fixture"
        files = $files
    } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $release "xen-runtime-deployment.json") -Encoding utf8
    [ordered]@{
        schema = 1
        source_root = "fixture"
        git_commit = $Commit
        git_dirty = $false
        runtime = $Runtime
        components = $Components
    } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $Root "xen-build-identity.json") -Encoding utf8
    $crtFiles = @($files | Where-Object { $fixtureCrtNames -contains $_.name } | ForEach-Object { $_.source })
    [ordered]@{
        schema = 1
        configuration = "Release"
        msvc_runtime = @{ source_directory = $fixtureCrtRoot; license_file = $fixtureCrtLicense }
        groups = @{
            launcher = $crtFiles
            source = @($files | Where-Object {
                $fixtureCrtNames -contains $_.name -or $fixtureSourceDependencies -contains $_.name
            } | ForEach-Object { $_.source })
        }
    } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $release "xen-release-layout.json") -Encoding utf8
    foreach ($entry in $fixturePayload.GetEnumerator()) {
        $destination = Join-Path $release $entry.Key
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repository $entry.Value) -Destination $destination
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $repositoryRoot
$root = $ownedTest.RootPath

try {
    $repository = Join-Path $root "repo"
    New-Item -ItemType Directory -Path $repository | Out-Null
    Write-Utf8 (Join-Path $repository "tracked.txt") "fixture"
    $fixturePayload = [ordered]@{
        "tools/model-data/model_data_pipeline.py" = "scripts/model_data_pipeline.py"
        "tools/model-data/model_training_environment.py" = "scripts/model_training_environment.py"
        "tools/model-data/model_training_requirements.txt" = "scripts/model_training_requirements.txt"
        "tools/recoil/import_recoil_profiles.py" = "scripts/import_recoil_profiles.py"
        "tools/recoil/build_recoil_dataset.py" = "scripts/build_recoil_dataset.py"
        "assets/recoil/legacy_manifest.json" = "assets/recoil/legacy_manifest.json"
        "assets/recoil/README.md" = "assets/recoil/README.md"
    }
    foreach ($relativePath in $fixturePayload.Values) {
        $destination = Join-Path $repository $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repositoryRoot $relativePath) -Destination $destination
    }
    & $GitExecutable -C $repository init --quiet
    & $GitExecutable -C $repository config user.email "xen-release-test@example.invalid"
    & $GitExecutable -C $repository config user.name "Xen Release Test"
    & $GitExecutable -C $repository add --all
    & $GitExecutable -C $repository commit --quiet -m "初始化发布夹具"
    if ($LASTEXITCODE -ne 0) { throw "无法创建发布夹具 Git 仓库" }
    $commit = (& $GitExecutable -C $repository rev-parse HEAD).Trim()

    $model = Join-Path $root "model.onnx"
    $tool = Join-Path $root "acceptance.ps1"
    Write-Utf8 $model "model"
    Write-Utf8 $tool "Write-Host acceptance"
    $nvidia = Join-Path $root "build-nvidia"
    $directml = Join-Path $root "build-directml"
    $openvino = Join-Path $root "build-openvino"
    $fixtureCrtNames = @(
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
        "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll")
    $fixtureSourceDependencies = @(
        "opencv_world4140.dll", "opencv_videoio_ffmpeg4140_64.dll",
        "Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")
    $fixtureCrtRoot = Join-Path $root "sdk/VS/VC/Redist/MSVC/14.51/x64/Microsoft.VC145.CRT"
    $fixtureCrtLicense = Join-Path $root "sdk/VS/Licenses/2052/Redist.txt"
    Write-Utf8 $fixtureCrtLicense "synthetic REDIST evidence for publisher contract only"
    $commonComponents = @(
        "imgui", "nlohmann-json", "ndi", "onnxruntime", "opencv",
        "simpleini", "spdlog", "msvc-runtime")
    $nvidiaComponents = @($commonComponents) + @("cuda", "cudnn", "tensorrt")
    $directMlComponents = @($commonComponents) + @("directml")
    $openVinoComponents = @($commonComponents) + @("openvino")
    New-FakeBuild $nvidia "nvidia" $commit @(
        "onnxruntime.dll", "onnxruntime_providers_cuda.dll",
        "onnxruntime_providers_tensorrt.dll", "nvinfer_10.dll") `
        $nvidiaComponents
    New-FakeBuild $directml "directml" $commit @(
        "onnxruntime.dll", "DirectML.dll") $directMlComponents
    New-FakeBuild $openvino "openvino" $commit @(
        "onnxruntime.dll", "onnxruntime_providers_openvino.dll", "openvino.dll") `
        $openVinoComponents

    $requiredComponents = @(
        $nvidiaComponents + $directMlComponents + $openVinoComponents |
            Sort-Object -Unique)
    $licenseEvidenceByComponent = [ordered]@{}
    foreach ($component in $requiredComponents) {
        $path = if ($component -eq "msvc-runtime") { $fixtureCrtLicense } else { Join-Path $root "license-$component.txt" }
        Write-Utf8 $path "license evidence for $component"
        $licenseEvidenceByComponent[$component] = "$component=$path"
    }
    $licenseEvidence = @($licenseEvidenceByComponent.Values)

    function Assert-PreflightFailure([string]$Id, [string]$ExpectedError, [string[]]$AdditionalTools = @()) {
        $invalidParent = Join-Path $root "required-invalid-$Id"
        $result = Invoke-Publisher @{
            NvidiaBuildDirectory = $nvidia
            DirectMlBuildDirectory = $directml
            OpenVinoBuildDirectory = $openvino
            ModelPath = $model
            LicenseEvidence = $licenseEvidence
            ToolFiles = $AdditionalTools
            RepositoryRoot = $repository
            GitExecutable = $GitExecutable
            OutputDirectory = (Join-Path $invalidParent "Xen-release")
        }
        if ($result.ExitCode -eq 0 -or $result.Output -notmatch $ExpectedError -or
            (Test-Path -LiteralPath $invalidParent)) {
            throw "必需载荷/依赖缺口没有在创建发布父目录前拒绝：$Id；$($result.Output)"
        }
    }
    Assert-PreflightFailure "unregistered-native-tool" "原生工具必须登记依赖闭包" @(
        (Join-Path $nvidia "Release/onnxruntime.dll"))

    # 逐项删除 owned 合成构建中的必需文件；缺件不得靠其他 runtime 或旧包补齐。
    $requiredFiles = @(
        "Xen.exe", "XenLauncher.exe",
        "xen_source_context.exe", "XenClockSource.exe", "XenSender.exe",
        "XenCaptureEvidence.exe", "XenAutoStopCapture.exe", "xen-release-layout.json"
    ) + @($fixturePayload.Keys)
    foreach ($relative in $requiredFiles) {
        $path = Join-Path $nvidia "Release/$relative"
        $originalBytes = [IO.File]::ReadAllBytes($path)
        $id = $relative.Replace('/', '-')
        try {
            Remove-Item -LiteralPath $path
            Assert-PreflightFailure "missing-$id" ([regex]::Escape((Split-Path -Leaf $relative)))
            [IO.File]::WriteAllBytes($path, [byte[]]@())
            Assert-PreflightFailure "empty-$id" "ordinary non-empty file"
        } finally { [IO.File]::WriteAllBytes($path, $originalBytes) }
    }
    foreach ($build in @($nvidia, $directml, $openvino)) {
        $path = Join-Path $build "Release/xen_recoil_tuner.exe"
        $originalBytes = [IO.File]::ReadAllBytes($path)
        try {
            Remove-Item -LiteralPath $path
            Assert-PreflightFailure "missing-tuner-$(Split-Path -Leaf $build)" "xen_recoil_tuner.exe"
        } finally { [IO.File]::WriteAllBytes($path, $originalBytes) }
    }
    $payloadPath = Join-Path $nvidia "Release/tools/model-data/model_data_pipeline.py"
    $payloadBytes = [IO.File]::ReadAllBytes($payloadPath)
    try {
        Write-Utf8 $payloadPath "stale built script"
        Assert-PreflightFailure "stale-script" "与当前版本不同"
    } finally { [IO.File]::WriteAllBytes($payloadPath, $payloadBytes) }

    $layoutPath = Join-Path $nvidia "Release/xen-release-layout.json"
    $layoutText = Get-Content -LiteralPath $layoutPath -Raw -Encoding utf8
    $report = Get-Content -LiteralPath (Join-Path $nvidia "Release/xen-runtime-deployment.json") -Raw | ConvertFrom-Json
    $ortSource = [string](@($report.files | Where-Object { $_.name -eq "onnxruntime.dll" })[0].source)
    try {
        foreach ($group in @("launcher", "source")) {
            $layout = $layoutText | ConvertFrom-Json
            $layout.groups.$group = @($layout.groups.$group) + @($ortSource)
            $layout | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $layoutPath -Encoding utf8
            Assert-PreflightFailure "provider-in-$group" "夹带 Provider"
        }
        $layout = $layoutText | ConvertFrom-Json
        $layout.groups.launcher = @($layout.groups.launcher | Where-Object { (Split-Path -Leaf $_) -ne "msvcp140_atomic_wait.dll" })
        $layout | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $layoutPath -Encoding utf8
        Assert-PreflightFailure "missing-atomic-wait" "缺少必需 CRT"
        $layout = $layoutText | ConvertFrom-Json
        $layout.msvc_runtime.source_directory = Join-Path $env:SystemRoot "System32"
        $layout | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $layoutPath -Encoding utf8
        Assert-PreflightFailure "system32-crt" "官方 VS x64 redist"
    } finally { [IO.File]::WriteAllText($layoutPath, $layoutText, [Text.UTF8Encoding]::new($false)) }
    $crtPath = Join-Path $fixtureCrtRoot "msvcp140_atomic_wait.dll"
    $crtBytes = [IO.File]::ReadAllBytes($crtPath)
    try {
        Write-Utf8 $crtPath "source changed after runtime deployment"
        Assert-PreflightFailure "changed-crt-source" "官方 CRT 来源与部署报告哈希不一致"
    } finally { [IO.File]::WriteAllBytes($crtPath, $crtBytes) }

    # 使用独立合成构建目录制造缺失，避免改动真实构建或删除既有夹具。
    foreach ($runtime in @("nvidia", "directml", "openvino")) {
        $originalBuild = switch ($runtime) {
            "nvidia" { $nvidia }
            "directml" { $directml }
            "openvino" { $openvino }
        }
        $originalIdentity = Get-Content -LiteralPath (Join-Path $originalBuild "xen-build-identity.json") -Raw | ConvertFrom-Json
        $originalDeployment = Get-Content -LiteralPath (Join-Path $originalBuild "Release/xen-runtime-deployment.json") -Raw | ConvertFrom-Json
        $missingBuild = Join-Path $root "missing-calibration-$runtime"
        New-FakeBuild $missingBuild $runtime $commit @($originalDeployment.files | ForEach-Object { $_.name }) `
            @($originalIdentity.components) $false
        $invalidParent = Join-Path $root "calibration-invalid-$runtime"
        $arguments = @{
            NvidiaBuildDirectory = $nvidia
            DirectMlBuildDirectory = $directml
            OpenVinoBuildDirectory = $openvino
            ModelPath = $model
            LicenseEvidence = $licenseEvidence
            RepositoryRoot = $repository
            GitExecutable = $GitExecutable
            OutputDirectory = (Join-Path $invalidParent "Xen-release")
        }
        $key = switch ($runtime) {
            "nvidia" { "NvidiaBuildDirectory" }
            "directml" { "DirectMlBuildDirectory" }
            "openvino" { "OpenVinoBuildDirectory" }
        }
        $arguments[$key] = $missingBuild
        $missingResult = Invoke-Publisher $arguments
        if ($missingResult.ExitCode -eq 0 -or $missingResult.Output -notmatch "xen_recoil_calibration.exe" -or
            (Test-Path -LiteralPath $invalidParent)) {
            throw "缺失校准工具必须在创建发布目录前拒绝：$runtime；$($missingResult.Output)"
        }
        [IO.File]::WriteAllBytes((Join-Path $missingBuild "Release/xen_recoil_calibration.exe"), [byte[]]@())
        $emptyResult = Invoke-Publisher $arguments
        if ($emptyResult.ExitCode -eq 0 -or $emptyResult.Output -notmatch "ordinary non-empty file" -or
            (Test-Path -LiteralPath $invalidParent)) {
            throw "空校准工具必须在创建发布目录前拒绝：$runtime；$($emptyResult.Output)"
        }
    }

    $nvidiaReportPath = Join-Path $nvidia `
        "Release\xen-runtime-deployment.json"
    $originalNvidiaReport = Get-Content -LiteralPath $nvidiaReportPath `
        -Raw -Encoding utf8
    $publisherSentinel = Join-Path $root "publisher-sentinel.txt"
    Write-Utf8 $publisherSentinel "must-stay-unchanged"
    $sentinelHash = (Get-FileHash -LiteralPath $publisherSentinel `
        -Algorithm SHA256).Hash
    $nestedRuntime = Join-Path $nvidia "Release\nested\evil.dll"
    Write-Utf8 $nestedRuntime "nested-runtime"
    $parentRuntime = Join-Path $nvidia "outside.dll"
    Write-Utf8 $parentRuntime "parent-runtime"
    $absoluteRuntime = Join-Path $root "absolute-runtime.dll"
    Write-Utf8 $absoluteRuntime "absolute-runtime"
    $invalidCases = @(
        [pscustomobject]@{
            Id = "descendant"
            Name = "nested/evil.dll"
            Path = $nestedRuntime
            Duplicate = $false
        },
        [pscustomobject]@{
            Id = "parent"
            Name = "..\outside.dll"
            Path = $parentRuntime
            Duplicate = $false
        },
        [pscustomobject]@{
            Id = "absolute"
            Name = $absoluteRuntime
            Path = $absoluteRuntime
            Duplicate = $false
        },
        [pscustomobject]@{
            Id = "duplicate"
            Name = "onnxruntime.dll"
            Path = (Join-Path $nvidia "Release\onnxruntime.dll")
            Duplicate = $true
        })
    foreach ($case in $invalidCases) {
        $report = $originalNvidiaReport | ConvertFrom-Json
        if ($case.Duplicate) {
            $extra = $report.files[0]
        } else {
            $extra = [pscustomobject]@{
                name = $case.Name
                source = "fixture/nvidia/$($case.Name)"
                sha256 = (Get-FileHash -LiteralPath $case.Path `
                    -Algorithm SHA256).Hash
            }
        }
        $report.files = @($report.files) + @($extra)
        $report | ConvertTo-Json -Depth 5 |
            Set-Content -LiteralPath $nvidiaReportPath -Encoding utf8
        $invalidOutput = Join-Path $root `
            ("Xen-release-invalid-{0}" -f $case.Id)
        $result = Invoke-Publisher @{
            NvidiaBuildDirectory = $nvidia
            DirectMlBuildDirectory = $directml
            OpenVinoBuildDirectory = $openvino
            ModelPath = $model
            LicenseEvidence = $licenseEvidence
            ToolFiles = $tool
            RepositoryRoot = $repository
            GitExecutable = $GitExecutable
            OutputDirectory = $invalidOutput
        }
        $failure = $result.Output
        $failureExitCode = $result.ExitCode
        $incoming = Get-ChildItem -LiteralPath $root -Force |
            Where-Object {
                $_.Name -like ".$(Split-Path -Leaf $invalidOutput).incoming-*"
            }
        if ($failureExitCode -eq 0 -or
            $failure -notmatch "safe basename|duplicate" -or
            (Test-Path -LiteralPath $invalidOutput) -or
            @($incoming).Count -ne 0 -or
            (Get-FileHash -LiteralPath $publisherSentinel `
                -Algorithm SHA256).Hash -ne $sentinelHash) {
            throw "部署报告非法名称未在任何复制前失败封闭：$($case.Id)；$failure"
        }
    }
    [IO.File]::WriteAllText(
        $nvidiaReportPath, $originalNvidiaReport,
        [Text.UTF8Encoding]::new($false))

    $closureFailures = [System.Collections.Generic.List[string]]::new()
    $licenseDirectory = Join-Path $root "license-directory"
    New-Item -ItemType Directory -Path $licenseDirectory | Out-Null
    Write-Utf8 (Join-Path $licenseDirectory "LICENSE.txt") "nested license"
    $zeroByteLicense = Join-Path $root "zero-byte-license.txt"
    [IO.File]::WriteAllBytes($zeroByteLicense, [byte[]]@())
    $unrelatedLicense = Join-Path $root "unrelated.txt"
    Write-Utf8 $unrelatedLicense "not declared by any build component"
    $closureCases = [System.Collections.Generic.List[object]]::new()
    $closureCases.Add([pscustomobject]@{
        Id = "directory"
        LicenseEvidence = @("imgui=$licenseDirectory")
        ExpectedError = "ordinary non-empty file"
    })
    $closureCases.Add([pscustomobject]@{
        Id = "zero-byte"
        LicenseEvidence = @("imgui=$zeroByteLicense")
        ExpectedError = "ordinary non-empty file"
    })
    $closureCases.Add([pscustomobject]@{
        Id = "duplicate"
        LicenseEvidence = @($licenseEvidence) + @($licenseEvidence[0])
        ExpectedError = "duplicate"
    })
    $closureCases.Add([pscustomobject]@{
        Id = "unrelated-single"
        LicenseEvidence = @("unrelated=$unrelatedLicense")
        ExpectedError = "unknown component"
    })
    foreach ($component in $requiredComponents) {
        $closureCases.Add([pscustomobject]@{
            Id = "missing-$component"
            LicenseEvidence = @($licenseEvidenceByComponent.GetEnumerator() |
                Where-Object { $_.Key -ne $component } |
                ForEach-Object { $_.Value })
            ExpectedError = "missing license evidence"
        })
    }
    foreach ($case in $closureCases) {
        $invalidParent = Join-Path $root "license-invalid-$($case.Id)"
        $invalidOutput = Join-Path $invalidParent "Xen-release"
        $result = Invoke-Publisher @{
            NvidiaBuildDirectory = $nvidia
            DirectMlBuildDirectory = $directml
            OpenVinoBuildDirectory = $openvino
            ModelPath = $model
            LicenseEvidence = $case.LicenseEvidence
            ToolFiles = $tool
            RepositoryRoot = $repository
            GitExecutable = $GitExecutable
            OutputDirectory = $invalidOutput
        }
        $failure = $result.Output
        $failureExitCode = $result.ExitCode
        if ($failureExitCode -eq 0 -or
            $failure -notmatch $case.ExpectedError -or
            (Test-Path -LiteralPath $invalidParent) -or
            (Get-FileHash -LiteralPath $publisherSentinel `
                -Algorithm SHA256).Hash -ne $sentinelHash) {
            $closureFailures.Add(
                "$($case.Id): exit=$failureExitCode; output=$failure")
        }
    }

    $nvidiaIdentityPath = Join-Path $nvidia "xen-build-identity.json"
    $nvidiaIdentityText = Get-Content -LiteralPath $nvidiaIdentityPath `
        -Raw -Encoding utf8
    $legacyIdentity = $nvidiaIdentityText | ConvertFrom-Json
    $legacyIdentity.PSObject.Properties.Remove("components")
    $legacyIdentity | ConvertTo-Json |
        Set-Content -LiteralPath $nvidiaIdentityPath -Encoding utf8
    $legacyParent = Join-Path $root "license-invalid-legacy-identity"
    $legacyResult = Invoke-Publisher @{
        NvidiaBuildDirectory = $nvidia
        DirectMlBuildDirectory = $directml
        OpenVinoBuildDirectory = $openvino
        ModelPath = $model
        LicenseEvidence = $licenseEvidence
        ToolFiles = $tool
        RepositoryRoot = $repository
        GitExecutable = $GitExecutable
        OutputDirectory = (Join-Path $legacyParent "Xen-release")
    }
    if ($legacyResult.ExitCode -eq 0 -or
        $legacyResult.Output -notmatch "components" -or
        (Test-Path -LiteralPath $legacyParent) -or
        (Get-FileHash -LiteralPath $publisherSentinel `
            -Algorithm SHA256).Hash -ne $sentinelHash) {
        $closureFailures.Add(
            "legacy-identity: exit=$($legacyResult.ExitCode); " +
            "output=$($legacyResult.Output)")
    }
    [IO.File]::WriteAllText(
        $nvidiaIdentityPath, $nvidiaIdentityText,
        [Text.UTF8Encoding]::new($false))

    $output = Join-Path $root "Xen-release"
    $result = Invoke-Publisher @{
        NvidiaBuildDirectory = $nvidia
        DirectMlBuildDirectory = $directml
        OpenVinoBuildDirectory = $openvino
        ModelPath = $model
        LicenseEvidence = $licenseEvidence
        ToolFiles = $tool
        RepositoryRoot = $repository
        GitExecutable = $GitExecutable
        OutputDirectory = $output
    }
    if ($result.ExitCode -ne 0) {
        throw "合法夹具未能生成统一发布包：$($result.Output)"
    }

    $manifest = Get-Content -LiteralPath (Join-Path $output "manifest.json") `
        -Encoding utf8 -Raw | ConvertFrom-Json
    $manifestEvidence = @($manifest.files | Where-Object {
        ([string]$_.path) -like 'licenses/*'
    })
    foreach ($relative in @($fixturePayload.Keys) + @(
            "tools/source/xen_source_context.exe", "tools/source/XenClockSource.exe",
            "tools/source/XenSender.exe", "tools/source/XenCaptureEvidence.exe",
            "tools/source/XenAutoStopCapture.exe")) {
        $record = @($manifest.files | Where-Object { $_.path -eq $relative })
        if ($record.Count -ne 1 -or -not (Test-Path -LiteralPath (Join-Path $output $relative) -PathType Leaf)) {
            throw "必需工具/资源没有唯一进入完整发布清单：$relative"
        }
    }
    foreach ($prefix in @("", "tools/source/", "runtimes/nvidia/", "runtimes/directml/", "runtimes/openvino/")) {
        foreach ($name in $fixtureCrtNames) {
            $record = @($manifest.files | Where-Object { $_.path -eq "$prefix$name" })
            if ($record.Count -ne 1) { throw "EXE 所在目录缺少同位 CRT：$prefix$name" }
        }
    }
    foreach ($entry in $manifest.files) {
        $relative = [string]$entry.path
        if ($relative -notlike "runtimes/*" -and
            (Split-Path -Leaf $relative) -match '^(onnxruntime|nvinfer|nvonnxparser|cudnn|cublas|cufft|cudart|DirectML|openvino|tbb12).*\.dll$') {
            throw "Provider 文件越出所属 runtime：$relative"
        }
    }
    foreach ($runtime in @("nvidia", "directml", "openvino")) {
        $relative = "runtimes/$runtime/xen_recoil_calibration.exe"
        $records = @($manifest.files | Where-Object { $_.path -eq $relative })
        $packed = Join-Path $output $relative
        if ($records.Count -ne 1 -or -not (Test-Path -LiteralPath $packed -PathType Leaf) -or
            $records[0].runtime -ne $runtime -or
            (Get-FileHash -LiteralPath $packed -Algorithm SHA256).Hash -ne $records[0].sha256 -or
            (Get-Content -LiteralPath $packed -Raw).Trim() -ne "calibration-$runtime") {
            throw "各runtime必须在Worker同目录包含对应校准工具及准确清单：$runtime"
        }
        $tunerPath = "runtimes/$runtime/xen_recoil_tuner.exe"
        $tunerRecords = @($manifest.files | Where-Object { $_.path -eq $tunerPath })
        if ($tunerRecords.Count -ne 1 -or $tunerRecords[0].runtime -ne $runtime -or
            (Get-FileHash -LiteralPath (Join-Path $output $tunerPath) -Algorithm SHA256).Hash -ne $tunerRecords[0].sha256) {
            throw "各 runtime 必须包含同构建调优工具及准确清单：$runtime"
        }
    }
    if ($manifest.schema -ne 1 -or $manifest.git_commit -ne $commit -or
        @($manifest.PSObject.Properties).Count -ne 5 -or
        @($manifest.runtimes).Count -ne 3 -or
        $manifestEvidence.Count -ne $requiredComponents.Count -or
        -not (Test-Path -LiteralPath (Join-Path $output "XenLauncher.exe")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "models/model.onnx")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "tools/acceptance/acceptance.ps1"))) {
        throw "统一发布包结构或清单内容不正确"
    }
    if (@($manifest.files | Where-Object { $_.path -eq "cache/model-workspace/settings.json" }).Count -ne 0 -or
        (Test-Path -LiteralPath (Join-Path $output "cache/model-workspace/settings.json"))) {
        throw "未提供可选 Workspace 设置时不得创建或登记设置文件"
    }
    $settingsPath = Join-Path $root "workspace-settings.json"
    $settingsText = '{"schema_version":1,"python_executable":"X:/keep-this-binding/venv/Scripts/python.exe","extension":{"preserve":true}}'
    Write-Utf8 $settingsPath $settingsText
    $settingsOutput = Join-Path $root "Xen-release-with-settings"
    $settingsArguments = @{
        NvidiaBuildDirectory = $nvidia
        DirectMlBuildDirectory = $directml
        OpenVinoBuildDirectory = $openvino
        ModelPath = $model
        LicenseEvidence = $licenseEvidence
        RepositoryRoot = $repository
        GitExecutable = $GitExecutable
        WorkspaceSettingsPath = $settingsPath
        OutputDirectory = $settingsOutput
    }
    $settingsResult = Invoke-Publisher $settingsArguments
    if ($settingsResult.ExitCode -ne 0) { throw "合法 Workspace 设置无法随包发布：$($settingsResult.Output)" }
    $settingsManifest = Get-Content -LiteralPath (Join-Path $settingsOutput "manifest.json") -Raw | ConvertFrom-Json
    $settingsRecords = @($settingsManifest.files | Where-Object { $_.path -eq "cache/model-workspace/settings.json" })
    $packedSettings = Join-Path $settingsOutput "cache/model-workspace/settings.json"
    $settingsHash = (Get-FileHash -LiteralPath $settingsPath -Algorithm SHA256).Hash
    if ($settingsRecords.Count -ne 1 -or $settingsRecords[0].sha256 -ne $settingsHash -or
        $settingsRecords[0].size -ne (Get-Item -LiteralPath $settingsPath).Length -or
        $settingsRecords[0].source -ine $settingsPath -or
        (Get-FileHash -LiteralPath $packedSettings -Algorithm SHA256).Hash -ne $settingsHash -or
        (Get-Content -LiteralPath $packedSettings -Raw).Trim() -cne $settingsText) {
        throw "Workspace 设置未按原始字节、来源及摘要精确收录，或路径被重写"
    }
    $invalidSettingsCases = @(
        @{ Id = "json"; Text = '{"schema_version":1,"private":WORKSPACE_SECRET_SENTINEL}'; Error = "有效 JSON" },
        @{ Id = "missing-schema"; Text = '{}'; Error = "schema_version=1" },
        @{ Id = "wrong-schema"; Text = '{"schema_version":2}'; Error = "schema_version=1" },
        @{ Id = "string-schema"; Text = '{"schema_version":"1"}'; Error = "schema_version=1" },
        @{ Id = "array"; Text = '[{"schema_version":1}]'; Error = "schema_version=1" }
    )
    foreach ($case in $invalidSettingsCases) {
        Write-Utf8 $settingsPath $case.Text
        $invalidParent = Join-Path $root "settings-invalid-$($case.Id)"
        $settingsArguments.OutputDirectory = Join-Path $invalidParent "Xen-release"
        $settingsResult = Invoke-Publisher $settingsArguments
        if ($settingsResult.ExitCode -eq 0 -or $settingsResult.Output -notmatch $case.Error -or
            $settingsResult.Output -match "WORKSPACE_SECRET_SENTINEL" -or
            (Test-Path -LiteralPath $invalidParent)) {
            throw "无效 Workspace 设置未在发布前拒绝，或内容被回显：$($case.Id)"
        }
    }
    $manifestComponents = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $manifestLicensePaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($evidence in $manifestEvidence) {
        $componentId = [string]$evidence.component_id
        $relativePath = [string]$evidence.path
        if ($requiredComponents -notcontains $componentId -or
            -not $manifestComponents.Add($componentId) -or
            -not $manifestLicensePaths.Add($relativePath) -or
            $relativePath -notmatch '^licenses/[^/\\]+$' -or
            -not (Test-Path -LiteralPath (Join-Path $output $relativePath) `
                -PathType Leaf)) {
            throw "许可证据清单未保持组件闭包或安全直接子路径"
        }
    }
    foreach ($component in $requiredComponents) {
        if (-not $manifestComponents.Contains($component)) {
            throw "许可证据清单缺少组件：$component"
        }
    }
    $incoming = Get-ChildItem -LiteralPath $root -Force |
        Where-Object { $_.Name -like ".Xen-release.incoming-*" }
    if (@($incoming).Count -ne 0) {
        throw "成功发布后仍残留 incoming 临时目录"
    }
    $manifestHash = (Get-FileHash -LiteralPath (Join-Path $output "manifest.json") `
        -Algorithm SHA256).Hash
    $overwriteResult = Invoke-Publisher @{
        NvidiaBuildDirectory = $nvidia
        DirectMlBuildDirectory = $directml
        OpenVinoBuildDirectory = $openvino
        ModelPath = $model
        LicenseEvidence = $licenseEvidence
        ToolFiles = $tool
        RepositoryRoot = $repository
        GitExecutable = $GitExecutable
        OutputDirectory = $output
    }
    if ($overwriteResult.ExitCode -eq 0 -or
        $overwriteResult.Output -notmatch "已存在|refus.*overwrite" -or
        (Get-FileHash -LiteralPath (Join-Path $output "manifest.json") `
            -Algorithm SHA256).Hash -ne $manifestHash) {
        throw "既有发布目录未保持拒绝覆盖且内容不变"
    }
    if ($closureFailures.Count -ne 0) {
        throw "许可证闭包负例未失败关闭：$($closureFailures -join ' | ')"
    }
    Write-Host "统一发布包原子组装、三运行时隔离和哈希清单测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-XenOwnedTestDirectory -RootPath $root `
            -BasePath $ownedTest.BasePath `
            -RepositoryRoot $repositoryRoot `
            -OwnerId $ownedTest.OwnerId
    }
}
