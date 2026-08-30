param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$testRootBase = [IO.Path]::GetFullPath($TestRoot)
if ($testRootBase -match '^[A-Za-z]:\\?$' -or
    $testRootBase -eq '\' -or $testRootBase.StartsWith('\\') -or
    (Split-Path -Leaf $testRootBase) -notlike "sender-cli-*") {
    throw "Sender CLI 测试目录必须是 sender-cli-* 专用子目录"
}
if (Test-Path -LiteralPath $testRootBase) {
    if (-not (Test-Path -LiteralPath $testRootBase -PathType Container)) {
        throw "Sender CLI 测试根必须是目录"
    }
} else {
    New-Item -ItemType Directory -Path $testRootBase -Force | Out-Null
}
$root = Join-Path $testRootBase (
    "run-$PID-$([guid]::NewGuid().ToString('N'))")
if (Test-Path -LiteralPath $root) {
    throw "Sender CLI 本轮唯一测试目录已存在"
}
New-Item -ItemType Directory -Path $root | Out-Null

function Invoke-SenderReport(
        [string]$ReportPath,
        [uint32]$Fps,
        [uint64]$MaximumSeconds,
        [uint64]$MaximumFrames) {
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $Executable `
            --destination "udp://127.0.0.1:9" `
            --fps $Fps `
            --max-seconds $MaximumSeconds `
            --max-frames $MaximumFrames `
            --report $ReportPath 2>&1) -join [Environment]::NewLine
        return [ordered]@{
            exit_code = $LASTEXITCODE
            output = $output
        }
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
}

function Assert-UsageCapacityFailure([System.Collections.IDictionary]$Result) {
    if ($Result.exit_code -ne 1 -or
        $Result.output -notmatch "参数错误" -or
        $Result.output -notmatch "用法" -or
        $Result.output -notmatch "200000") {
        throw "超容量报告组合必须以用法错误拒绝；实际退出码=$($Result.exit_code)"
    }
}

try {
    # 已存在目标是 red 阶段的安全保险：旧实现会在 Capture 之前以 exit 6
    # 拒绝覆盖，因此本测试即使失败也不会启动 DXGI 或打开网络 Sender。
    $existingReport = Join-Path $root "existing.sender.json"
    $sentinel = [byte[]](0x58, 0x65, 0x6e, 0x0a)
    [IO.File]::WriteAllBytes($existingReport, $sentinel)
    Push-Location $root
    try {
        $existingResult = Invoke-SenderReport $existingReport 240 834 0
        $unboundedResult = Invoke-SenderReport $existingReport 240 0 0
        $atCapacityResult = Invoke-SenderReport $existingReport 200000 1 0
        $frameBoundedResult = Invoke-SenderReport $existingReport 240 834 1
        $oversizedFrameBoundResult = Invoke-SenderReport `
            $existingReport 240 834 200001
        $oversizedUnboundedFrameResult = Invoke-SenderReport `
            $existingReport 240 0 200001
    } finally {
        Pop-Location
    }
    Assert-UsageCapacityFailure $existingResult
    Assert-UsageCapacityFailure $unboundedResult
    Assert-UsageCapacityFailure $oversizedFrameBoundResult
    Assert-UsageCapacityFailure $oversizedUnboundedFrameResult
    if ($atCapacityResult.exit_code -ne 6 -or
        $atCapacityResult.output -notmatch "报告目标已存在" -or
        $frameBoundedResult.exit_code -ne 6 -or
        $frameBoundedResult.output -notmatch "报告目标已存在" -or
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($existingReport)) -ne
        [Convert]::ToBase64String($sentinel) -or
        @(Get-ChildItem -LiteralPath $root -Filter "*.pending-*" -File).Count -ne 0) {
        throw "容量边界必须保留 no-overwrite，且不得改写报告或遗留 pending"
    }

    # 第一阶段通过后才验证不存在的报告父目录，避免 red 阶段触达 Capture。
    $isolatedWorkingDirectory = Join-Path $root "isolated-cwd"
    New-Item -ItemType Directory -Path $isolatedWorkingDirectory | Out-Null
    $missingParent = Join-Path $isolatedWorkingDirectory "must-not-exist"
    $missingReport = Join-Path $missingParent "sender.json"
    Push-Location $isolatedWorkingDirectory
    try {
        $missingResult = Invoke-SenderReport $missingReport 240 834 0
    } finally {
        Pop-Location
    }
    Assert-UsageCapacityFailure $missingResult
    if ((Test-Path -LiteralPath $missingParent) -or
        @(Get-ChildItem -LiteralPath $isolatedWorkingDirectory -Force).Count -ne 0) {
        throw "超容量组合必须在日志、Capture 和报告目录副作用前拒绝"
    }

    Write-Host "Sender CLI 报告容量前置门禁测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
