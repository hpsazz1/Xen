param(
    [Parameter(Mandatory = $true)]
    [string]$ModernRawModelPath,
    [Parameter(Mandatory = $true)]
    [string]$YoloV5ObjectnessModelPath,
    [Parameter(Mandatory = $true)]
    [string]$EndToEndModelPath,
    [Parameter(Mandatory = $true)]
    [string]$ComparisonImagePath,
    [string]$OnnxRuntimeRoot = $env:ONNXRUNTIME_ROOT,
    [string]$OpenCvDir = $env:OpenCV_DIR,
    [string]$BuildDirectory = "",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-compatibility"
}
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $PSScriptRoot `
        ("..\cache\compatibility\detector-models-{0}.json" -f `
            (Get-Date -Format "yyyyMMdd-HHmmss"))
}

function Resolve-InputFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不存在：$Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Get-FileRecord {
    param([Parameter(Mandatory = $true)][string]$Path)

    $file = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $file.FullName
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
    }
}

function Publish-FileAtomically {
    param(
        [Parameter(Mandatory = $true)]
        [string]$TemporaryPath,
        [Parameter(Mandatory = $true)]
        [string]$FinalPath
    )

    if ($null -eq ("XenCompatibilityAtomicFile" -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;

public static class XenCompatibilityAtomicFile
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileExW(
        string existingFileName, string newFileName, int flags);
}
'@
    }

    # MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH。源和目标位于同一目录，
    # 因此读者只会看到完整旧报告或完整新报告，不会看到半份 JSON。
    if (-not [XenCompatibilityAtomicFile]::MoveFileExW(
            $TemporaryPath, $FinalPath, 0x00000009)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "无法原子发布兼容矩阵报告，Win32 error=$errorCode"
    }
}

function Invoke-DetectorContractTest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $true)]
        [string]$ModelPath,
        [Parameter(Mandatory = $true)]
        [string]$Contract,
        [string]$ComparisonImagePath = ""
    )

    Write-Host "验证 Detector 输出契约：$Contract"
    # Windows PowerShell 会把原生程序的 stderr 包装成错误记录。测试成败只按
    # 退出码判断，同时保留完整输出用于解析真实 Provider、输入尺寸和变化输入指纹。
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $arguments = @($ModelPath, "cpu", "--output-format", $Contract)
        if (-not [string]::IsNullOrWhiteSpace($ComparisonImagePath)) {
            $arguments += @("--comparison-image", $ComparisonImagePath)
        }
        $lines = @(& $Executable @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    foreach ($line in $lines) {
        Write-Host ([string]$line)
    }
    if ($exitCode -ne 0) {
        throw "Detector 契约 $Contract 验证失败，退出码：$exitCode"
    }

    $output = $lines -join "`n"
    $match = [regex]::Match(
        $output,
        'input=(\d+)x(\d+), provider=([^,\r\n]+), output_format=([^,\r\n]+).*black_fingerprint=(\d+), comparison_fingerprint=(\d+)',
        [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success) {
        throw "Detector 契约 $Contract 的输出无法解析。"
    }

    $provider = $match.Groups[3].Value.Trim()
    $reportedContract = $match.Groups[4].Value.Trim()
    $blackFingerprint = [uint64]$match.Groups[5].Value
    $comparisonFingerprint = [uint64]$match.Groups[6].Value
    if ($provider -ne "CPUExecutionProvider") {
        throw "Detector 契约 $Contract 未实际使用 CPU：$provider"
    }
    if ($reportedContract -ne $Contract) {
        throw "Detector 输出契约不一致：请求 $Contract，实际 $reportedContract"
    }
    if ($blackFingerprint -eq 0 -or $comparisonFingerprint -eq 0 -or
        $blackFingerprint -eq $comparisonFingerprint) {
        throw "Detector 契约 $Contract 的变化输入输出指纹未发生有效变化。"
    }

    $fileRecord = Get-FileRecord $ModelPath
    $fileRecord.input_width = [int]$match.Groups[1].Value
    $fileRecord.input_height = [int]$match.Groups[2].Value
    $fileRecord.output_format = $reportedContract
    $fileRecord.provider = $provider
    $fileRecord.black_fingerprint = [string]$blackFingerprint
    $fileRecord.comparison_fingerprint = [string]$comparisonFingerprint
    $fileRecord.comparison_source = if (
        [string]::IsNullOrWhiteSpace($ComparisonImagePath)) {
        "white"
    } else {
        $ComparisonImagePath
    }
    return $fileRecord
}

$ModernRawModelPath = Resolve-InputFile `
    $ModernRawModelPath "现代 Ultralytics raw 模型"
$YoloV5ObjectnessModelPath = Resolve-InputFile `
    $YoloV5ObjectnessModelPath "YOLOv5 objectness 模型"
$EndToEndModelPath = Resolve-InputFile `
    $EndToEndModelPath "端到端 NMS 模型"
$ComparisonImagePath = Resolve-InputFile `
    $ComparisonImagePath "端到端变化输入对照图像"
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)

$modelSnapshots = @(
    Get-FileRecord $ModernRawModelPath
    Get-FileRecord $YoloV5ObjectnessModelPath
    Get-FileRecord $EndToEndModelPath
)
$comparisonImageSnapshot = Get-FileRecord $ComparisonImagePath

# 兼容矩阵固定使用 CPU EP，隔离 TensorRT、CUDA、DirectML 与 NDI SDK，避免可选
# SDK 改变构建语义。XEN_TEST_MODEL 使用现代 raw 模型额外覆盖 AUTO 默认识别。
& (Join-Path $PSScriptRoot "build.ps1") `
    -BuildDirectory $BuildDirectory `
    -OnnxRuntimeRoot $OnnxRuntimeRoot `
    -OpenCvDir $OpenCvDir `
    -TensorRtRoot "" `
    -TensorRtMajor 0 `
    -CudnnRoot "" `
    -CudaRoot "" `
    -DirectMlRoot "" `
    -NdiSdkRoot "" `
    -ModelPath $ModernRawModelPath `
    -Configuration "Release"
if ($LASTEXITCODE -ne 0) {
    throw "Detector 兼容矩阵 Release 构建或基础测试失败，退出码：$LASTEXITCODE"
}

$testExecutable = Join-Path $BuildDirectory `
    "Release\detector_model_test.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "Detector 真实模型测试程序不存在：$testExecutable"
}

$executableSnapshot = Get-FileRecord $testExecutable
$outputDirectory = Split-Path -Parent $testExecutable
$runtimeFiles = @(Get-ChildItem -LiteralPath $outputDirectory -File |
    Where-Object {
        $_.Name -like "onnxruntime*.dll" -or
        $_.Name -like "opencv_world*.dll"
    } | Sort-Object Name)
if ($runtimeFiles.Count -eq 0) {
    throw "Detector 测试输出目录缺少 ONNX Runtime/OpenCV 运行库。"
}
$runtimeRecords = @($runtimeFiles | ForEach-Object {
    Get-FileRecord $_.FullName
})

$originalPath = $env:PATH
try {
    # 仅保留系统目录，第三方 DLL 必须从测试程序目录加载，防止开发机 PATH
    # 掩盖运行库部署缺失或加载到另一套 ONNX Runtime。
    $env:PATH = @(
        (Join-Path $env:SystemRoot "System32"),
        $env:SystemRoot
    ) -join ";"
    $results = @(
        Invoke-DetectorContractTest $testExecutable `
            $ModernRawModelPath "channel_first"
        Invoke-DetectorContractTest $testExecutable `
            $YoloV5ObjectnessModelPath "objectness"
        Invoke-DetectorContractTest $testExecutable `
            $EndToEndModelPath "end_to_end" $ComparisonImagePath
    )
} finally {
    $env:PATH = $originalPath
}
$currentComparisonImage = Get-FileRecord $ComparisonImagePath
if ($currentComparisonImage.sha256 -ne $comparisonImageSnapshot.sha256 -or
    $currentComparisonImage.bytes -ne $comparisonImageSnapshot.bytes) {
    throw "兼容矩阵运行期间对照图像发生变化：$ComparisonImagePath"
}

if ($results.Count -ne 3) {
    throw "Detector 兼容矩阵结果数量错误：$($results.Count)"
}
for ($index = 0; $index -lt $modelSnapshots.Count; ++$index) {
    $current = Get-FileRecord $modelSnapshots[$index].path
    if ($current.sha256 -ne $modelSnapshots[$index].sha256 -or
        $current.bytes -ne $modelSnapshots[$index].bytes) {
        throw "兼容矩阵运行期间模型发生变化：$($current.path)"
    }
}
$currentExecutable = Get-FileRecord $testExecutable
if ($currentExecutable.sha256 -ne $executableSnapshot.sha256 -or
    $currentExecutable.bytes -ne $executableSnapshot.bytes) {
    throw "兼容矩阵运行期间测试程序发生变化：$testExecutable"
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$gitCommit = $null
$gitDirty = $null
$gitCommand = Get-Command "git.exe" -ErrorAction SilentlyContinue
if ($null -ne $gitCommand) {
    $gitCommit = (& $gitCommand.Source -C $repositoryRoot rev-parse HEAD).Trim()
    $gitDirty = @(& $gitCommand.Source -C $repositoryRoot `
        status --porcelain --untracked-files=normal).Count -gt 0
}

$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$temporaryReportPath = Join-Path $reportDirectory `
    ((Split-Path -Leaf $ReportPath) + ".tmp-" +
        [guid]::NewGuid().ToString("N"))
try {
    $report = [ordered]@{
        schema_version = 1
        generated_utc = [DateTime]::UtcNow.ToString("o")
        source = [ordered]@{
            repository = $repositoryRoot
            git_commit = $gitCommit
            git_dirty = $gitDirty
        }
        build = [ordered]@{
            configuration = "Release"
            directory = $BuildDirectory
            executable = $executableSnapshot
            deployed_runtimes = $runtimeRecords
        }
        models = $results
        input = [ordered]@{
            end_to_end_comparison_image = $comparisonImageSnapshot
        }
        validation = [ordered]@{
            modern_raw_auto_ctest = $true
            explicit_output_contracts = $true
            changing_input_fingerprints = $true
            clean_runtime_path = $true
            inputs_unchanged_during_run = $true
        }
    }
    $report | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $temporaryReportPath -Encoding UTF8

    # 临时文件和最终文件位于同一目录；只在三种模型全部通过且文件已关闭后发布。
    Publish-FileAtomically $temporaryReportPath $ReportPath
} finally {
    if (Test-Path -LiteralPath $temporaryReportPath -PathType Leaf) {
        Remove-Item -LiteralPath $temporaryReportPath -Force
    }
}

Write-Host "Detector 真实模型兼容矩阵通过：$ReportPath"
