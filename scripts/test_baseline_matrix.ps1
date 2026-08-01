param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,
    [Parameter(Mandatory = $true)]
    [string]$CpuOnnxRuntimeRoot,
    [Parameter(Mandatory = $true)]
    [string]$GpuOnnxRuntimeRoot,
    [Parameter(Mandatory = $true)]
    [string]$DirectMlOnnxRuntimeRoot,
    [Parameter(Mandatory = $true)]
    [string]$OpenVinoOnnxRuntimeRoot,
    [Parameter(Mandatory = $true)]
    [string]$OpenCvDir,
    [Parameter(Mandatory = $true)]
    [string]$TensorRtRoot,
    [Parameter(Mandatory = $true)]
    [string]$CudnnRoot,
    [Parameter(Mandatory = $true)]
    [string]$CudaRoot,
    [Parameter(Mandatory = $true)]
    [string]$DirectMlRoot,
    [Parameter(Mandatory = $true)]
    [string]$NdiSdkRoot,
    [string]$BuildRoot = "",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $PSScriptRoot "..\build-matrix"
}
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $PSScriptRoot "..\cache\matrix\baseline.json"
}

function Resolve-RequiredPath {
    param([string]$Path, [string]$Description, [string]$PathType = "Container")
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Get-FileEvidence {
    param([string]$Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        bytes = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}

function Invoke-CleanExecutable {
    param([string]$Executable, [string[]]$Arguments)
    $originalPath = $env:PATH
    try {
        $env:PATH = @(
            (Join-Path $env:SystemRoot "System32"),
            $env:SystemRoot
        ) -join ";"
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $output = @(& $Executable @Arguments 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
    } finally {
        $env:PATH = $originalPath
    }
    $output | ForEach-Object { Write-Host ([string]$_) }
    if ($exitCode -ne 0) {
        throw "clean PATH 命令失败，退出码=${exitCode}：$Executable"
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Get-ProviderProfileSummary {
    param(
        [string]$ProfilePath,
        [string]$ExpectedProvider,
        [string[]]$AllowedProviders
    )
    # Windows PowerShell 5 把 JSON 顶层数组作为单个对象返回，必须先接住
    # ConvertFrom-Json 结果再展开，否则整个事件数组会被误当成一个事件。
    $document = Get-Content -LiteralPath $ProfilePath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $events = @($document)
    $counts = [ordered]@{}
    foreach ($event in $events) {
        if ([string]$event.cat -ne "Node" -or $null -eq $event.args) {
            continue
        }
        $property = $event.args.PSObject.Properties["provider"]
        if ($null -eq $property -or
            [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            continue
        }
        $provider = [string]$property.Value
        if (-not $counts.Contains($provider)) { $counts[$provider] = 0 }
        $counts[$provider] = [long]$counts[$provider] + 1
    }
    if ($counts.Count -eq 0) { throw "ORT profile 没有 Provider 节点：$ProfilePath" }
    if (-not $counts.Contains($ExpectedProvider) -or
        [long]$counts[$ExpectedProvider] -le 0) {
        throw "ORT profile 没有请求的 Provider 节点：$ExpectedProvider"
    }
    $unexpected = @($counts.Keys | Where-Object { $AllowedProviders -notcontains $_ })
    if ($unexpected.Count -ne 0) {
        throw "ORT profile 出现未授权 Provider：$($unexpected -join ', ')"
    }
    return [ordered]@{
        file = Get-FileEvidence $ProfilePath
        provider_counts = $counts
    }
}

function Get-CTestCount {
    param([string]$BuildDirectory)
    $output = @(& ctest.exe --test-dir $BuildDirectory -C Release -N 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "无法读取 CTest 清单：$BuildDirectory"
    }
    $line = @($output | ForEach-Object { [string]$_ } |
        Where-Object { $_ -match '^Total Tests: ([0-9]+)$' })
    if ($line.Count -ne 1) { throw "CTest 清单缺少测试总数：$BuildDirectory" }
    return [int]([regex]::Match($line[0], '([0-9]+)$').Groups[1].Value)
}

function Publish-FileAtomically {
    param([string]$TemporaryPath, [string]$FinalPath)
    if ($null -eq ("XenMatrixAtomicFile" -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;

public static class XenMatrixAtomicFile
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileExW(
        string existingFileName, string newFileName, int flags);
}
'@
    }
    # MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH。临时文件与最终
    # 报告位于同一目录，失败时不会留下可误认成完整矩阵的半份 JSON。
    if (-not [XenMatrixAtomicFile]::MoveFileExW(
            $TemporaryPath, $FinalPath, 0x00000009)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "矩阵报告原子发布失败，Win32 error=$errorCode"
    }
}

function Invoke-Build {
    param(
        [string]$Name,
        [string]$BuildDirectory,
        [string]$OnnxRuntimeRoot,
        [string]$TestModel,
        [string]$TensorRt = "",
        [string]$Cudnn = "",
        [string]$Cuda = "",
        [string]$DirectMl = "",
        [string]$Ndi = "",
        [string]$OpenVinoDevice = ""
    )
    Write-Host "===== 构建矩阵：$Name ====="
    & (Join-Path $PSScriptRoot "build.ps1") `
        -BuildDirectory $BuildDirectory `
        -OnnxRuntimeRoot $OnnxRuntimeRoot `
        -OpenCvDir $OpenCvDir `
        -TensorRtRoot $TensorRt `
        -CudnnRoot $Cudnn `
        -CudaRoot $Cuda `
        -DirectMlRoot $DirectMl `
        -NdiSdkRoot $Ndi `
        -ModelPath $TestModel `
        -OpenVinoTestDevice $OpenVinoDevice `
        -Configuration Release
}

function Invoke-ProviderCase {
    param(
        [string]$Name,
        [string]$BuildDirectory,
        [string]$Backend,
        [string]$ExpectedProvider,
        [string[]]$AllowedProviders,
        [string]$CachePath = ""
    )
    $outputDirectory = Join-Path $BuildDirectory "Release"
    $executable = Join-Path $outputDirectory "detector_model_test.exe"
    $profileDirectory = Join-Path $BuildDirectory "profiles"
    New-Item -ItemType Directory -Path $profileDirectory -Force | Out-Null
    $profilePrefix = Join-Path $profileDirectory "$Name-ort"
    $arguments = @($ModelPath, $Backend)
    if (-not [string]::IsNullOrWhiteSpace($CachePath)) { $arguments += $CachePath }
    $arguments += @("--provider-profile-prefix", $profilePrefix)
    $output = Invoke-CleanExecutable $executable $arguments
    $profileLine = @($output | Where-Object { $_.StartsWith("provider_profile=") })
    if ($profileLine.Count -ne 1) { throw "$Name 没有输出唯一 Provider profile" }
    $profilePath = $profileLine[0].Substring("provider_profile=".Length)
    $summaryLine = @($output | Where-Object {
        $_.StartsWith("真实模型测试通过：")
    })
    if ($summaryLine.Count -ne 1) { throw "$Name 没有输出唯一变化输入摘要" }
    $fingerprints = [regex]::Match(
        $summaryLine[0],
        'black_fingerprint=([0-9]+), comparison_fingerprint=([0-9]+)')
    if (-not $fingerprints.Success -or
        $fingerprints.Groups[1].Value -eq $fingerprints.Groups[2].Value) {
        throw "$Name 没有输出有效的变化输入指纹"
    }
    $deploymentPath = Join-Path $outputDirectory "xen-runtime-deployment.json"
    return [ordered]@{
        backend = $Backend
        ctest_count = Get-CTestCount $BuildDirectory
        fingerprints = @(
            $fingerprints.Groups[1].Value,
            $fingerprints.Groups[2].Value)
        executable = Get-FileEvidence $executable
        deployment = Get-FileEvidence $deploymentPath
        profile = Get-ProviderProfileSummary `
            $profilePath $ExpectedProvider $AllowedProviders
    }
}

$ModelPath = Resolve-RequiredPath $ModelPath "固定检测模型" "Leaf"
$CpuOnnxRuntimeRoot = Resolve-RequiredPath $CpuOnnxRuntimeRoot "CPU ORT"
$GpuOnnxRuntimeRoot = Resolve-RequiredPath $GpuOnnxRuntimeRoot "GPU ORT"
$DirectMlOnnxRuntimeRoot = Resolve-RequiredPath $DirectMlOnnxRuntimeRoot "DirectML ORT"
$OpenVinoOnnxRuntimeRoot = Resolve-RequiredPath $OpenVinoOnnxRuntimeRoot "OpenVINO ORT"
$OpenCvDir = Resolve-RequiredPath $OpenCvDir "OpenCV 配置目录"
$TensorRtRoot = Resolve-RequiredPath $TensorRtRoot "TensorRT SDK"
$CudnnRoot = Resolve-RequiredPath $CudnnRoot "cuDNN SDK"
$CudaRoot = Resolve-RequiredPath $CudaRoot "CUDA Toolkit"
$DirectMlRoot = Resolve-RequiredPath $DirectMlRoot "DirectML 可再发行包"
$NdiSdkRoot = Resolve-RequiredPath $NdiSdkRoot "NDI SDK"
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$modelSnapshot = Get-FileEvidence $ModelPath

$cpuBuild = "$BuildRoot-cpu"
$cudaBuild = "$BuildRoot-cuda"
$tensorrtBuild = "$BuildRoot-tensorrt"
$directmlBuild = "$BuildRoot-directml"
$openvinoBuild = "$BuildRoot-openvino"
$ndiBuild = "$BuildRoot-ndi"
$noNdiBuild = "$BuildRoot-no-ndi"
$matrixBuildDirectories = @(
    $cpuBuild, $cudaBuild, $tensorrtBuild, $directmlBuild,
    $openvinoBuild, $ndiBuild, $noNdiBuild)
foreach ($directory in $matrixBuildDirectories) {
    if (Test-Path -LiteralPath $directory) {
        throw "矩阵要求独立干净目录，目标已存在：$directory"
    }
}

$results = [ordered]@{}
Invoke-Build "CPU" $cpuBuild $CpuOnnxRuntimeRoot $ModelPath
$results.cpu = Invoke-ProviderCase "cpu" $cpuBuild "cpu" `
    "CPUExecutionProvider" @("CPUExecutionProvider")

Invoke-Build "CUDA" $cudaBuild $GpuOnnxRuntimeRoot $ModelPath `
    -Cudnn $CudnnRoot -Cuda $CudaRoot
$results.cuda = Invoke-ProviderCase "cuda" $cudaBuild "cuda" `
    "CUDAExecutionProvider" `
    @("CUDAExecutionProvider", "CPUExecutionProvider")

Invoke-Build "TensorRT" $tensorrtBuild $GpuOnnxRuntimeRoot $ModelPath `
    -TensorRt $TensorRtRoot -Cudnn $CudnnRoot -Cuda $CudaRoot
$trtCache = Join-Path $tensorrtBuild "cache\tensorrt\matrix"
New-Item -ItemType Directory -Path $trtCache -Force | Out-Null
$results.tensorrt = Invoke-ProviderCase "tensorrt" $tensorrtBuild `
    "tensorrt" "TensorrtExecutionProvider" `
    @("TensorrtExecutionProvider", "CUDAExecutionProvider", "CPUExecutionProvider") $trtCache

Invoke-Build "DirectML" $directmlBuild $DirectMlOnnxRuntimeRoot $ModelPath `
    -DirectMl $DirectMlRoot
$results.directml = Invoke-ProviderCase "directml" $directmlBuild `
    "directml" "DmlExecutionProvider" @("DmlExecutionProvider")

Invoke-Build "OpenVINO CPU" $openvinoBuild $OpenVinoOnnxRuntimeRoot `
    $ModelPath -OpenVinoDevice "CPU"
$openvinoExecutable = Join-Path $openvinoBuild "Release\openvino_model_test.exe"
$openvinoPrefix = Join-Path $openvinoBuild "profiles\openvino-cpu"
New-Item -ItemType Directory -Path (Split-Path -Parent $openvinoPrefix) -Force | Out-Null
$openvinoOutput = Invoke-CleanExecutable $openvinoExecutable `
    @($ModelPath, "cpu", $openvinoPrefix)
$openvinoProfileLine = @($openvinoOutput | Where-Object { $_.StartsWith("provider_profile=") })
if ($openvinoProfileLine.Count -ne 1) { throw "OpenVINO 没有输出唯一 Provider profile" }
$openvinoProfile = $openvinoProfileLine[0].Substring("provider_profile=".Length)
$openvinoFingerprintLine = @($openvinoOutput | Where-Object {
    $_.StartsWith("provider=OpenVINOExecutionProvider")
})
if ($openvinoFingerprintLine.Count -ne 1) {
    throw "OpenVINO 没有输出唯一变化输入摘要"
}
$openvinoFingerprints = [regex]::Match(
    $openvinoFingerprintLine[0], 'fingerprints=([0-9]+),([0-9]+)')
if (-not $openvinoFingerprints.Success -or
    $openvinoFingerprints.Groups[1].Value -eq
        $openvinoFingerprints.Groups[2].Value) {
    throw "OpenVINO 没有输出有效的变化输入指纹"
}
$results.openvino = [ordered]@{
    backend = "openvino"
    ctest_count = Get-CTestCount $openvinoBuild
    fingerprints = @(
        $openvinoFingerprints.Groups[1].Value,
        $openvinoFingerprints.Groups[2].Value)
    executable = Get-FileEvidence $openvinoExecutable
    deployment = Get-FileEvidence (Join-Path $openvinoBuild "Release\xen-runtime-deployment.json")
    profile = Get-ProviderProfileSummary $openvinoProfile `
        "OpenVINOExecutionProvider" @("OpenVINOExecutionProvider")
}

Write-Host "===== 构建矩阵：NDI / 无 NDI ====="
& (Join-Path $PSScriptRoot "test_ndi.ps1") `
    -NdiBuildDirectory $ndiBuild `
    -NoNdiBuildDirectory $noNdiBuild `
    -OnnxRuntimeRoot $CpuOnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -NdiSdkRoot $NdiSdkRoot `
    -Configuration Release
$results.ndi = [ordered]@{
    with_sdk_ctest_count = Get-CTestCount $ndiBuild
    without_sdk_ctest_count = Get-CTestCount $noNdiBuild
    with_sdk = Get-FileEvidence (Join-Path $ndiBuild "Release\xen-runtime-deployment.json")
    without_sdk = Get-FileEvidence (Join-Path $noNdiBuild "Release\xen-runtime-deployment.json")
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$modelAfter = Get-FileEvidence $ModelPath
if ($modelAfter.sha256 -ne $modelSnapshot.sha256 -or
    $modelAfter.bytes -ne $modelSnapshot.bytes) {
    throw "矩阵运行期间真实模型输入发生变化"
}
$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString("o")
    source = [ordered]@{
        repository = $repositoryRoot
        git_commit = (& git.exe -C $repositoryRoot rev-parse HEAD).Trim()
        git_dirty = @(& git.exe -C $repositoryRoot status --porcelain --untracked-files=normal).Count -gt 0
    }
    inputs = [ordered]@{
        model = $modelSnapshot
    }
    results = $results
    validation = [ordered]@{
        release_builds = $true
        all_applicable_ctest = $true
        clean_path = $true
        changing_input_fingerprints = $true
        provider_nodes = $true
        deployment_sources_and_hashes = $true
        independent_build_directories = $true
        inputs_unchanged_during_run = $true
    }
}

$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$temporaryPath = "$ReportPath.tmp-$([guid]::NewGuid().ToString('N'))"
try {
    $report | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $temporaryPath -Encoding utf8
    Publish-FileAtomically $temporaryPath $ReportPath
} finally {
    if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }
}
Write-Host "统一 Provider/SDK Release 矩阵通过：$ReportPath"
