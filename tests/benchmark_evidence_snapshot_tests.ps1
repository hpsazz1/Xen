param([Parameter(Mandatory = $true)][string]$FixtureExecutable, [switch]$NetworkOnly)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$repository = Split-Path -Parent $PSScriptRoot
$entry = [IO.Path]::GetFullPath((Join-Path $repository 'scripts/benchmark_runtime.ps1'))
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-bench-bytes-' + [guid]::NewGuid().ToString('N'))
$release = Join-Path $root 'build/Release'
$sdk = Join-Path $root 'sdk'
[IO.Directory]::CreateDirectory($release) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $sdk 'a/b/c')) | Out-Null
$template = Join-Path $root 'template'
& $FixtureExecutable $template
if ($LASTEXITCODE -ne 0) { throw '实际DebugReport合成写入失败' }
$profileText = '[{"cat":"Node","args":{"provider":"CUDAExecutionProvider"}}]'
[IO.File]::WriteAllText("$template.profile.json", $profileText, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $release 'fixture.dll'), '不可加载的合成DLL来源快照')
Copy-Item -LiteralPath (Join-Path $release 'fixture.dll') -Destination $sdk
$cache = @("ONNXRUNTIME_ROOT:PATH=$sdk", "OpenCV_DIR:PATH=$(Join-Path $sdk 'a/b/c')", "XEN_CUDNN_ROOT:PATH=$sdk", "XEN_CUDA_ROOT:PATH=$sdk") -join "`n"
[IO.File]::WriteAllText((Join-Path $root 'build/CMakeCache.txt'), $cache)
$model = Join-Path $root 'model.onnx'
[IO.File]::WriteAllText($model, '不可加载的合成模型来源快照')
$escapedTemplate = $template.Replace('\', '\\')
$writer = @"
using System;
using System.IO;
public static class BenchmarkEvidenceFixture {
    public static int Main(string[] args) {
        string prefix = null, profile = null, backend = null;
        for (int i = 0; i + 1 < args.Length; ++i) {
            if (args[i] == "--report-prefix") prefix = args[i + 1];
            if (args[i] == "--provider-profile") profile = args[i + 1];
            if (args[i] == "--backend") backend = args[i + 1];
        }
        if (prefix == null) return 2;
        if (backend == "cpu") {
            File.WriteAllText(prefix + ".csv", File.ReadAllText("$escapedTemplate.csv").Replace("CUDAExecutionProvider", "CPUExecutionProvider"));
            File.WriteAllText(prefix + ".json", File.ReadAllText("$escapedTemplate.json").Replace("CUDAExecutionProvider", "CPUExecutionProvider"));
        } else {
            File.Copy("$escapedTemplate.csv", prefix + ".csv");
            File.Copy("$escapedTemplate.json", prefix + ".json");
        }
        if (profile != null) File.Copy("$escapedTemplate.profile.json", profile);
        return 0;
    }
}
"@
Add-Type -TypeDefinition $writer -Language CSharp -OutputAssembly (Join-Path $release 'XenBenchmark.exe') -OutputType ConsoleApplication

function Find-Line([string]$Text) {
    $lines = [IO.File]::ReadAllLines($entry)
    for ($i = 0; $i -lt $lines.Length; ++$i) {
        if ($lines[$i].Trim() -eq $Text) { return $i + 1 }
    }
    throw "实际脚本屏障未找到：$Text"
}
function Bytes-Hash([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '') }
    finally { $sha.Dispose() }
}
$failed = 0
if ($NetworkOnly) {
    $entry = [IO.Path]::GetFullPath((Join-Path $repository 'scripts/benchmark_network_receiver.ps1'))
    try {
        foreach ($case in @('control', 'cpu-control', 'json-before-read', 'csv-before-read', 'profile-before-read', 'config-before-read', 'json-after-read', 'csv-after-read', 'profile-after-read', 'config-after-read', 'environment-after-read', 'json-after-rename')) {
            $prefix = Join-Path $root "$case/result"
            $isControl = $case -in @('control', 'cpu-control')
            $backend = if ($case -eq 'cpu-control') { 'cpu' } else { 'cuda' }
            $global:XenNetworkEvidenceFixture = @{ Case = $case; Prefix = $prefix; Hits = 0; Blocked = $false }
            $breakpoint = $null
            if (-not $isControl) {
                $boundary = if ($case -like '*-before-read') { '$reportPath = "$ReportPrefix.json"' }
                else { '$snapshot = $report.final_snapshot' }
                $breakpoint = Set-PSBreakpoint -Script $entry -Line (Find-Line $boundary) -Action {
                    $state = $global:XenNetworkEvidenceFixture
                    ++$state.Hits
                    $kind = $state.Case.Split('-')[0]
                    $suffix = switch ($kind) {
                        'json' { '.json' }; 'csv' { '.csv' }; 'profile' { '.provider-profile.json' }
                        'config' { '.receiver.ini' }; 'environment' { '.environment.json' }
                    }
                    $path = $state.Prefix + $suffix
                    if ($kind -eq 'json') {
                        $reportB = [IO.File]::ReadAllText($path) | ConvertFrom-Json
                        $reportB.samples[0].mouse_sent = $true
                        $bytes = [Text.Encoding]::UTF8.GetBytes(($reportB | ConvertTo-Json -Depth 30))
                    } else { $bytes = [Text.Encoding]::UTF8.GetBytes('被替换的未验证B字节') }
                    try {
                        if ($state.Case -eq 'json-after-rename') { [IO.File]::Move($path, $path + '.other-writer') }
                        [IO.File]::WriteAllBytes($path, $bytes)
                    }
                    catch {
                        $inner = $_.Exception.InnerException
                        if ($null -ne $inner -and ($inner.HResult -band 0xffff) -in @(32, 33)) { $state.Blocked = $true }
                        else { throw }
                    }
                }
            }
            $rejection = ''
            try {
                & $entry -ModelPath $model -ReportPrefix $prefix -BuildDirectory (Join-Path $root 'build') -Backend $backend -WarmupSamples 0 -MinimumSamples 1 -MinimumSeconds 0 -MaximumSeconds 1 -EnableFp16 off -EnableCudaGraph off -EnableGpuPreprocess off
            } catch { $rejection = $_.Exception.Message }
            finally { if ($null -ne $breakpoint) { Remove-PSBreakpoint -Breakpoint $breakpoint } }
            try {
                $state = $global:XenNetworkEvidenceFixture
                if (-not $isControl -and $state.Hits -ne 1) { throw '没有命中外层实际消费屏障' }
                if ($case -like '*-before-read') {
                    if ($rejection -notmatch '路径或摘要' -or [IO.File]::Exists("$prefix.network.json")) { throw '环境A与产物B混配未因绑定失败拒绝' }
                    foreach ($suffix in @('.json', '.csv', '.provider-profile.json', '.environment.json')) {
                        if (-not [IO.File]::Exists($prefix + $suffix)) { throw '外层错误删除了Runtime产物' }
                    }
                } else {
                    if ($rejection) { throw $rejection }
                    if (-not $isControl -and -not $state.Blocked) { throw '已验证文件在标记发布前仍可被替换' }
                    $marker = [IO.File]::ReadAllText("$prefix.network.json") | ConvertFrom-Json
                    foreach ($artifact in @('receiver_config', 'runtime_report', 'runtime_environment', 'provider_profile')) {
                        $evidence = $marker.artifacts.$artifact
                        if ($artifact -eq 'provider_profile' -and $backend -eq 'cpu') {
                            if ($null -ne $evidence) { throw 'CPU控制不得捏造profile' }
                            continue
                        }
                        if ($evidence.sha256 -ne (Bytes-Hash ([IO.File]::ReadAllBytes($evidence.path)))) { throw "$artifact 标记摘要与固定字节不符" }
                    }
                    $environment = [IO.File]::ReadAllText("$prefix.environment.json") | ConvertFrom-Json
                    if ($environment.report.csv_sha256 -ne (Bytes-Hash ([IO.File]::ReadAllBytes("$prefix.csv")))) { throw 'CSV没有绑定环境中的摘要' }
                }
                foreach ($suffix in @('.receiver.ini', '.json', '.csv', '.environment.json', '.provider-profile.json')) {
                    if ([IO.File]::Exists($prefix + $suffix)) {
                        $released = [IO.File]::Open($prefix + $suffix, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                        $released.Dispose()
                    }
                }
                Write-Host "network-${case}: PASS"
            } catch { ++$failed; Write-Host "network-${case}: FAIL $($_.Exception.Message)" }
        }
        if ($failed) { throw "$failed 个正式network证据绑定案例失败" }
    } finally {
        Remove-Variable -Name XenNetworkEvidenceFixture -Scope Global -ErrorAction SilentlyContinue
        Write-Host "合成network夹具保留：$root"
    }
    exit 0
}
try {
    foreach ($case in @('control', 'profile-parse-hash', 'profile-hash-move', 'json-parse-hash', 'json-hash-move', 'csv-parse-hash', 'csv-hash-move')) {
        $prefix = Join-Path $root $case
        $global:XenBenchmarkSnapshotFixture = @{ Paths = @{}; Captures = 0; Mutations = 0; Case = $case }
        $capture = Set-PSBreakpoint -Script $entry -Line (Find-Line '$finishedUtc = [DateTime]::UtcNow') -Action {
            $state = $global:XenBenchmarkSnapshotFixture
            ++$state.Captures
            $state.Paths = @{ csv = $pendingCsv; json = $pendingJson; profile = $pendingProviderProfile }
        }
        $mutate = $null
        if ($case -ne 'control') {
            $boundary = if ($case -like '*-hash-move') {
                '$published = New-Object System.Collections.Generic.List[string]'
            } elseif ($case -like 'profile-*') { '$events = @($document)' }
            elseif ($case -like 'json-*') { '$expectedProviders = @{' }
            else { '$csvRetention = Get-XenRuntimeCsvRetentionMetadata -Lines $csvLines' }
            $mutate = Set-PSBreakpoint -Script $entry -Line (Find-Line $boundary) -Action {
                $state = $global:XenBenchmarkSnapshotFixture
                ++$state.Mutations
                $kind = $state.Case.Split('-')[0]
                # 只替换实际进程原始输出路径，不能修改未来实现的独立冻结副本。
                [IO.File]::WriteAllText($state.Paths[$kind], '被替换的未验证B字节', [Text.UTF8Encoding]::new($false))
            }
        }
        try {
            & $entry -ModelPath $model -ReportPrefix $prefix -BuildDirectory (Join-Path $root 'build') -Backend cuda -WarmupSamples 0 -MinimumSamples 1 -MinimumSeconds 0 -MaximumSeconds 1 -ExpectedCaptureBackend xudp_jpeg -ExpectedEncodedWidth 320 -ExpectedEncodedHeight 320 -EnableFp16 off -EnableCudaGraph off -EnableGpuPreprocess off
            $environment = [IO.File]::ReadAllText("$prefix.environment.json") | ConvertFrom-Json
            foreach ($kind in @('csv', 'json', 'profile')) {
                $inputPath = if ($kind -eq 'profile') { "$template.profile.json" } else { "$template.$kind" }
                $outputPath = if ($kind -eq 'profile') { "$prefix.provider-profile.json" } else { "$prefix.$kind" }
                $expected = [IO.File]::ReadAllBytes($inputPath)
                $actual = [IO.File]::ReadAllBytes($outputPath)
                $recordedHash = if ($kind -eq 'profile') { $environment.benchmark.provider_execution.profile.file.sha256 } else { $environment.report."${kind}_sha256" }
                if ([Convert]::ToBase64String($expected) -ne [Convert]::ToBase64String($actual) -or $recordedHash -ne (Bytes-Hash $expected)) {
                    throw "$kind 解析/摘要/发布不是同一份A字节"
                }
                if ($kind -eq 'profile' -and
                    $environment.benchmark.provider_execution.profile.file.last_write_utc -ne [IO.File]::GetLastWriteTimeUtc($outputPath).ToString('o')) { throw '发布profile元数据不匹配' }
            }
            if ($global:XenBenchmarkSnapshotFixture.Captures -ne 1 -or
                ($case -ne 'control' -and $global:XenBenchmarkSnapshotFixture.Mutations -ne 1)) { throw '未实际命中生产脚本交错边界' }
            Write-Host "${case}: PASS"
        } catch { ++$failed; Write-Host "${case}: FAIL $($_.Exception.Message)" }
        finally {
            Remove-PSBreakpoint -Breakpoint $capture
            if ($null -ne $mutate) { Remove-PSBreakpoint -Breakpoint $mutate }
        }
    }
    foreach ($case in @('invalid-profile', 'invalid-json', 'copy-collision', 'publish-collision')) {
        $prefix = Join-Path $root "$case/result"
        $global:XenBenchmarkSnapshotFixture = @{ Case = $case; Hits = 0; Foreign = ''; Prefix = $prefix }
        $boundary = if ($case -eq 'copy-collision') { '$OwnedCopies.Add($copy) | Out-Null' }
        elseif ($case -eq 'publish-collision') { '$published = New-Object System.Collections.Generic.List[string]' }
        else { '$finishedUtc = [DateTime]::UtcNow' }
        $breakpoint = Set-PSBreakpoint -Script $entry -Line (Find-Line $boundary) -Action {
            $state = $global:XenBenchmarkSnapshotFixture
            ++$state.Hits
            if ($state.Case -eq 'invalid-profile') {
                [IO.File]::WriteAllText($pendingProviderProfile, '[{"cat":"Node","args":{"provider":"CPUExecutionProvider"}}]')
            } elseif ($state.Case -eq 'invalid-json') {
                [IO.File]::WriteAllText($pendingJson, 'invalid JSON')
            } else {
                $state.Foreign = if ($state.Case -eq 'copy-collision') { $copy.path } else { $state.Prefix + '.csv' }
                [IO.File]::WriteAllText($state.Foreign, '另一个任务的文件，不得覆盖或清理')
            }
        }
        $rejected = $false
        try {
            & $entry -ModelPath $model -ReportPrefix $prefix -BuildDirectory (Join-Path $root 'build') -Backend cuda -WarmupSamples 0 -MinimumSamples 1 -MinimumSeconds 0 -MaximumSeconds 1 -ExpectedCaptureBackend xudp_jpeg -ExpectedEncodedWidth 320 -ExpectedEncodedHeight 320 -EnableFp16 off -EnableCudaGraph off -EnableGpuPreprocess off
        } catch { $rejected = $true }
        finally { Remove-PSBreakpoint -Breakpoint $breakpoint }
        try {
            $state = $global:XenBenchmarkSnapshotFixture
            if (-not $rejected -or $state.Hits -ne 1 -or [IO.File]::Exists("$prefix.environment.json")) { throw '失败入口未拒绝或发布了完成标记' }
            if ($state.Foreign -and [IO.File]::ReadAllText($state.Foreign) -ne '另一个任务的文件，不得覆盖或清理') { throw '外来文件未保留' }
            $unexpected = @(Get-ChildItem -LiteralPath (Split-Path -Parent $prefix) -File |
                Where-Object { $_.FullName -ne $state.Foreign })
            if ($unexpected.Count -ne 0) { throw '自有冻结副本、原pending或部分发布文件未清理' }
            Write-Host "${case}: PASS 拒绝、清理与外来文件保留"
        } catch { ++$failed; Write-Host "${case}: FAIL $($_.Exception.Message)" }
    }
    if ($failed) { throw "$failed 个正式Runtime脚本字节绑定案例失败" }
} finally {
    Remove-Variable -Name XenBenchmarkSnapshotFixture -Scope Global -ErrorAction SilentlyContinue
    Write-Host "合成夹具保留：$root"
}
