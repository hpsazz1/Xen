param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$LaunchScript
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

foreach ($path in @($PrepareScript, $LaunchScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "composite-phase script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "composite-phase script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_b_composite_phase_task',
        'physical_b_composite_phase_calibration',
        '--profile physical-b-composite-phase-calibration',
        'AWAITING_AUXILIARY_PREFLIGHT',
        'final_plan_frozen_on_auxiliary_before_sidecar = $true',
        'same_auxiliary_host_preflight_required = $true',
        'response_revealed_before_final_plan = $false',
        'sequence_sample_count = 295',
        'window_count = 42',
        'negative_control_count = 4',
        'expected_nonzero_transition_count = 38',
        'max_abs_prefix_x_counts = 1',
        'minimum_coverage_frames = $minimumSidecarFrames',
        'production_aim_changed = $false',
        'fixed_pixel_speed_used_as_gate = $false',
        'PREPARED_NOT_LAUNCHED',
        'physical_launch_executed = $false',
        'XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT')) {
    if (-not $prepare.Contains($required)) {
        throw "composite-phase Prepare is missing contract text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw "composite-phase Prepare must not execute Launch"
}

foreach ($required in @(
        '$isBCompositeTask',
        'XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT',
        'XenMouseEffectProbeCompositeSeal.exe',
        '--plan-seed',
        '--preflight-output',
        '--plan-output',
        '--composite-plan',
        '--composite-plan-sha256',
        '--composite-schedule-ledger',
        'PHASE_CONFIRMED',
        '[uint64]$task.sidecar.minimum_coverage_frames -ne 1735',
        '[int]$sequence.schema -ne 7',
        '$samples.Count -ne 295',
        '$sequenceWindows.Count -ne 42')) {
    if (-not $launch.Contains($required)) {
        throw "composite-phase Launch is missing contract text: $required"
    }
}
$sealIndex = $launch.IndexOf('    Invoke-CompositeSeal -Executable')
$sidecarIndex = $launch.IndexOf(
    '$sidecarProcess = Start-Process -FilePath')
if ($sealIndex -lt 0 -or $sidecarIndex -lt 0 -or
    $sealIndex -ge $sidecarIndex) {
    throw "scheduler preflight/final plan must happen before sidecar start"
}
if ($launch.Contains('& ([string]$task.files.ledger_producer.path)') -or
    $launch.Contains('& ([string]$task.files.binder.path)') -or
    $launch.Contains('& ([string]$task.files.evaluator.path)')) {
    throw "Launch must not derive or evaluate composite response evidence"
}

# 只提取生产 Seal 调用函数；无设备 fixture 覆盖 PS5 错误流和参数传递，
# 不执行 Launch 主体，也不把合成文件当作 scheduler 或物理验收。
$tokens = $null
$errors = $null
$launchAst = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $LaunchScript).Path,
    [ref]$tokens,
    [ref]$errors)
foreach ($functionName in @('Quote-NativeArgument', 'Invoke-CompositeSeal')) {
    $functionAst = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $functionName
    }, $true)
    if ($null -eq $functionAst) {
        throw "Launch 缺少可测试的 Seal 函数：$functionName"
    }
    . ([scriptblock]::Create($functionAst.Extent.Text))
}

$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).
    TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$fixtureRoot = Join-Path $temporaryParent (
    'xen-composite-seal-caller-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
try {
    $fixtureExecutable = Join-Path $fixtureRoot 'Seal fixture 中文.exe'
    $fixtureSource = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class CompositeSealCallerFixture
{
    public static int Main(string[] args)
    {
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput(),
            new UTF8Encoding(false)) { AutoFlush = true });
        Console.SetError(new StreamWriter(Console.OpenStandardError(),
            new UTF8Encoding(false)) { AutoFlush = true });
        if (args.Length != 12) return 91;
        var values = new Dictionary<string, string>();
        for (int index = 0; index < args.Length; index += 2)
            values.Add(args[index], args[index + 1]);
        if (values["--run-uuid"] != "23a07885-8191-424a-9f8b-6d3fbc4e73ab" ||
            values["--activation-epoch"] != "1788597555949" ||
            File.ReadAllText(values["--sequence"], Encoding.UTF8) != "序列 fixture")
            return 92;
        string mode = File.ReadAllText(values["--plan-seed"], Encoding.UTF8);
        if (mode == "stderr-error")
        {
            Console.Error.WriteLine("scheduler preflight 失败: preflight lateness/width/total active budget 超限: event=16, lateness_ns=176400, marker_width_ns=0, active_ns=0, active_total_before_ns=1342100");
            return 3;
        }
        if (mode == "empty-error") return 3;
        if (mode == "stdout-error")
        {
            Console.WriteLine("仅标准输出中的失败详情");
            return 3;
        }
        if (mode == "large-output")
        {
            Console.Write(new string('出', 131072));
            Console.Error.Write(new string('错', 131072));
        }
        if (mode != "missing-preflight")
            File.WriteAllText(values["--preflight-output"], "{}", new UTF8Encoding(false));
        if (mode != "missing-plan")
            File.WriteAllText(values["--plan-output"], "{}", new UTF8Encoding(false));
        Console.WriteLine("合成输出已完成");
        Console.Error.WriteLine("合成诊断行不替代退出码");
        return 0;
    }
}
'@
    Add-Type -TypeDefinition $fixtureSource -Language CSharp `
        -OutputAssembly $fixtureExecutable -OutputType ConsoleApplication

    $utf8 = [Text.UTF8Encoding]::new($false)
    $expectedFailure = 'scheduler preflight 失败: preflight lateness/width/total active budget 超限: event=16, lateness_ns=176400, marker_width_ns=0, active_ns=0, active_total_before_ns=1342100'
    $cases = @(
        @{ name = 'stderr-error'; rejected = $true; detail = $expectedFailure; exit_code = 3 },
        @{ name = 'missing-preflight'; rejected = $true; detail = ''; exit_code = 0 },
        @{ name = 'missing-plan'; rejected = $true; detail = ''; exit_code = 0 },
        @{ name = 'success'; rejected = $false; detail = ''; exit_code = 0 },
        @{ name = 'empty-error'; rejected = $true; detail = '<empty>'; exit_code = 3 },
        @{ name = 'stdout-error'; rejected = $true; detail = '仅标准输出中的失败详情'; exit_code = 3 },
        @{ name = 'start-failure'; rejected = $true; detail = 'composite seal 启动失败'; exit_code = $null },
        @{ name = 'large-output'; rejected = $false; detail = ''; exit_code = 0 }
    )
    foreach ($case in $cases) {
        $caseRoot = Join-Path $fixtureRoot $case.name
        [void][IO.Directory]::CreateDirectory($caseRoot)
        $planSeed = Join-Path $caseRoot 'plan seed 中文.json'
        $sequence = Join-Path $caseRoot 'sequence 中文.json'
        $preflight = Join-Path $caseRoot 'scheduler preflight 中文.json'
        $plan = Join-Path $caseRoot 'final plan 中文.json'
        [IO.File]::WriteAllText($planSeed, $case.name, $utf8)
        [IO.File]::WriteAllText($sequence, '序列 fixture', $utf8)
        $executable = if ($case.name -eq 'start-failure') {
            Join-Path $caseRoot 'absent seal.exe'
        } else { $fixtureExecutable }
        $caught = $null
        $followingReached = $false
        $previousOutputEncoding = [Console]::OutputEncoding
        try {
            # 强制父进程使用非 UTF-8，验证 Seal 中文来自显式管道解码。
            [Console]::OutputEncoding = [Text.Encoding]::ASCII
            Invoke-CompositeSeal -Executable $executable `
                -PlanSeed $planSeed -Sequence $sequence `
                -PreflightOutput $preflight -PlanOutput $plan `
                -RunUuid '23a07885-8191-424a-9f8b-6d3fbc4e73ab' `
                -ActivationEpoch '1788597555949'
            $followingReached = $true
        } catch { $caught = $_ }
        finally { [Console]::OutputEncoding = $previousOutputEncoding }
        if ($case.rejected) {
            if ($null -eq $caught -or $followingReached) {
                throw "Seal 失败后仍到达后继操作：$($case.name)"
            }
            $message = [string]$caught.Exception.Message
            if ($caught.FullyQualifiedErrorId -like '*NativeCommandError*' -or
                -not $message.Contains($case.detail) -or
                ($null -ne $case.exit_code -and
                    -not $message.Contains("ExitCode=$($case.exit_code)"))) {
                throw "Seal caller 诊断不完整：case=$($case.name)；ErrorId=$($caught.FullyQualifiedErrorId)；$message"
            }
        } elseif ($null -ne $caught -or -not $followingReached -or
            -not (Test-Path -LiteralPath $preflight -PathType Leaf) -or
            -not (Test-Path -LiteralPath $plan -PathType Leaf)) {
            throw "Seal 合成成功路径被拒绝：$($case.name)；$caught"
        }
        Write-Host "Seal caller 合同通过：$($case.name)"
    }
} finally {
    $resolvedFixtureRoot = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolvedFixtureRoot.StartsWith(
            $temporaryParent, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedFixtureRoot) -notmatch
            '^xen-composite-seal-caller-[0-9a-f]{32}$') {
        throw '拒绝清理不属于当前 Seal caller 测试的目录'
    }
    if (Test-Path -LiteralPath $resolvedFixtureRoot -PathType Container) {
        Remove-Item -LiteralPath $resolvedFixtureRoot -Recurse -Force
    }
}

Write-Host "Physical B composite-phase Prepare/Launch contract passed."
