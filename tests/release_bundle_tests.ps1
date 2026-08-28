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

function Write-Utf8([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Set-Content -LiteralPath $Path -Value $Content -Encoding utf8
}

function New-FakeBuild(
        [string]$Root,
        [string]$Runtime,
        [string]$Commit,
        [string[]]$RuntimeFiles) {
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
    } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $Root "xen-build-identity.json") -Encoding utf8
}

$root = [IO.Path]::GetFullPath($TestRoot)
if ($root -match '^[A-Za-z]:\\?$' -or $root -eq '\') {
    throw "拒绝在文件系统根目录执行发布夹具测试"
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

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
    $license = Join-Path $root "LICENSE.txt"
    $tool = Join-Path $root "acceptance.ps1"
    Write-Utf8 $model "model"
    Write-Utf8 $license "license"
    Write-Utf8 $tool "Write-Host acceptance"
    $nvidia = Join-Path $root "build-nvidia"
    $directml = Join-Path $root "build-directml"
    $openvino = Join-Path $root "build-openvino"
    New-FakeBuild $nvidia "nvidia" $commit @(
        "onnxruntime.dll", "onnxruntime_providers_cuda.dll",
        "onnxruntime_providers_tensorrt.dll", "nvinfer_10.dll")
    New-FakeBuild $directml "directml" $commit @(
        "onnxruntime.dll", "DirectML.dll")
    New-FakeBuild $openvino "openvino" $commit @(
        "onnxruntime.dll", "onnxruntime_providers_openvino.dll", "openvino.dll")

    $output = Join-Path $root "Xen-release"
    $windowsPowerShell = Join-Path $env:SystemRoot `
        "System32\WindowsPowerShell\v1.0\powershell.exe"
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $PublishScript `
        -NvidiaBuildDirectory $nvidia `
        -DirectMlBuildDirectory $directml `
        -OpenVinoBuildDirectory $openvino `
        -ModelPath $model `
        -LicenseFiles $license `
        -ToolFiles $tool `
        -RepositoryRoot $repository `
        -GitExecutable $GitExecutable `
        -OutputDirectory $output
    if ($LASTEXITCODE -ne 0) { throw "合法夹具未能生成统一发布包" }

    $manifest = Get-Content -LiteralPath (Join-Path $output "manifest.json") `
        -Encoding utf8 -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.git_commit -ne $commit -or
        @($manifest.runtimes).Count -ne 3 -or
        -not (Test-Path -LiteralPath (Join-Path $output "XenLauncher.exe")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "models/model.onnx")) -or
        -not (Test-Path -LiteralPath (Join-Path $output "tools/acceptance.ps1"))) {
        throw "统一发布包结构或清单内容不正确"
    }
    $incoming = Get-ChildItem -LiteralPath $root -Force |
        Where-Object { $_.Name -like ".Xen-release.incoming-*" }
    if (@($incoming).Count -ne 0) {
        throw "成功发布后仍残留 incoming 临时目录"
    }
    Write-Host "统一发布包原子组装、三运行时隔离和哈希清单测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
