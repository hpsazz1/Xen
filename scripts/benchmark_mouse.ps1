param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Win32", "KmboxNet", "Makcu")]
    [string]$Backend,
    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,
    [Parameter(Mandatory = $true)]
    [switch]$AllowPhysicalOutput,
    [Parameter(Mandatory = $true)]
    [ValidateSet("XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT")]
    [string]$PhysicalOutputConfirmation,
    [string]$BuildDirectory = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("Auto", "ConfiguredExternalDevicePeer", "LoopbackUdpFake")]
    [string]$PeerTestBoundary = "Auto",
    [ValidateRange(0, 100000)]
    [int]$WarmupPairs = 100,
    [ValidateRange(1, 500000)]
    [int]$SamplePairs = 10000,
    [ValidateRange(-32767, 32767)]
    [int]$DxCounts = 1,
    [ValidateRange(-32767, 32767)]
    [int]$DyCounts = 0,
    [string]$KmboxIp = "",
    [ValidateRange(0, 65535)]
    [int]$KmboxPort = 0,
    [string]$KmboxUuid = "",
    [string]$MakcuPort = "",
    [ValidateSet(4000000)]
    [int]$MakcuBaudRate = 4000000,
    [ValidateRange(1, 10000)]
    [int]$ConnectTimeoutMs = 1000,
    [ValidateRange(1, 1000)]
    [int]$CommandTimeoutMs = 300,
    [string]$GitExecutable = "git"
)

$ErrorActionPreference = "Stop"

function Get-FileEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        last_write_utc = $file.LastWriteTimeUtc.ToString("o")
        file_version = $file.VersionInfo.FileVersion
        product_version = $file.VersionInfo.ProductVersion
    }
}

function Get-DllEvidence {
    param([Parameter(Mandatory = $true)][string]$Directory)
    $result = [ordered]@{}
    foreach ($file in @(Get-ChildItem -LiteralPath $Directory -File `
            -Filter "*.dll" | Sort-Object Name)) {
        $result[$file.Name] = Get-FileEvidence $file.FullName
    }
    return $result
}

function Assert-SnapshotUnchanged {
    param(
        [System.Collections.IDictionary]$Before,
        [System.Collections.IDictionary]$After,
        [string]$Description
    )
    if ($Before.sha256 -ne $After.sha256 -or
        $Before.length -ne $After.length) {
        throw "$Description 在鼠标基准期间发生变化。"
    }
}

if (-not $AllowPhysicalOutput.IsPresent -or
    $PhysicalOutputConfirmation -ne
        "XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT") {
    throw "鼠标基准会发送真实输入，必须同时给出物理输出开关和固定确认令牌。"
}
if ($DxCounts -eq 0 -and $DyCounts -eq 0) {
    throw "DxCounts 与 DyCounts 不能同时为零。"
}
if ($PeerTestBoundary -eq "LoopbackUdpFake" -and
    $Backend -ne "KmboxNet") {
    throw "LoopbackUdpFake boundary 只适用于 KmboxNet 专项。"
}
if (($Backend -eq "KmboxNet" -or $Backend -eq "Makcu") -and
    $PeerTestBoundary -eq "Auto") {
    throw "$Backend 必须显式声明 ConfiguredExternalDevicePeer；" +
        "127/8 KMBOX fake 必须显式声明 LoopbackUdpFake。"
}
if ($Backend -eq "KmboxNet") {
    if ($KmboxIp -notmatch `
            '^(25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|[01]?[0-9]?[0-9])){3}$' -or
        $KmboxPort -le 0 -or
        $KmboxUuid -notmatch '^[0-9A-Fa-f]{8}$') {
        throw "KmboxNet 必须提供有效 IPv4、端口和 8 位十六进制 UUID。"
    }
    $isLoopbackEndpoint = $KmboxIp -match '^127\.'
    if ($PeerTestBoundary -eq "LoopbackUdpFake" -and
        -not $isLoopbackEndpoint) {
        throw "LoopbackUdpFake boundary 只允许 127/8 endpoint。"
    }
    if ($PeerTestBoundary -eq "ConfiguredExternalDevicePeer" -and
        $isLoopbackEndpoint) {
        throw "127/8 endpoint 不能声明 ConfiguredExternalDevicePeer；" +
            "必须显式声明 LoopbackUdpFake boundary。"
    }
    if (-not [string]::IsNullOrEmpty($MakcuPort) -or
        $PSBoundParameters.ContainsKey("MakcuBaudRate")) {
        throw "KmboxNet 基准不得携带 MAKCU 设备参数。"
    }
} elseif ($Backend -eq "Makcu") {
    if ($MakcuPort -notmatch '^(?i:COM)([1-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-6])$') {
        throw "Makcu 必须提供 COM1..COM256 串口。"
    }
    if (-not [string]::IsNullOrEmpty($KmboxIp) -or
        $KmboxPort -ne 0 -or
        -not [string]::IsNullOrEmpty($KmboxUuid)) {
        throw "Makcu 基准不得携带 KMBOX 设备参数。"
    }
} elseif ($PeerTestBoundary -ne "Auto") {
    throw "Win32 execution boundary 内生为 local_os_api，不接受设备 peer 声明。"
} elseif (-not [string]::IsNullOrEmpty($KmboxIp) -or
          $KmboxPort -ne 0 -or
          -not [string]::IsNullOrEmpty($KmboxUuid) -or
          -not [string]::IsNullOrEmpty($MakcuPort) -or
          $PSBoundParameters.ContainsKey("MakcuBaudRate")) {
    throw "Win32 基准不得携带物理设备参数。"
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build"
}
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$executable = Join-Path `
    $BuildDirectory "$Configuration\XenMouseBenchmark.exe"
$cmakeCache = Join-Path $BuildDirectory "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf) -or
    -not (Test-Path -LiteralPath $cmakeCache -PathType Leaf)) {
    throw "XenMouseBenchmark 或 CMakeCache.txt 不存在：$BuildDirectory"
}

$outputDirectory = Split-Path -Parent $executable
$ReportPrefix = [System.IO.Path]::GetFullPath($ReportPrefix)
$reportDirectory = Split-Path -Parent $ReportPrefix
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$finalReport = "$ReportPrefix.mouse.json"
$finalEnvironment = "$ReportPrefix.mouse.environment.json"
$runUuid = [guid]::NewGuid().ToString("D")
$pendingId = $runUuid.Replace("-", "")
$pendingReport = "$finalReport.pending-$pendingId"
$pendingEnvironment = "$finalEnvironment.pending-$pendingId"
foreach ($path in @($finalReport, $finalEnvironment,
                    $pendingReport, $pendingEnvironment)) {
    if (Test-Path -LiteralPath $path) {
        throw "鼠标基准目标已存在，拒绝覆盖：$path"
    }
}

$executableBefore = Get-FileEvidence $executable
$cacheBefore = Get-FileEvidence $cmakeCache
$dllsBefore = Get-DllEvidence $outputDirectory
$backendArgument = switch ($Backend) {
    "Win32" { "win32" }
    "KmboxNet" { "kmbox_net" }
    "Makcu" { "makcu" }
}
$backendReportName = switch ($Backend) {
    "Win32" { "win32_send_input" }
    "KmboxNet" { "kmbox_net" }
    "Makcu" { "makcu" }
}
$expectedCompletionSemantic = switch ($Backend) {
    "Win32" { "windows_input_stream_insertion" }
    "KmboxNet" { "kmbox_matched_udp_protocol_ack" }
    "Makcu" { "makcu_matched_serial_device_status_ack" }
}
$expectedPeerTestBoundary = if (
        $PeerTestBoundary -eq "LoopbackUdpFake") {
    "loopback_udp_fake"
} elseif ($Backend -eq "Win32") {
    "local_os_api"
} else {
    "configured_external_device_peer"
}
$expectedProtocolAckObserved = $Backend -ne "Win32"
$expectedAggregationKey =
    "$backendReportName|$expectedCompletionSemantic|" +
    "$expectedPeerTestBoundary|none"
$arguments = @(
    "--backend", $backendArgument,
    "--report", $pendingReport,
    "--run-uuid", $runUuid,
    "--peer-test-boundary", $expectedPeerTestBoundary,
    "--warmup-pairs", [string]$WarmupPairs,
    "--sample-pairs", [string]$SamplePairs,
    "--dx-counts", [string]$DxCounts,
    "--dy-counts", [string]$DyCounts,
    "--allow-physical-output",
    "--confirm-physical-output", $PhysicalOutputConfirmation
)
if ($Backend -eq "KmboxNet") {
    $arguments += @(
        "--kmbox-ip", $KmboxIp,
        "--kmbox-port", [string]$KmboxPort,
        "--kmbox-uuid", $KmboxUuid,
        "--connect-timeout-ms", [string]$ConnectTimeoutMs,
        "--command-timeout-ms", [string]$CommandTimeoutMs
    )
} elseif ($Backend -eq "Makcu") {
    $arguments += @(
        "--makcu-port", $MakcuPort,
        "--makcu-baud-rate", [string]$MakcuBaudRate,
        "--connect-timeout-ms", [string]$ConnectTimeoutMs,
        "--command-timeout-ms", [string]$CommandTimeoutMs
    )
}

$startedUtc = [DateTime]::UtcNow
$originalTaskPath = $env:PATH
$exitCode = -1
$published = New-Object System.Collections.Generic.List[string]
try {
    try {
        # 可执行目标只依赖 Windows 系统 DLL。clean PATH 防止开发 SDK 掩盖部署问题。
        $env:PATH = @(
            (Join-Path $env:SystemRoot "System32"),
            $env:SystemRoot
        ) -join ";"
        Push-Location $repositoryRoot
        try {
            & $executable @arguments
            $exitCode = $LASTEXITCODE
        } finally {
            Pop-Location
        }
    } finally {
        $env:PATH = $originalTaskPath
    }
    $finishedUtc = [DateTime]::UtcNow
    if ($exitCode -ne 0) {
        throw "XenMouseBenchmark 失败，退出码：$exitCode"
    }
    if (-not (Test-Path -LiteralPath $pendingReport -PathType Leaf)) {
        throw "XenMouseBenchmark 成功退出但没有生成 pending 报告。"
    }

    $executableAfter = Get-FileEvidence $executable
    $cacheAfter = Get-FileEvidence $cmakeCache
    $dllsAfter = Get-DllEvidence $outputDirectory
    Assert-SnapshotUnchanged $executableBefore $executableAfter `
        "XenMouseBenchmark"
    Assert-SnapshotUnchanged $cacheBefore $cacheAfter "CMake Cache"
    if (($dllsBefore.Keys -join "`n") -ne
        ($dllsAfter.Keys -join "`n")) {
        throw "鼠标基准期间部署 DLL 集合发生变化。"
    }
    foreach ($name in $dllsBefore.Keys) {
        Assert-SnapshotUnchanged $dllsBefore[$name] $dllsAfter[$name] `
            "部署 DLL $name"
    }

    $rawReport = Get-Content -LiteralPath $pendingReport -Raw -Encoding UTF8
    $report = $rawReport | ConvertFrom-Json
    $expectedFormalCommands = [uint64]$SamplePairs * 2L
    $expectedTotalCommands = `
        ([uint64]$WarmupPairs + [uint64]$SamplePairs + 1L) * 2L
    $expectedBackend = $backendReportName
    $expectedEndpoint = switch ($Backend) {
        "Win32" { "" }
        "KmboxNet" { "${KmboxIp}:$KmboxPort" }
        "Makcu" { $MakcuPort }
    }
    $expectedBaudRate = if ($Backend -eq "Makcu") {
        $MakcuBaudRate
    } else {
        0
    }
    $parsedRunUuid = [guid]::Empty
    $provenance = $report.provenance
    $requiredProvenanceFields = @(
        "completion_semantic", "peer_test_boundary",
        "protocol_ack_observed", "physical_effect_observation_method",
        "physical_effect_observed", "aggregation_key")
    if ($null -eq $provenance) {
        throw "鼠标基准报告缺少 completion/peer/effect provenance。"
    }
    foreach ($field in $requiredProvenanceFields) {
        if ($null -eq $provenance.PSObject.Properties[$field]) {
            throw "鼠标基准报告 provenance 缺少字段：$field"
        }
    }
    if ($report.schema -ne 2 -or
        -not [guid]::TryParseExact([string]$report.run_uuid, "D",
                                   [ref]$parsedRunUuid) -or
        [string]$report.run_uuid -ne $runUuid -or
        [string]$provenance.completion_semantic -ne
            $expectedCompletionSemantic -or
        [string]$provenance.peer_test_boundary -ne
            $expectedPeerTestBoundary -or
        [bool]$provenance.protocol_ack_observed -ne
            $expectedProtocolAckObserved -or
        [string]$provenance.physical_effect_observation_method -ne "none" -or
        [bool]$provenance.physical_effect_observed -ne $false -or
        [string]$provenance.aggregation_key -ne $expectedAggregationKey) {
        throw "鼠标基准 run UUID 或 completion/peer/effect 聚合键不匹配，拒绝跨语义合并。"
    }
    if (-not $report.complete -or
        $report.backend -ne $expectedBackend -or
        $report.endpoint -ne $expectedEndpoint -or
        -not $report.authorization.physical_output -or
        -not $report.authorization.confirmation_token -or
        -not $report.command.paired_reverse -or
        [int]$report.command.dx_counts -ne $DxCounts -or
        [int]$report.command.dy_counts -ne $DyCounts -or
        [uint64]$report.configuration.warmup_pairs -ne $WarmupPairs -or
        [uint64]$report.configuration.sample_pairs -ne $SamplePairs -or
        [int]$report.configuration.baud_rate -ne $expectedBaudRate -or
        [uint64]$report.stats.successful_commands -ne $expectedTotalCommands -or
        [uint64]$report.stats.formal_successful_commands -ne `
            $expectedFormalCommands -or
        [uint64]$report.stats.failed_commands -ne 0 -or
        $report.stats.final_status -ne "READY" -or
        @($report.samples).Count -ne $expectedFormalCommands) {
        throw "鼠标基准配置、样本数、失败数或最终状态不符合正式门槛。"
    }
    if ($Backend -eq "KmboxNet" -and
        $rawReport -match [regex]::Escape($KmboxUuid)) {
        throw "鼠标基准报告不得记录 KMBOX UUID。"
    }
    $expectedSequence = [uint64]1
    foreach ($sample in @($report.samples)) {
        $sequence = [uint64]$sample.sequence
        $expectedDirection = if (($sequence % 2L) -eq 1L) { 1 } else { -1 }
        $expectedPair = [uint64](($sequence + 1L) -shr 1)
        if ($sequence -ne $expectedSequence -or
            [uint64]$sample.pair_index -ne $expectedPair -or
            [int]$sample.direction -ne $expectedDirection -or
            [int]$sample.dx_counts -ne $DxCounts * $expectedDirection -or
            [int]$sample.dy_counts -ne $DyCounts * $expectedDirection -or
            [double]$sample.latency_ms -lt 0) {
            throw "鼠标正式样本不是连续、合法的正反 counts 对。"
        }
        ++$expectedSequence
    }
    $timing = $report.timing.command_latency
    if ([double]$timing.mean_ms -lt 0 -or
        [double]$timing.p50_ms -lt 0 -or
        [double]$timing.p95_ms -lt [double]$timing.p50_ms -or
        [double]$timing.p99_ms -lt [double]$timing.p95_ms -or
        [double]$timing.max_ms -lt [double]$timing.p99_ms) {
        throw "鼠标命令耗时汇总非法。"
    }

    $gitCommit = (& $GitExecutable -C $repositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "读取 Git commit 失败。" }
    $gitStatus = @(& $GitExecutable -C $repositoryRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw "读取 Git 工作树状态失败。" }
    $processor = Get-CimInstance Win32_Processor |
        Select-Object -First 1 Name, Manufacturer, NumberOfCores,`
            NumberOfLogicalProcessors, MaxClockSpeed
    $operatingSystem = Get-CimInstance Win32_OperatingSystem |
        Select-Object Caption, Version, BuildNumber, OSArchitecture
    $environment = [ordered]@{
        schema = 2
        run_uuid = $runUuid
        complete = $true
        started_utc = $startedUtc.ToString("o")
        finished_utc = $finishedUtc.ToString("o")
        duration_seconds = ($finishedUtc - $startedUtc).TotalSeconds
        git = [ordered]@{
            commit = $gitCommit
            dirty = $gitStatus.Count -ne 0
            status = $gitStatus
        }
        build = [ordered]@{
            directory = $BuildDirectory
            configuration = $Configuration
            cmake_cache = $cacheAfter
            executable = $executableAfter
            runtime_dlls = $dllsAfter
            clean_path = $true
        }
        machine = [ordered]@{
            computer_name = $env:COMPUTERNAME
            processor = $processor
            operating_system = $operatingSystem
        }
        mouse = [ordered]@{
            backend = $expectedBackend
            endpoint = $report.endpoint
            kmbox_uuid_recorded = $false
            baud_rate = [int]$report.configuration.baud_rate
            connect_timeout_ms = [int]$report.configuration.connect_timeout_ms
            command_timeout_ms = [int]$report.configuration.command_timeout_ms
        }
        benchmark = [ordered]@{
            warmup_pairs = $WarmupPairs
            sample_pairs = $SamplePairs
            dx_counts = $DxCounts
            dy_counts = $DyCounts
            successful_commands = $expectedTotalCommands
            formal_successful_commands = $expectedFormalCommands
            failed_commands = 0
            provenance = [ordered]@{
                completion_semantic =
                    [string]$provenance.completion_semantic
                peer_test_boundary =
                    [string]$provenance.peer_test_boundary
                protocol_ack_observed =
                    [bool]$provenance.protocol_ack_observed
                physical_effect_observation_method =
                    [string]$provenance.physical_effect_observation_method
                physical_effect_observed =
                    [bool]$provenance.physical_effect_observed
                aggregation_key = [string]$provenance.aggregation_key
            }
            timing = $report.timing
        }
        report = Get-FileEvidence $pendingReport
    }
    $environment.report["path"] = $finalReport
    $environmentText = $environment | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText(
        $pendingEnvironment, $environmentText,
        [System.Text.UTF8Encoding]::new($false))

    [System.IO.File]::Move($pendingReport, $finalReport)
    $published.Add($finalReport)
    [System.IO.File]::Move($pendingEnvironment, $finalEnvironment)
    $published.Add($finalEnvironment)
    Write-Host "鼠标正式基准通过："
    Write-Host "  backend=$expectedBackend, commands=$expectedTotalCommands, formal=$expectedFormalCommands, failed=0"
    Write-Host "  semantic=$expectedCompletionSemantic, boundary=$expectedPeerTestBoundary, physical_effect_observed=false"
    Write-Host "  mean=$($timing.mean_ms) ms, P95=$($timing.p95_ms) ms, P99=$($timing.p99_ms) ms"
    Write-Host "  报告：$finalReport"
    Write-Host "  环境清单：$finalEnvironment"
} catch {
    foreach ($path in $published) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    throw
} finally {
    Remove-Item -LiteralPath $pendingReport -Force `
        -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $pendingEnvironment -Force `
        -ErrorAction SilentlyContinue
}
