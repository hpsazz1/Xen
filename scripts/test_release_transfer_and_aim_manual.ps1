param(
    [string]$TestRoot = (Join-Path $PSScriptRoot `
        "..\cache\release-transfer-aim-manual-test")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Utf8([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText(
        $Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

function Add-ManifestFile(
        [System.Collections.Generic.List[object]]$Files,
        [string]$Root,
        [string]$RelativePath,
        [string]$Runtime = "") {
    $path = Join-Path $Root $RelativePath
    $file = Get-Item -LiteralPath $path
    $Files.Add([ordered]@{
        path = $RelativePath.Replace('\', '/')
        runtime = $Runtime
        size = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $path `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        source = "fixture/$RelativePath"
    })
}

$root = [System.IO.Path]::GetFullPath($TestRoot)
if ($root -match '^[A-Za-z]:\?$' -or $root -eq '\') {
    throw "拒绝在文件系统根目录运行测试。"
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

try {
    $package = Join-Path $root "Xen-fixture"
    foreach ($directory in @(
            "models", "runtimes\nvidia", "runtimes\directml",
            "runtimes\openvino", "tools", "cache", "logs", "licenses")) {
        New-Item -ItemType Directory -Path (Join-Path $package $directory) `
            -Force | Out-Null
    }
    Write-Utf8 (Join-Path $package "XenLauncher.exe") "launcher"
    Write-Utf8 (Join-Path $package "config.ini") "fixture-config"
    Write-Utf8 (Join-Path $package "models\14wv11.onnx") "model"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\Xen.exe") "nvidia"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\nvinfer_10.dll") "trt"
    Write-Utf8 (Join-Path $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll") "ndi"
    Write-Utf8 (Join-Path $package "runtimes\directml\Xen.exe") "dml"
    Write-Utf8 (Join-Path $package "runtimes\openvino\Xen.exe") "ov"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "aim_report.ps1") `
        -Destination (Join-Path $package "tools\aim_report.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "invoke_aim_manual_acceptance.ps1") `
        -Destination (Join-Path $package `
            "tools\invoke_aim_manual_acceptance.ps1")

    $files = [System.Collections.Generic.List[object]]::new()
    Add-ManifestFile $files $package "XenLauncher.exe"
    Add-ManifestFile $files $package "config.ini"
    Add-ManifestFile $files $package "models\14wv11.onnx"
    Add-ManifestFile $files $package "runtimes\nvidia\Xen.exe" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\nvinfer_10.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\directml\Xen.exe" "directml"
    Add-ManifestFile $files $package `
        "runtimes\openvino\Xen.exe" "openvino"
    Add-ManifestFile $files $package "tools\aim_report.ps1"
    Add-ManifestFile $files $package `
        "tools\invoke_aim_manual_acceptance.ps1"
    [ordered]@{
        schema = 1
        product = "Xen"
        git_commit = "1" * 40
        runtimes = @(
            [ordered]@{
                id = "nvidia"
                executable = "runtimes/nvidia/Xen.exe"
                backends = @("cpu", "cuda", "tensorrt")
            },
            [ordered]@{
                id = "directml"
                executable = "runtimes/directml/Xen.exe"
                backends = @("directml")
            },
            [ordered]@{
                id = "openvino"
                executable = "runtimes/openvino/Xen.exe"
                backends = @("openvino")
            }
        )
        files = @($files)
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $package "manifest.json") `
            -Encoding utf8

    $destination = Join-Path $root "remote"
    New-Item -ItemType Directory -Path $destination | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot "transfer_release_bundle.ps1") `
        -PackagePath $package -DestinationRoot $destination
    if ($LASTEXITCODE -ne 0) {
        throw "完整发布包传输夹具失败。"
    }
    $published = Join-Path $destination "releases\Xen-fixture"
    if (-not (Test-Path -LiteralPath $published -PathType Container) -or
        @(Get-ChildItem -LiteralPath (Join-Path $destination "releases") `
            -Force | Where-Object { $_.Name -like ".incoming-*" }).Count -ne 0) {
        throw "完整发布包传输未原子收口。"
    }

    Write-Utf8 (Join-Path $published "cache\tensorrt\fixture.engine") `
        "runtime provider cache"
    Write-Utf8 (Join-Path $published "logs\xen.log") "runtime log"

    $invalidMutablePrefix = Join-Path $published `
        "cache-shadow\unknown.dll"
    Write-Utf8 $invalidMutablePrefix "not mutable cache"
    $invalidRoot = Join-Path $root "invalid-prefix-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $invalidRoot
    if ($LASTEXITCODE -eq 0) {
        throw "cache 同名前缀目录不应绕过人工验收清单校验。"
    }
    Remove-Item -LiteralPath $invalidMutablePrefix -Force

    $trackingRoot = Join-Path $root "tracking-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $trackingRoot
    if ($LASTEXITCODE -ne 0) { throw "tracking 任务准备失败。" }
    $trackingConfig = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "config.ini") -Raw -Encoding utf8
    if ($trackingConfig -notmatch '(?m)^backend=tensorrt\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=ndi\r?$' -or
        $trackingConfig -notmatch '(?m)^enable_prediction=false\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=kmbox_net\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_ip=192\.168\.2\.188\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_port=13384\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_uuid=7679E04E\r?$' -or
        $trackingConfig -notmatch '(?m)^allow_send_input=true\r?$') {
        throw "tracking 配置没有固定 NDI、TensorRT 或 KMBOX 契约。"
    }

    $predictionRoot = Join-Path $root "prediction-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Prepare -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -ne 0) { throw "prediction 任务准备失败。" }
    $predictionConfig = Get-Content -LiteralPath `
        (Join-Path $predictionRoot "config.ini") -Raw -Encoding utf8
    if ($predictionConfig -notmatch '(?m)^enable_prediction=true\r?$' -or
        $predictionConfig -notmatch `
            '(?m)^max_prediction_lead_percent=35\.000000\r?$') {
        throw "prediction 配置没有启用已验证的预测边界。"
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Launch -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -eq 0) {
        throw "缺少物理输出双重授权时 Launch 不应成功。"
    }
    Write-Host "完整包传输、Aim 配置生成、清单更新和授权拒绝回归通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
