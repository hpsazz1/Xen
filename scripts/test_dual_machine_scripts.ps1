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
    "scripts/aim_report.ps1",
    "scripts/aim_control_diagnostics.ps1",
    "scripts/runtime_report_sequence.ps1",
    "scripts/test_benchmark_report_scale.ps1",
    "scripts/publish_dual_machine_package.ps1",
    "scripts/invoke_aim_manual_acceptance.ps1",
    "scripts/run_ndi_clock_source.ps1",
    "scripts/publish_aim_worker_delta.ps1",
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
Assert-True ($networkReceiverText -match
    '\[string\]\$AimPrediction\s*=\s*"off"' -and
    $networkReceiverText -match
        '\[double\]\$AimMaxPredictionLeadPercent\s*=\s*35\.0' -and
    $networkReceiverText -match '(?m)^\s*"\[aim\]",\s*$' -and
    $networkReceiverText -match
        '"enable_prediction=\$aimPredictionValue"' -and
    $networkReceiverText -match
        '"max_prediction_lead_percent=\$aimMaxPredictionLeadText"' -and
    $networkReceiverText -match '(?m)^\s*"\[mouse\]",\s*$' -and
    $networkReceiverText -match '"allow_send_input=false"') `
    "网络接收脚本必须只暴露预测开关/最大提前距离，并显式固定完整 Aim 与禁用 Mouse。"
Assert-True ($networkReceiverText -match
    '-ExpectedAimPrediction\s+\$AimPrediction' -and
    $networkReceiverText -match
        '-ExpectedAimMaxPredictionLeadPercent' -and
    $networkReceiverText -match
        'summary\s*=\s*\$environment\.report\.aim') `
    "网络接收脚本必须把 Aim 参数和 schema 14 汇总传入正式报告。"

$runtimeBenchmarkText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/benchmark_runtime.ps1"))
Assert-True ($runtimeBenchmarkText -match
    'if \(\$report\.schema -ne 14\)' -and
    $runtimeBenchmarkText -match 'Get-XenAimReportSummary' -and
    $runtimeBenchmarkText -match
        'prediction_point_outside_box_frames' -and
    $runtimeBenchmarkText -match 'Get-XenRuntimeSequenceValues' -and
    $runtimeBenchmarkText -notmatch
        '@\(\$report\.samples\)\[\$csvIndex\]') `
    "Runtime 正式入口必须消费 schema 14，并把预测出框作为观测而非违规。"

$packageScriptText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/publish_dual_machine_package.ps1"))
Assert-True ($packageScriptText -match '"aim_report\.ps1"' -and
    $packageScriptText -match '"runtime_report_sequence\.ps1"') `
    "双机便携包必须携带 Aim 与线性 sequence 报告助手。"

$aimDeltaText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/publish_aim_worker_delta.ps1"))
Assert-True ($aimDeltaText -match
        '"AIM-SUPERJUMP-ACCEPT-001"\)\]' -and
    $aimDeltaText -match
        '\[string\]\$TaskId\s*=\s*"AIM-LATENCY-COMP-001"' -and
    $aimDeltaText -match
        '\$remoteScript\s*\+\s*''" -TaskId ''\s*\+\s*\$TaskId' -and
    $aimDeltaText -match
        '\[string\]\$task\.task_id\s*-ne\s*\$TaskId' -and
    $aimDeltaText -match
        '\[bool\]\$task\.aim\.prediction_enabled\s*-ne\s*\(\$Profile -eq "prediction"\)' -and
    $aimDeltaText -match
        '\[double\]\$task\.aim\.counts_per_pixel_x\s*-ne\s*\$resolvedCountsPerPixelX' -and
    $aimDeltaText -match
        '\[double\]\$task\.aim\.counts_per_pixel_y\s*-ne\s*\$resolvedCountsPerPixelY' -and
    $aimDeltaText -match
        '\[double\]\$task\.aim\.max_counts_per_frame\s*-ne\s*\$MaxCountsPerFrame' -and
    $aimDeltaText -match
        '\[double\]\$task\.aim\.control_delay_ms\s*-ne\s*\$ControlDelayMs' -and
    $aimDeltaText -match
        '\[double\]\$task\.aim\.max_delay_compensation_ms\s*-ne' -and
    $aimDeltaText -match
        ''' -MaxDelayCompensationPercent ''\s*\+\s*') `
    "Aim 差量入口必须透传任务 ID、延迟参数，并按 profile 回读 prediction 快照。"
Assert-True ($aimDeltaText -match '\$toolSpecs\s*=\s*@\(' -and
    $aimDeltaText -match 'tools/aim_report\.ps1' -and
    $aimDeltaText -match 'tools/aim_control_diagnostics\.ps1' -and
    $aimDeltaText -match '\$changedTools' -and
    $aimDeltaText -match
        '\$manifest\.git_commit\s*=\s*\$commit\.ToLowerInvariant\(\)' -and
    $aimDeltaText -match '\$remoteToolStageName' -and
    $aimDeltaText -match '\$moveStatements\.Add\(' -and
    $aimDeltaText -match 'ConvertTo-PowerShellEncodedCommand\s+\$applyScript' -and
    $aimDeltaText -match 'escapedManifestStage') `
    "Aim 差量入口必须暂存完整报告工具闭包，并由辅机本地 PowerShell 最后发布 manifest。"
Assert-True (([regex]::Matches(
        $aimDeltaText,
        '\$records\s*=\s*@\(@\(\$(?:manifest|finalManifest)\.files\)\s*\|\s*Where-Object')).Count -eq 2) `
    "Aim 差量入口的单项 manifest 查询必须保持显式数组，不能在严格模式下退化为标量。"

$aimManualText = [System.IO.File]::ReadAllText(
    (Join-Path $RepositoryRoot "scripts/invoke_aim_manual_acceptance.ps1"))
Assert-True ($aimManualText -match
        'tools\\aim_control_diagnostics\.ps1' -and
    $aimManualText -match
        'Get-XenAimControlDiagnosticsSummary\s+-Samples\s+\$allSamples' -and
    $aimManualText -match
        'reverse_translation_detail_diagnostics_available' -and
    $aimManualText -match
        'control_diagnostics\s*=\s*\$controlDiagnostics' -and
    $aimManualText -match '(?m)^\s*schema\s*=\s*2\s*$') `
    "Aim 人工入口必须把 schema 14 控制诊断直接写入自动汇总。"

& (Join-Path $RepositoryRoot "scripts/test_benchmark_report_scale.ps1") `
    -SyntheticSampleCount 72002 -LegacyProbeCount 5000 -Quiet

$networkPrepareRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("xen-network-aim-{0}" -f [guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $networkPrepareRoot -Force |
        Out-Null
    $networkPrefix = Join-Path $networkPrepareRoot "receiver"
    & (Join-Path $RepositoryRoot "scripts/benchmark_network_receiver.ps1") `
        -ModelPath (Join-Path $networkPrepareRoot "missing.onnx") `
        -ReportPrefix $networkPrefix -CaptureBackend ndi `
        -AimPrediction on -AimMaxPredictionLeadPercent 42.5 `
        -PrepareOnly | Out-Null
    $preparedConfig = [System.IO.File]::ReadAllText(
        "$networkPrefix.receiver.ini")
    Assert-True ($preparedConfig -match '(?m)^\[aim\]\r?$' -and
        $preparedConfig -match '(?m)^person_class_ids=0,2\r?$' -and
        $preparedConfig -match '(?m)^head_class_ids=1,3\r?$' -and
        $preparedConfig -match '(?m)^enable_prediction=true\r?$' -and
        $preparedConfig -match
            '(?m)^max_prediction_lead_percent=42\.500000\r?$' -and
        $preparedConfig -match '(?m)^\[mouse\]\r?$' -and
        $preparedConfig -match '(?m)^allow_send_input=false\r?$') `
        "PrepareOnly 必须落盘完整 Aim、预测参数和显式禁用 Mouse。"
} finally {
    Remove-Item -LiteralPath $networkPrepareRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}

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
Assert-True ($dualReceiverText -match
    '\[string\]\$AimPrediction\s*=\s*"off"' -and
    $dualReceiverText -match
        'AimPrediction\s*=\s*\$AimPrediction' -and
    $dualReceiverText -match
        'AimMaxPredictionLeadPercent\s*=\s*\$AimMaxPredictionLeadPercent' -and
    $dualReceiverText -match
        '\$CaptureBackend -eq "udp_mjpeg"') `
    "双机入口必须透传少量 Aim 参数，并只限制裸 UDP 场景。"

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
        allowed_capture_backends = @("xudp_jpeg", "udp_mjpeg", "ndi")
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
    $ndiOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File $invokeScript `
        -Scenario SuperJump -CaptureBackend ndi -Mode Prepare `
        -PackageRoot $fixtureRoot -Backend cpu 2>&1)
    $ndiExitCode = $LASTEXITCODE
    $ndiText = $ndiOutput -join "`n"
    Assert-True ($ndiExitCode -ne 0 -and
        $ndiText -match "便携包缺少运行输入" -and
        $ndiText -notmatch "只运行 GeometryStatic") `
        "NDI 必须允许 SuperJump 等 Aim 场景，并在后续真实输入门禁处失败。"

    $udpOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File $invokeScript `
        -Scenario SuperJump -CaptureBackend udp_mjpeg -Mode Prepare `
        -PackageRoot $fixtureRoot -Backend cpu 2>&1)
    $udpExitCode = $LASTEXITCODE
    Assert-True ($udpExitCode -ne 0 -and
        ($udpOutput -join "`n") -match
            "裸 UDP 只运行 GeometryStatic") `
        "裸 UDP 仍应只保留三个兼容对照锚点。"
    $ErrorActionPreference = $previousErrorActionPreference
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}

Write-Host "双机 PowerShell 脚本回归通过。"
