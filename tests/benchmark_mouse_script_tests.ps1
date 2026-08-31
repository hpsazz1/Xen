param(
    [string]$BuildDirectory = "",
    [Parameter(Mandatory = $true)]
    [string]$GitExecutable
)

$ErrorActionPreference = "Stop"

function Assert-Contract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) { throw $Message }
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot "build-b4b-mouse"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$serverExecutable = Join-Path `
    $BuildDirectory "Release\mouse_benchmark_tests.exe"
$benchmarkExecutable = Join-Path `
    $BuildDirectory "Release\XenMouseBenchmark.exe"
foreach ($path in @($serverExecutable, $benchmarkExecutable)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Mouse benchmark 脚本专项缺少 Release 目标：$path"
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("xen-b4b-mouse-script-" + [guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
$server = $null
try {
    $scriptPath = Join-Path $repositoryRoot "scripts\benchmark_mouse.ps1"

    # 该进程只写测试报告，不调用任何鼠标 API；用于覆盖脚本的 Win32 聚合合同。
    $fixtureBuild = Join-Path $temporaryRoot "fixture-build"
    $fixtureRelease = Join-Path $fixtureBuild "Release"
    [System.IO.Directory]::CreateDirectory($fixtureRelease) | Out-Null
    $fixtureExecutable = Join-Path $fixtureRelease "XenMouseBenchmark.exe"
    $fixtureSource = @'
using System;
using System.IO;
using System.Text;

public static class ScriptReportFixture
{
    public static int Main(string[] args)
    {
        string report = null;
        string runUuid = null;
        for (int index = 0; index + 1 < args.Length; ++index)
        {
            if (args[index] == "--report") report = args[index + 1];
            if (args[index] == "--run-uuid") runUuid = args[index + 1];
        }
        if (String.IsNullOrEmpty(report) || String.IsNullOrEmpty(runUuid))
            return 2;

        bool mismatch = Environment.GetEnvironmentVariable(
            "XEN_MOUSE_BENCHMARK_TEST_PROVENANCE_MISMATCH") == "1";
        string semantic = mismatch
            ? "kmbox_matched_udp_protocol_ack"
            : "windows_input_stream_insertion";
        string aggregation = "win32_send_input|" + semantic +
            "|local_os_api|none";
        string json =
            "{\"schema\":2,\"run_uuid\":\"" + runUuid +
            "\",\"complete\":true,\"backend\":\"win32_send_input\"," +
            "\"endpoint\":\"\",\"provenance\":{" +
            "\"completion_semantic\":\"" + semantic + "\"," +
            "\"peer_test_boundary\":\"local_os_api\"," +
            "\"protocol_ack_observed\":false," +
            "\"physical_effect_observation_method\":\"none\"," +
            "\"physical_effect_observed\":false," +
            "\"aggregation_key\":\"" + aggregation + "\"}," +
            "\"authorization\":{\"physical_output\":true," +
            "\"confirmation_token\":true}," +
            "\"command\":{\"dx_counts\":2,\"dy_counts\":-1," +
            "\"paired_reverse\":true}," +
            "\"configuration\":{\"warmup_pairs\":0," +
            "\"sample_pairs\":1,\"connect_timeout_ms\":0," +
            "\"command_timeout_ms\":0,\"baud_rate\":0," +
            "\"kmbox_uuid_recorded\":false}," +
            "\"stats\":{\"successful_commands\":4," +
            "\"formal_successful_commands\":2,\"failed_commands\":0," +
            "\"final_status\":\"READY\"}," +
            "\"timing\":{\"open_ms\":0,\"first_command_ms\":0," +
            "\"first_compensation_ms\":0,\"warmup_elapsed_ms\":0," +
            "\"formal_elapsed_ms\":0,\"total_elapsed_ms\":0," +
            "\"command_latency\":{\"mean_ms\":0,\"p50_ms\":0," +
            "\"p95_ms\":0,\"p99_ms\":0,\"max_ms\":0}}," +
            "\"samples\":[{\"sequence\":1,\"pair_index\":1," +
            "\"direction\":1,\"dx_counts\":2,\"dy_counts\":-1," +
            "\"latency_ms\":0},{\"sequence\":2,\"pair_index\":1," +
            "\"direction\":-1,\"dx_counts\":-2,\"dy_counts\":1," +
            "\"latency_ms\":0}]}";
        File.WriteAllText(report, json, new UTF8Encoding(false));
        return 0;
    }
}
'@
    Add-Type -TypeDefinition $fixtureSource -Language CSharp `
        -OutputAssembly $fixtureExecutable -OutputType ConsoleApplication
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureBuild "CMakeCache.txt"), "test fixture",
        [System.Text.UTF8Encoding]::new($false))

    $win32Prefix = Join-Path $temporaryRoot "win32-contract"
    & $scriptPath `
        -Backend Win32 `
        -ReportPrefix $win32Prefix `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
        -BuildDirectory $fixtureBuild `
        -GitExecutable $GitExecutable `
        -Configuration Release `
        -WarmupPairs 0 `
        -SamplePairs 1 `
        -DxCounts 2 `
        -DyCounts -1
    $win32Report = Get-Content -LiteralPath "$win32Prefix.mouse.json" `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Contract ($win32Report.provenance.aggregation_key -eq
            "win32_send_input|windows_input_stream_insertion|local_os_api|none") `
        "脚本必须按报告 backend 名保留 Win32 聚合键。"

    $mismatchPrefix = Join-Path $temporaryRoot "semantic-mismatch"
    $mismatchError = ""
    $env:XEN_MOUSE_BENCHMARK_TEST_PROVENANCE_MISMATCH = "1"
    try {
        & $scriptPath `
            -Backend Win32 `
            -ReportPrefix $mismatchPrefix `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
            -BuildDirectory $fixtureBuild `
            -GitExecutable $GitExecutable `
            -Configuration Release `
            -WarmupPairs 0 `
            -SamplePairs 1 `
            -DxCounts 2 `
            -DyCounts -1
    } catch {
        $mismatchError = $_.Exception.Message
    } finally {
        Remove-Item Env:XEN_MOUSE_BENCHMARK_TEST_PROVENANCE_MISMATCH `
            -ErrorAction SilentlyContinue
    }
    Assert-Contract ($mismatchError -match '拒绝跨语义合并') `
        "脚本必须拒绝 backend 与 completion semantic 不匹配的报告。"
    Assert-Contract (-not (Test-Path -LiteralPath `
            "$mismatchPrefix.mouse.json")) `
        "semantic 不匹配不得发布正式报告。"

    $implicitExternalPrefix = Join-Path $temporaryRoot `
        "implicit-external-kmbox\report"
    $implicitExternalError = ""
    try {
        & $scriptPath `
            -Backend KmboxNet `
            -ReportPrefix $implicitExternalPrefix `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
            -BuildDirectory $fixtureBuild `
            -GitExecutable $GitExecutable `
            -Configuration Release `
            -PeerTestBoundary Auto `
            -WarmupPairs 0 `
            -SamplePairs 1 `
            -DxCounts 1 `
            -KmboxIp 192.0.2.1 `
            -KmboxPort 1 `
            -KmboxUuid A1B2C3D4
    } catch {
        $implicitExternalError = $_.Exception.Message
    }
    Assert-Contract (-not (Test-Path -LiteralPath `
            (Split-Path -Parent $implicitExternalPrefix))) `
        "KmboxNet 缺少显式 execution boundary 必须在打开或 mkdir 前拒绝。"
    Assert-Contract ($implicitExternalError -match `
            'ConfiguredExternalDevicePeer') `
        "KmboxNet 的旧 Auto 不能再隐式声明 external device peer。"

    $implicitMakcuPrefix = Join-Path $temporaryRoot `
        "implicit-external-makcu\report"
    $implicitMakcuError = ""
    try {
        & $scriptPath `
            -Backend Makcu `
            -ReportPrefix $implicitMakcuPrefix `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
            -BuildDirectory $fixtureBuild `
            -GitExecutable $GitExecutable `
            -Configuration Release `
            -WarmupPairs 0 `
            -SamplePairs 1 `
            -DxCounts 1 `
            -MakcuPort COM8
    } catch {
        $implicitMakcuError = $_.Exception.Message
    }
    Assert-Contract (-not (Test-Path -LiteralPath `
            (Split-Path -Parent $implicitMakcuPrefix))) `
        "Makcu 缺少显式 execution boundary 必须在打开或 mkdir 前拒绝。"
    Assert-Contract ($implicitMakcuError -match `
            'ConfiguredExternalDevicePeer') `
        "Makcu 的默认 Auto 不能再隐式声明 external device peer。"

    $externalAsFakePrefix = Join-Path $temporaryRoot "external-as-fake"
    $externalAsFakeError = ""
    try {
        & $scriptPath `
            -Backend KmboxNet `
            -ReportPrefix $externalAsFakePrefix `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
            -BuildDirectory $BuildDirectory `
            -GitExecutable $GitExecutable `
            -Configuration Release `
            -PeerTestBoundary LoopbackUdpFake `
            -WarmupPairs 0 `
            -SamplePairs 1 `
            -DxCounts 1 `
            -KmboxIp 192.0.2.1 `
            -KmboxPort 1 `
            -KmboxUuid A1B2C3D4
    } catch {
        $externalAsFakeError = $_.Exception.Message
    }
    Assert-Contract ($externalAsFakeError -match '127/8') `
        "脚本必须在打开 endpoint 前拒绝把外部 KMBOX 标成 loopback fake。"
    Assert-Contract (-not (Test-Path -LiteralPath `
            "$externalAsFakePrefix.mouse.json")) `
        "边界不匹配不得发布正式报告。"

    $loopbackAsExternalPrefix = Join-Path $temporaryRoot "loopback-as-external"
    $loopbackAsExternalError = ""
    try {
        & $scriptPath `
            -Backend KmboxNet `
            -ReportPrefix $loopbackAsExternalPrefix `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
            -BuildDirectory $BuildDirectory `
            -GitExecutable $GitExecutable `
            -Configuration Release `
            -PeerTestBoundary Auto `
            -WarmupPairs 0 `
            -SamplePairs 1 `
            -DxCounts 1 `
            -KmboxIp 127.0.0.1 `
            -KmboxPort 1 `
            -KmboxUuid A1B2C3D4
    } catch {
        $loopbackAsExternalError = $_.Exception.Message
    }
    Assert-Contract ($loopbackAsExternalError -match 'LoopbackUdpFake') `
        "脚本必须在打开 endpoint 前拒绝把 127/8 标成 external peer。"
    Assert-Contract (-not (Test-Path -LiteralPath `
            "$loopbackAsExternalPrefix.mouse.json")) `
        "边界不匹配不得发布正式报告。"

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $serverExecutable
    $startInfo.Arguments = "--serve-script-kmbox-fake"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $server = [System.Diagnostics.Process]::new()
    $server.StartInfo = $startInfo
    Assert-Contract ($server.Start()) "无法启动 loopback KMBOX fake。"
    $portLine = $server.StandardOutput.ReadLine()
    Assert-Contract ($portLine -match '^PORT=([0-9]+)$') `
        "loopback KMBOX fake 未返回端口：$portLine"
    $port = [int]$Matches[1]

    # 正式 wrapper 在启动目标前会核对可执行文件、CMake cache 与部署
    # DLL。固定等待把这段 preflight 与 fake 的存活合同变成确定性回归，
    # 避免只在主构建输出较大时偶发暴露过短的测试端接收预算。
    Start-Sleep -Milliseconds 2000

    $reportPrefix = Join-Path $temporaryRoot "script-contract"
    & $scriptPath `
        -Backend KmboxNet `
        -ReportPrefix $reportPrefix `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT `
        -BuildDirectory $BuildDirectory `
        -GitExecutable $GitExecutable `
        -Configuration Release `
        -PeerTestBoundary LoopbackUdpFake `
        -WarmupPairs 0 `
        -SamplePairs 1 `
        -DxCounts 3 `
        -DyCounts -2 `
        -KmboxIp 127.0.0.1 `
        -KmboxPort $port `
        -KmboxUuid A1B2C3D4 `
        -ConnectTimeoutMs 500 `
        -CommandTimeoutMs 300

    Assert-Contract ($server.WaitForExit(5000)) `
        "loopback KMBOX fake 未在协议样本完成后退出。"
    Assert-Contract ($server.ExitCode -eq 0) `
        ("loopback KMBOX fake 退出码非法：" + $server.ExitCode +
         "，stderr=" + $server.StandardError.ReadToEnd())

    $reportPath = "$reportPrefix.mouse.json"
    $environmentPath = "$reportPrefix.mouse.environment.json"
    $rawReport = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8
    $report = $rawReport | ConvertFrom-Json
    $environment = Get-Content -LiteralPath $environmentPath -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $parsedRunUuid = [guid]::Empty
    Assert-Contract (
        $report.schema -eq 2 -and $environment.schema -eq 2 -and
        [guid]::TryParseExact([string]$report.run_uuid, "D",
                              [ref]$parsedRunUuid) -and
        [string]$environment.run_uuid -eq [string]$report.run_uuid) `
        "正式报告与环境报告必须共享 schema 2 run UUID。"
    Assert-Contract (
        $report.provenance.completion_semantic -eq
            "kmbox_matched_udp_protocol_ack" -and
        $report.provenance.peer_test_boundary -eq
            "loopback_udp_fake" -and
        $report.provenance.protocol_ack_observed -eq $true -and
        $report.provenance.physical_effect_observation_method -eq "none" -and
        $report.provenance.physical_effect_observed -eq $false -and
        $report.provenance.aggregation_key -eq
            "kmbox_net|kmbox_matched_udp_protocol_ack|loopback_udp_fake|none") `
        "loopback 报告必须自证 ACK、fake peer 和未观察 physical effect。"
    Assert-Contract (
        $environment.benchmark.provenance.aggregation_key -eq
            $report.provenance.aggregation_key -and
        $environment.benchmark.provenance.completion_semantic -eq
            $report.provenance.completion_semantic -and
        $environment.benchmark.provenance.peer_test_boundary -eq
            $report.provenance.peer_test_boundary) `
        "脚本复制 timing 前必须保留完整聚合键，禁止跨语义合并。"
    Assert-Contract (-not $rawReport.Contains("A1B2C3D4")) `
        "脚本专项报告不得泄露 KMBOX UUID。"
    Write-Host "Mouse benchmark PowerShell 专项全部通过。"
} finally {
    if ($null -ne $server -and -not $server.HasExited) {
        $server.Kill()
        $server.WaitForExit()
    }
    $resolvedTemporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
    $resolvedSystemTemp = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath())
    if ($resolvedTemporaryRoot.StartsWith(
            $resolvedSystemTemp,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
            "xen-b4b-mouse-script-",
            [System.StringComparison]::Ordinal)) {
        [System.IO.Directory]::Delete($resolvedTemporaryRoot, $true)
    }
}
