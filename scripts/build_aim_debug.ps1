param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-release-096e1a7-nvidia"),
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$Target = "xen_app"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Read-Json([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不存在：$Path"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw -Encoding utf8 |
            ConvertFrom-Json
    } catch {
        throw "$Description 不是有效 JSON：$Path；$($_.Exception.Message)"
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory -ErrorAction Stop).Path
$identity = Read-Json (Join-Path $buildRoot "xen-build-identity.json") "构建身份"
$commit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "无法读取当前 Git 提交。"
}
if ($identity.runtime -ne "nvidia") {
    throw "轻量 Aim 调试只允许 NVIDIA 构建目录，实际 runtime=$($identity.runtime)。"
}
if ($identity.git_commit -ne $commit) {
    throw "构建身份提交与当前源码不一致；请先重新配置 CMake：$($identity.git_commit) != $commit"
}

$outputDirectory = Join-Path $buildRoot $Configuration
$deploymentPath = Join-Path $outputDirectory "xen-runtime-deployment.json"
$deployment = Read-Json $deploymentPath "运行库部署报告"
if ($deployment.configuration -ne $Configuration -or
    [System.IO.Path]::GetFullPath([string]$deployment.output_directory) -ne
    [System.IO.Path]::GetFullPath($outputDirectory)) {
    throw "运行库部署报告未绑定当前 NVIDIA 输出目录。"
}
$runtimeNames = @($deployment.files | ForEach-Object { [string]$_.name })
foreach ($required in @(
        "onnxruntime.dll", "onnxruntime_providers_cuda.dll",
        "onnxruntime_providers_tensorrt.dll", "nvinfer_10.dll",
        "Processing.NDI.Lib.x64.dll")) {
    if ($runtimeNames -notcontains $required -or
        -not (Test-Path -LiteralPath (Join-Path $outputDirectory $required) -PathType Leaf)) {
        throw "NVIDIA/NDI 固定运行时缺少：$required"
    }
}

Write-Host "轻量 Aim 调试构建：runtime=nvidia target=$Target config=$Configuration"
Write-Host "固定运行时目录：$outputDirectory"
& cmake --build $buildRoot --config $Configuration --target $Target --parallel
if ($LASTEXITCODE -ne 0) {
    throw "轻量 Aim 调试构建失败，退出码：$LASTEXITCODE"
}

$worker = Join-Path $outputDirectory "Xen.exe"
if (-not (Test-Path -LiteralPath $worker -PathType Leaf)) {
    throw "轻量构建未生成 Xen.exe：$worker"
}
$workerHash = (Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash
Write-Host "轻量构建完成：$worker"
Write-Host "Xen.exe SHA-256：$workerHash"
Write-Host "Provider/NDI DLL 未重建，继续复用固定输出目录中的已校验闭包。"
