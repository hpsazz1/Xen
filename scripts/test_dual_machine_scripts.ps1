param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot "..")
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) { throw $Message }
}

$scriptPaths = @(
    "scripts/benchmark_runtime.ps1",
    "scripts/benchmark_network_receiver.ps1",
    "scripts/invoke_dual_machine_receiver.ps1",
    "scripts/publish_dual_machine_package.ps1",
    "scripts/runtime_environment.ps1"
) | ForEach-Object { Join-Path $RepositoryRoot $_ }

foreach ($path in $scriptPaths) {
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $path, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) `
        "PowerShell 语法解析失败：$path"
}

$networkReceiverText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/benchmark_network_receiver.ps1"))
foreach ($field in @(
    "source_dropped_frames",
    "transport_dropped_frames",
    "transport_invalid_packets",
    "runtime_overwritten_frames")) {
    Assert-True ($networkReceiverText -match
        [regex]::Escape("$field={")) `
        "网络门禁错误必须打印实际值和阈值：$field。"
}
Assert-True ($networkReceiverText -match
    'if \(-not \$runtimeReportPublished\)') `
    "网络门禁拒绝后必须保留已原子发布的 Runtime 报告。"
Assert-True ($networkReceiverText -match
    '(?s)throw \(\("网络门禁未通过.*"网络完成标记不发布。"\) -f') `
    "网络门禁文本必须先拼接完整模板，再应用格式参数。"
Assert-True ($networkReceiverText -match
    '\[string\]\$EnablePerformanceProbes\s*=\s*"off"') `
    "网络接收脚本必须声明默认关闭的性能探针参数。"
Assert-True ($networkReceiverText -match
    '-EnablePerformanceProbes\s+\$EnablePerformanceProbes') `
    "网络接收脚本必须向 Runtime 基准精确透传性能探针状态。"
Assert-True ($networkReceiverText -match
    'coverage\s*=\s*\$report\.coverage' -and
    $networkReceiverText -match
        'ndi_video_queue_depth\s*=\s*\$report\.ndi_video_queue_depth') `
    "网络完成标记必须携带覆盖分段和 NDI 队列深度测量证据。"

$dualReceiverText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/invoke_dual_machine_receiver.ps1"))
Assert-True ($dualReceiverText -match
    '\[string\]\$EnablePerformanceProbes\s*=\s*"off"' -and
    $dualReceiverText -match
        'EnablePerformanceProbes\s*=\s*\$EnablePerformanceProbes' -and
    $dualReceiverText -match
        '\[uint64\]\$MaximumRuntimeOverwrittenFrames\s*=\s*0' -and
    $dualReceiverText -match
        'MaximumRuntimeOverwrittenFrames\s*=\s*\$MaximumRuntimeOverwrittenFrames') `
    "双机远程入口必须显式透传探针状态和默认为零的 Runtime 覆盖上限。"

. (Join-Path $RepositoryRoot "scripts/runtime_environment.ps1")
$successQuery = {
    param([string]$ClassName)
    if ($ClassName -eq "Win32_VideoController") {
        return [pscustomobject]@{
            Name = "Test GPU"
            DriverVersion = "1.2.3"
            AdapterRAM = 4096
        }
    }
    return [pscustomobject]@{ Caption = "Test Windows" }
}
$complete = Get-XenRuntimeHardwareInventory -CimQuery $successQuery
Assert-True ($complete.inventory_status -eq "complete") `
    "正常 WMI 查询必须生成 complete 硬件清单。"
Assert-True ($complete.os -eq "Test Windows" -and
    @($complete.gpu).Count -eq 1 -and
    $complete.gpu[0].name -eq "Test GPU") `
    "正常 WMI 查询的 OS/GPU 字段不正确。"

$failureQuery = {
    param([string]$ClassName)
    throw "模拟非管理员拒绝：$ClassName"
}
$partial = Get-XenRuntimeHardwareInventory -CimQuery $failureQuery
Assert-True ($partial.inventory_status -eq "partial") `
    "WMI 拒绝必须生成 partial 硬件清单。"
Assert-True (-not [string]::IsNullOrWhiteSpace($partial.os) -and
    @($partial.gpu).Count -eq 0 -and
    @($partial.inventory_errors).Count -ge 2) `
    "WMI 拒绝必须保留 OS 回退和明确错误。"

$invokeScript = Join-Path $RepositoryRoot `
    "scripts/invoke_dual_machine_receiver.ps1"
$previousLocation = Get-Location
$previousErrorActionPreference = $ErrorActionPreference
try {
    Set-Location (Join-Path $env:SystemRoot "System32")
    # Windows PowerShell 5 会把子 PowerShell 的预期 stderr 包装为非终止错误；
    # 本回归按退出码和完整文本判断，不能让外层 Stop 提前截断证据。
    $ErrorActionPreference = "Continue"
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $invokeScript -Scenario GeometryStatic -Mode Prepare `
        -EnablePerformanceProbes on 2>&1)
    $exitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
    Set-Location $previousLocation
}
$outputText = $output -join "`n"
Assert-True ($exitCode -ne 0 -and
    $outputText -match "便携包清单不存在" -and
    $outputText -notmatch "Join-Path") `
    "任意工作目录启动必须绑定探针参数并在脚本体内解析默认 PackageRoot。"

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("xen-dual-provider-{0}" -f [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $fixtureManifest = [ordered]@{
        schema = 1
        complete = $true
        package_type = "xen-dual-machine-receiver"
        package_id = "provider-fixture"
        allowed_backends = @("cpu")
        allowed_capture_backends = @("xudp_jpeg")
        model = [ordered]@{ relative_path = "Release/models/missing.onnx" }
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureRoot "package-manifest.json"),
        ($fixtureManifest | ConvertTo-Json -Depth 5),
        [System.Text.UTF8Encoding]::new($false))
    $ErrorActionPreference = "Continue"
    foreach ($backend in @("tensorrt", "directml")) {
        $providerOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File $invokeScript `
            -Scenario GeometryStatic -Mode Prepare `
            -PackageRoot $fixtureRoot -Backend $backend 2>&1)
        $providerExitCode = $LASTEXITCODE
        Assert-True ($providerExitCode -ne 0 -and
            ($providerOutput -join "`n") -match
                "便携包不允许 Provider：$backend") `
            "远程入口必须在访问模型前拒绝清单未授权的 Provider：$backend。"
    }
    $ErrorActionPreference = $previousErrorActionPreference
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}

Write-Host "双机 PowerShell 脚本回归通过。"
