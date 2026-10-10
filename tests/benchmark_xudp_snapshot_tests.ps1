param([Parameter(Mandatory = $true)][string]$GitExecutable)
$ErrorActionPreference = "Stop"
$root = Join-Path ([IO.Path]::GetTempPath()) ("xen-xudp-snapshot-" + [guid]::NewGuid().ToString("N"))
$build = Join-Path $root "build"
$release = Join-Path $build "Release"
[IO.Directory]::CreateDirectory($release) | Out-Null
try {
    $report = [ordered]@{
        schema = 1; complete = $true; stop_reason = "duration"; elapsed_seconds = 1.0
        destination_url = "udp://127.0.0.1:19876"
        geometry = [ordered]@{ source_width = 32; source_height = 32; encoded_width = 32; encoded_height = 32
            roi_x = 0; roi_y = 0; roi_width = 32; roi_height = 32
            source_pixels_per_pixel_x = 1.0; source_pixels_per_pixel_y = 1.0 }
        stats = [ordered]@{ frames_sent = 1; frames_failed = 0; samples_dropped = 0
            largest_datagram_bytes = 128; datagrams_sent = 1; jpeg_bytes_sent = 20; wire_bytes_sent = 128 }
        timing = [ordered]@{ capture_to_send = [ordered]@{ p95_ms = 0.25 } }
        samples = @([ordered]@{ sequence = 1 })
    }
    $script:validBytes = [Text.Encoding]::UTF8.GetBytes(($report | ConvertTo-Json -Depth 8 -Compress))
    $payload = [Convert]::ToBase64String($script:validBytes)
    # 此独立进程只写合成JSON，无Capture、网络发送或设备调用。正式脚本保持原调用链。
    $fixtureSource = @"
using System;
using System.IO;
public static class XudpReportFixture {
    public static int Main(string[] args) {
        for (int i = 0; i + 1 < args.Length; ++i) {
            if (args[i] == "--report") {
                File.WriteAllBytes(args[i + 1], Convert.FromBase64String("$payload"));
                return 0;
            }
        }
        return 2;
    }
}
"@
    Add-Type -TypeDefinition $fixtureSource -Language CSharp -OutputAssembly (Join-Path $release "XenSender.exe") -OutputType ConsoleApplication
    [IO.File]::WriteAllText((Join-Path $build "CMakeCache.txt"), "synthetic sender fixture")
    $fixtureState = @{ Native = $GitExecutable; Root = $root; Bytes = $script:validBytes; Count = 0; Mutate = $false }
    $function:git = {
        # 正式入口在报告门槛检查后读取Git；这里只暂停于该边界，更改sender拥有的原待发布文件。
        if ($fixtureState.Mutate -and $fixtureState.Count -eq 0) {
            $fixtureState.Count++
            $pending = @(Get-ChildItem -LiteralPath $fixtureState.Root -Filter "race.sender.json.pending-*" -File)
            if ($pending.Count -ne 1) { throw "原报告屏障必须命中唯一待发布文件" }
            $bad = [Text.Encoding]::UTF8.GetString($fixtureState.Bytes) | ConvertFrom-Json
            $bad.complete = $false
            $bad.stats.frames_failed = 1
            [IO.File]::WriteAllText($pending[0].FullName, ($bad | ConvertTo-Json -Depth 8 -Compress), [Text.UTF8Encoding]::new($false))
        }
        & $fixtureState.Native @args
        $global:LASTEXITCODE = $LASTEXITCODE
    }.GetNewClosure()
    $entry = Join-Path $PSScriptRoot "..\scripts\benchmark_xudp_sender.ps1"
    foreach ($case in @("control", "race")) {
        $fixtureState.Mutate = $case -eq "race"
        $prefix = Join-Path $root $case
        & $entry -Destination "udp://127.0.0.1:19876" -ReportPrefix $prefix -BuildDirectory $build `
            -ExpectedSourceWidth 32 -ExpectedSourceHeight 32 -RoiWidth 32 -RoiHeight 32 -DurationSeconds 1 -MinimumFrames 1
        $bytes = [IO.File]::ReadAllBytes("$prefix.sender.json")
        $actual = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
        $environment = Get-Content -LiteralPath "$prefix.sender.environment.json" -Raw -Encoding UTF8 | ConvertFrom-Json
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $expectedHash = [BitConverter]::ToString($sha.ComputeHash($script:validBytes)).Replace("-", "") }
        finally { $sha.Dispose() }
        if (-not $actual.complete -or $actual.stats.frames_failed -ne 0 -or
            $environment.report.sha256 -ne $expectedHash -or $environment.report.length -ne $script:validBytes.Length -or
            [Convert]::ToBase64String($bytes) -ne [Convert]::ToBase64String($script:validBytes)) {
            throw "解析A后发布B：判断、摘要和发布必须来自同一冻结字节；case=$case published_failed=$($actual.stats.frames_failed)"
        }
    }
    if ($fixtureState.Count -ne 1) { throw "未实际进入检查后原路径替换屏障" }
    Write-Host "XUDP实际脚本冻结报告控制组及路径替换通过；零捕获/网络发送/设备输出。"
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith("xen-xudp-snapshot-", [StringComparison]::Ordinal)) {
        [IO.Directory]::Delete($resolved, $true)
    }
}
