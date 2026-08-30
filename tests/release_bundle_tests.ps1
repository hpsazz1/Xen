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
        [string[]]$Components) {
    $release = Join-Path $Root "Release"
    New-Item -ItemType Directory -Path $release -Force | Out-Null
    Write-Utf8 (Join-Path $release "Xen.exe") "worker-$Runtime"
    Write-Utf8 (Join-Path $release "XenLauncher.exe") "launcher"
    $files = @()
    foreach ($name in $RuntimeFiles) {
        $path = Join-Path $release $name
        Write-Utf8 $path "runtime-$Runtime-$name"
        $files += [ordered]@{
            name = $name
            source = "fixture/$Runtime/$name"
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
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $repositoryRoot
$root = $ownedTest.RootPath

try {
    $repository = Join-Path $root "repo"
    New-Item -ItemType Directory -Path $repository | Out-Null
    Write-Utf8 (Join-Path $repository "tracked.txt") "fixture"
    & $GitExecutable -C $repository init --quiet
    & $GitExecutable -C $repository config user.email "xen-release-test@example.invalid"
    & $GitExecutable -C $repository config user.name "Xen Release Test"
    & $GitExecutable -C $repository add tracked.txt
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
    $commonComponents = @(
        "imgui", "nlohmann-json", "ndi", "onnxruntime", "opencv",
        "simpleini", "spdlog")
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
        $path = Join-Path $root "license-$component.txt"
        Write-Utf8 $path "license evidence for $component"
        $licenseEvidenceByComponent[$component] = "$component=$path"
    }
    $licenseEvidence = @($licenseEvidenceByComponent.Values)

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
    if ($manifest.schema -ne 1 -or $manifest.git_commit -ne $commit -or
        @($manifest.PSObject.Properties).Count -ne 5 -or
        @($manifest.runtimes).Count -ne 3 -or
        $manifestEvidence.Count -ne $requiredComponents.Count -or
        -not (Test-Path -LiteralPath (Join-Path $output "XenLauncher.exe")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "models/model.onnx")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "tools/acceptance.ps1"))) {
        throw "统一发布包结构或清单内容不正确"
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
