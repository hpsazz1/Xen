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
foreach ($functionName in @('Quote-NativeArgument', 'Invoke-Utf8NativeProcess', 'Invoke-CompositeSeal')) {
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
    # 执行生产 Launch 在 Seal/sidecar 之前的 seed/sequence 消费块；只读本地合成 JSON。
    # 缺失校验必须表现为混包被接受的反例，不能只用脚本文本包含常量证明。
    $policyFunction = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Assert-CompositeSchedulerPolicy'
    }, $true)
    if ($null -ne $policyFunction) {
        . ([scriptblock]::Create($policyFunction.Extent.Text))
    }
    $seedConsumer = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Extent.Text.StartsWith('if ($isBCompositeTask) {') -and
            $node.Extent.Text.Contains('$planSeed = Get-Content')
    }, $true)
    if ($null -eq $seedConsumer -or $seedConsumer.Extent.StartOffset -ge $sealIndex) {
        throw 'Launch 必须先消费 seed/sequence 再调用 Seal'
    }
    $legacyPolicy = [ordered]@{
        timer_mode = 'HIGH_RESOLUTION_ONE_SHOT_OR_FAIL'
        active_guard_ns = 300000; max_wake_lateness_ns = 150000
        max_event_interval_width_ns = 100000
        max_active_wait_ns_per_event = 350000; max_active_wait_ns_total = 14700000
    }
    $resourcePolicy = [ordered]@{
        timer_mode = 'HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1'
        active_guard_ns = 1000000; max_wake_lateness_ns = 150000
        max_event_interval_width_ns = 100000
        max_active_wait_ns_per_event = 1000000; max_active_wait_ns_total = 42000000
    }
    $isBCompositeTask = $true
    $task = [pscustomobject]@{
        run_uuid = 'policy-fixture'; activation_epoch = 1; scope_id = 'scope'
        sequence_sha256 = 'sequence-semantic'
        files = [pscustomobject]@{
            plan_seed = [pscustomobject]@{ path = (Join-Path $fixtureRoot 'seed.json') }
            capture_policy = [pscustomobject]@{ path = (Join-Path $fixtureRoot 'capture.json') }
            sequence = [pscustomobject]@{ path = (Join-Path $fixtureRoot 'sequence.json'); sha256 = 'sequence-file' }
            binder = [pscustomobject]@{ sha256 = 'binder' }
            evaluator = [pscustomobject]@{ sha256 = 'evaluator' }
            ledger_producer = [pscustomobject]@{ sha256 = 'producer' }
            composite_seal_executable = [pscustomobject]@{ sha256 = 'seal' }
        }
    }
    [IO.File]::WriteAllText($task.files.capture_policy.path,
        '{"semantic_sha256":"capture"}', [Text.UTF8Encoding]::new($false))
    $policyCases = @(
        @{ name = 'legacy'; sequence = $legacyPolicy; seed = $legacyPolicy; reject = $false },
        @{ name = 'mixed-seed'; sequence = $resourcePolicy; seed = $legacyPolicy; reject = $true },
        @{ name = 'active-1ms'; sequence = $resourcePolicy; seed = $resourcePolicy; reject = $false },
        @{ name = 'mixed-sequence'; sequence = $legacyPolicy; seed = $resourcePolicy; reject = $true }
    )
    foreach ($original in @($legacyPolicy, $resourcePolicy)) {
        foreach ($field in @('active_guard_ns', 'max_wake_lateness_ns',
                'max_event_interval_width_ns', 'max_active_wait_ns_per_event', 'max_active_wait_ns_total')) {
            $mutated = [ordered]@{}
            foreach ($key in $original.Keys) { $mutated[$key] = $original[$key] }
            $mutated[$field] = [int64]$mutated[$field] + 1
            $policyCases += @{ name = "$($original.timer_mode)-$field"; sequence = $mutated; seed = $mutated; reject = $true }
        }
    }
    foreach ($value in @('HIGH_RESOLUTION_ONE_SHOT_UNKNOWN', 'high_resolution_one_shot_or_fail')) {
        $mutated = [ordered]@{}
        foreach ($key in $legacyPolicy.Keys) { $mutated[$key] = $legacyPolicy[$key] }
        $mutated.timer_mode = $value
        $policyCases += @{ name = $value; sequence = $mutated; seed = $mutated; reject = $true }
    }
    foreach ($kind in @('string-budget', 'missing-budget', 'missing-mode')) {
        $mutated = [ordered]@{}
        foreach ($key in $resourcePolicy.Keys) { $mutated[$key] = $resourcePolicy[$key] }
        switch ($kind) {
            'string-budget' { $mutated.active_guard_ns = '1000000' }
            'missing-budget' { $mutated.Remove('active_guard_ns') }
            'missing-mode' { $mutated.Remove('timer_mode') }
        }
        $policyCases += @{ name = $kind; sequence = $mutated; seed = $mutated; reject = $true }
    }
    foreach ($case in $policyCases) {
        $seedPolicy = [ordered]@{}
        foreach ($key in $case.seed.Keys) { $seedPolicy[$key] = $case.seed[$key] }
        $seedPolicy.preflight_file_sha256 = $null
        $seedDocument = [ordered]@{
            schema_version = 1; evidence_type = 'mouse_effect_probe_b_composite_phase_calibration_plan'
            status = 'AWAITING_AUXILIARY_PREFLIGHT'; physical_output_capability = $false
            physical_dispatch_count = 0; production_aim_changed = $false; new_production_gain_claimed = $false
            run_uuid = 'policy-fixture'; activation_epoch = 1; scope_id = 'scope'; frozen_at_utc_unix_ns = $null
            sequence_binding = @{
                sequence_profile = 'physical_b_composite_phase_calibration'; sample_count = 295; window_count = 42
                sequence_file_sha256 = 'sequence-file'; sequence_semantic_sha256 = 'sequence-semantic'
            }
            scheduler_policy = $seedPolicy; capture_policy = @{ semantic_sha256 = 'capture' }
            seal = @{
                binder_file_sha256 = 'binder'; evaluator_file_sha256 = 'evaluator'; producer_file_sha256 = 'producer'
                report_verifier_file_sha256 = 'seal'; response_revealed_before_freeze = $false
            }
        }
        [IO.File]::WriteAllText($task.files.plan_seed.path,
            ($seedDocument | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($task.files.sequence.path,
            (@{ request = $case.sequence } | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
        $rejected = $false
        try { . ([scriptblock]::Create($seedConsumer.Extent.Text)) } catch { $rejected = $true }
        if ($rejected -ne $case.reject) {
            throw "Launch pre-Seal scheduler 消费结果无效：$($case.name), rejected=$rejected"
        }
        Write-Host "Launch pre-Seal scheduler 合同通过：$($case.name)"
    }

    # 执行实际 Seal 输出回读中的两条策略校验语句；不执行 Seal 或 sidecar。
    $policyCalls = @($launchAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Assert-CompositeSchedulerPolicy' -and
            $node.Extent.StartOffset -gt $sealIndex -and
            $node.Extent.StartOffset -lt $sidecarIndex
    }, $true))
    if ($policyCalls.Count -ne 2) { throw 'Seal 输出回读必须在 sidecar 前核 preflight/final 两份策略' }
    $compositeTimerMode = 'HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1'
    foreach ($case in @(
            @{ name = 'sealed-new'; preflight = $resourcePolicy; plan = $resourcePolicy; reject = $false },
            @{ name = 'old-preflight'; preflight = $legacyPolicy; plan = $resourcePolicy; reject = $true },
            @{ name = 'old-final-plan'; preflight = $resourcePolicy; plan = $legacyPolicy; reject = $true })) {
        $schedulerPreflight = $case.preflight | ConvertTo-Json | ConvertFrom-Json
        $compositePlan = @{ scheduler_policy = $case.plan } | ConvertTo-Json -Depth 5 | ConvertFrom-Json
        $rejected = $false
        try {
            foreach ($call in $policyCalls) { $null = . ([scriptblock]::Create($call.Extent.Text)) }
        } catch { $rejected = $true }
        if ($rejected -ne $case.reject) { throw "Seal 输出策略消费无效：$($case.name)" }
    }

    $autoArmFunction = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Get-BoundedCompositeAutoArm'
    }, $true)
    if ($null -eq $autoArmFunction) { throw 'Launch 缺少显式 bounded composite 自动取证消费入口' }
    . ([scriptblock]::Create($autoArmFunction.Extent.Text))
    $autoArmCall = @($launchAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Get-BoundedCompositeAutoArm'
    }, $true))
    if ($autoArmCall.Count -ne 1 -or $autoArmCall[0].Extent.StartOffset -ge $seedConsumer.Extent.StartOffset) {
        throw 'Launch 必须在 seed/Seal 前消费自动取证授权标志'
    }
    $autoCases = @(
        @{ name = 'old-composite'; composite = $true; flag = $null; deadman = $true; frontend = $true; seconds = 15; enabled = $false; reject = $false },
        @{ name = 'explicit-manual'; composite = $true; flag = $false; deadman = $true; frontend = $true; seconds = 15; enabled = $false; reject = $false },
        @{ name = 'bounded'; composite = $true; flag = $true; deadman = $false; frontend = $false; seconds = 15; enabled = $true; reject = $false },
        @{ name = 'bounded-with-deadman'; composite = $true; flag = $true; deadman = $true; frontend = $false; seconds = 15; enabled = $false; reject = $true },
        @{ name = 'bounded-with-frontend'; composite = $true; flag = $true; deadman = $false; frontend = $true; seconds = 15; enabled = $false; reject = $true },
        @{ name = 'bounded-overlong'; composite = $true; flag = $true; deadman = $false; frontend = $false; seconds = 16; enabled = $false; reject = $true },
        @{ name = 'bounded-other-profile'; composite = $false; flag = $true; deadman = $false; frontend = $false; seconds = 15; enabled = $false; reject = $true },
        @{ name = 'unflagged-no-deadman'; composite = $true; flag = $null; deadman = $false; frontend = $true; seconds = 15; enabled = $false; reject = $true },
        @{ name = 'string-flag'; composite = $true; flag = 'true'; deadman = $false; frontend = $false; seconds = 15; enabled = $false; reject = $true }
    )
    foreach ($case in $autoCases) {
        $safety = @{ right_button_deadman_required = $case.deadman }
        if ($null -ne $case.flag) { $safety.bounded_composite_auto_arm = $case.flag }
        $autoTask = @{ safety = $safety; sidecar = @{ max_seconds = $case.seconds }
            requires_user_frontend_launch = $case.frontend } |
            ConvertTo-Json -Depth 5 | ConvertFrom-Json
        $rejected = $false
        $enabled = $false
        try { $enabled = Get-BoundedCompositeAutoArm $autoTask $case.composite }
        catch { $rejected = $true }
        if ($rejected -ne $case.reject -or (-not $rejected -and $enabled -ne $case.enabled)) {
            throw "Launch bounded 自动取证消费结果无效：$($case.name)"
        }
        Write-Host "Launch bounded 自动取证合同通过：$($case.name)"
    }
    $eventMonitorFunction = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Get-BoundedCompositeEventMonitor'
    }, $true)
    if ($null -eq $eventMonitorFunction) { throw 'Launch 缺少独立事件监控授权入口' }
    . ([scriptblock]::Create($eventMonitorFunction.Extent.Text))
    $eventMonitorCalls = @($launchAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Get-BoundedCompositeEventMonitor'
    }, $true))
    if ($eventMonitorCalls.Count -ne 1 -or $eventMonitorCalls[0].Extent.StartOffset -ge $seedConsumer.Extent.StartOffset) {
        throw '事件监控授权必须在 seed/Seal/sidecar 前消费'
    }
    foreach ($case in @(
            @{ name = 'legacy-missing'; flag = $null; auto = $false; composite = $true; expected = $false; reject = $false },
            @{ name = 'auto-missing'; flag = $null; auto = $true; composite = $true; expected = $false; reject = $false },
            @{ name = 'explicit-false'; flag = $false; auto = $true; composite = $true; expected = $false; reject = $false },
            @{ name = 'event'; flag = $true; auto = $true; composite = $true; expected = $true; reject = $false },
            @{ name = 'no-auto'; flag = $true; auto = $false; composite = $true; expected = $false; reject = $true },
            @{ name = 'other-profile'; flag = $true; auto = $true; composite = $false; expected = $false; reject = $true },
            @{ name = 'string-true'; flag = 'true'; auto = $true; composite = $true; expected = $false; reject = $true },
            @{ name = 'number-true'; flag = 1; auto = $true; composite = $true; expected = $false; reject = $true },
            @{ name = 'string-false'; flag = 'false'; auto = $true; composite = $true; expected = $false; reject = $true })) {
        $safety = @{}
        if ($null -ne $case.flag) { $safety.bounded_composite_event_monitor = $case.flag }
        $eventTask = @{ safety = $safety } | ConvertTo-Json -Depth 4 | ConvertFrom-Json
        $rejected = $false; $enabled = $false
        try { $enabled = Get-BoundedCompositeEventMonitor $eventTask $case.composite $case.auto }
        catch { $rejected = $true }
        if ($rejected -ne $case.reject -or (-not $rejected -and $enabled -ne $case.expected)) {
            throw "独立事件监控授权消费无效：$($case.name)"
        }
        Write-Host "Launch 事件监控授权合同通过：$($case.name)"
    }
    $argumentConsumer = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Extent.Text.StartsWith('if ($isBCompositeTask) {') -and
            $node.Extent.Text.Contains('$probeArguments +=')
    }, $true)
    if ($null -eq $argumentConsumer) { throw 'Launch 缺少 composite 原生参数消费块' }
    $compositePlanPath = 'plan'; $compositePlanFileSha256 = 'plan-hash'; $compositeSchedulePath = 'schedule'
    foreach ($enabled in @($false, $true)) {
        $boundedCompositeEventMonitor = $false
        $boundedCompositeAutoArm = $enabled
        $isBCompositeTask = $true
        $probeArguments = @()
        . ([scriptblock]::Create($argumentConsumer.Extent.Text))
        if (@($probeArguments | Where-Object { $_ -ceq '--bounded-composite-auto-arm' }).Count -ne [int]$enabled -or
            $probeArguments.Count -ne (6 + [int]$enabled)) {
            throw 'Launch 必须只为显式自动模式传一次无值 bounded flag'
        }
    }
    $isBCompositeTask = $true; $boundedCompositeAutoArm = $true; $boundedCompositeEventMonitor = $true; $probeArguments = @()
    . ([scriptblock]::Create($argumentConsumer.Extent.Text))
    if ($probeArguments.Count -ne 8 -or
        @($probeArguments | Where-Object { $_ -ceq '--bounded-composite-auto-arm' }).Count -ne 1 -or
        @($probeArguments | Where-Object { $_ -ceq '--bounded-composite-event-monitor' }).Count -ne 1) {
        throw '独立事件监控必须同时传两个显式无值开关，且各一次'
    }
    $isBCompositeTask = $false; $boundedCompositeAutoArm = $true; $probeArguments = @()
    . ([scriptblock]::Create($argumentConsumer.Extent.Text))
    if ($probeArguments.Count -ne 0) { throw '其他 profile 不得取得 bounded composite 参数' }
    $cueFunction = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'ConvertTo-PhysicalProbeOperatorCue'
    }, $true)
    . ([scriptblock]::Create($cueFunction.Extent.Text))
    if ((ConvertTo-PhysicalProbeOperatorCue 'KMBOX monitor 已就绪' $true) -cne
            '【自动有限取证】无需按住右键；End/F8 可急停。' -or
        (ConvertTo-PhysicalProbeOperatorCue '有限composite自动武装：等待有效monitor，End/F8可急停。' $true) -cne
            '【自动有限取证】无需按住右键；End/F8 可急停。' -or
        (ConvertTo-PhysicalProbeOperatorCue 'Mouse Effect Probe 时间线完成' $true) -cne
            '【命令阶段结束】正在整理自动有限取证证据。' -or
        (ConvertTo-PhysicalProbeOperatorCue 'KMBOX monitor 已就绪') -cne
            '【按住右键】5 秒内按住并持续保持；直到看到“现在松开右键”。') {
        throw '自动取证提示必须明确免按右键，旧默认提示保持原样'
    }
    if ((ConvertTo-PhysicalProbeOperatorCue '有限composite事件订阅：首态未知时按显式策略开始；真实键态与故障照常检查。' $true $true) -cne
            '【事件监控有限取证】首态未知按 UNKNOWN 记录；已收到的 End/F8 事件可急停。' -or
        (ConvertTo-PhysicalProbeOperatorCue 'Mouse Effect Probe 时间线完成' $true $true) -cne
            '【命令阶段结束】正在整理事件监控有限取证证据。') {
        throw '事件监控提示必须明确 UNKNOWN 与已收到事件，不得复用完整 monitor 就绪声明'
    }

    $armingEvidenceFunction = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Get-CompositeArmingEvidence'
    }, $true)
    if ($null -eq $armingEvidenceFunction) { throw 'Launch 缺少事件策略与零包未知事实的独立消费入口' }
    . ([scriptblock]::Create($armingEvidenceFunction.Extent.Text))
    $armingEvidenceCalls = @($launchAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Get-CompositeArmingEvidence'
    }, $true))
    if ($armingEvidenceCalls.Count -ne 1 -or
        $armingEvidenceCalls[0].Extent.StartOffset -ge $launch.IndexOf('    $executionComplete =')) {
        throw '完成判断必须消费实际 safety ledger 的独立事件策略'
    }
    $eventLedgerFixture = @{
        arming_policy = 'BOUNDED_COMPOSITE_SUBSCRIBED_EVENT_MONITOR'
        bounded_composite_event_monitor = $true; input_state_ever_observed = $false
        recording_failed = $false; dropped_observation_count = 0
        monitor_packet_recording_failed = $false; dropped_monitor_packet_count = 0
        terminal_decision = 'bounded_ready_without_input_state'; probe_stop_reason = 'normal_completion'
        monitor_packets = @()
        observations = @(@{
            observed_at_steady_ns = 1; phase = 'arming'; poll_succeeded = $true
            monitor_status = 'WAITING'; state_valid = $false; monitor_sequence = 0
            right_button_pressed = $false; end_pressed = $false; f8_pressed = $false
            decision = 'bounded_ready_without_input_state'
        }, @{
            observed_at_steady_ns = 2; phase = 'active'; poll_succeeded = $true
            monitor_status = 'WAITING'; state_valid = $false; monitor_sequence = 0
            right_button_pressed = $false; end_pressed = $false; f8_pressed = $false
            decision = 'bounded_ready_without_input_state'
        })
    } | ConvertTo-Json -Depth 8
    foreach ($case in @('event-zero', 'old-manual-zero', 'old-auto-zero', 'wrong-policy', 'missing-policy',
            'false-event-flag', 'string-event-flag', 'missing-ever-observed', 'forged-ever-observed',
            'observation-drop', 'packet-drop', 'recording-failed', 'packet-recording-failed',
            'poll-failure', 'fake-ready', 'fake-sequence', 'fake-right-button', 'received-end',
            'received-f8', 'ordinary-ready-decision', 'empty-observations', 'unexpected-packet',
            'noninteger-observation-drop', 'boolean-packet-drop', 'noninteger-sequence')) {
        $ledger = $eventLedgerFixture | ConvertFrom-Json
        $auto = $true; $eventMode = $true; $reject = $false; $zeroComplete = $false
        switch ($case) {
            'event-zero' { $zeroComplete = $true }
            'old-manual-zero' {
                $auto = $false; $eventMode = $false
                $ledger.PSObject.Properties.Remove('arming_policy')
                $ledger.PSObject.Properties.Remove('bounded_composite_event_monitor')
                $ledger.PSObject.Properties.Remove('input_state_ever_observed')
                foreach ($observation in $ledger.observations) { $observation.decision = 'waiting' }
            }
            'old-auto-zero' {
                $eventMode = $false; $ledger.arming_policy = 'BOUNDED_COMPOSITE_AUTO_ARM'
                $ledger.bounded_composite_event_monitor = $false
                foreach ($observation in $ledger.observations) { $observation.decision = 'waiting' }
            }
            'wrong-policy' { $ledger.arming_policy = 'BOUNDED_COMPOSITE_AUTO_ARM'; $reject = $true }
            'missing-policy' { $ledger.PSObject.Properties.Remove('arming_policy'); $reject = $true }
            'false-event-flag' { $ledger.bounded_composite_event_monitor = $false; $reject = $true }
            'string-event-flag' { $ledger.bounded_composite_event_monitor = 'true'; $reject = $true }
            'missing-ever-observed' { $ledger.PSObject.Properties.Remove('input_state_ever_observed'); $reject = $true }
            'forged-ever-observed' { $ledger.input_state_ever_observed = $true; $reject = $true }
            'observation-drop' { $ledger.dropped_observation_count = 1 }
            'packet-drop' { $ledger.dropped_monitor_packet_count = 1 }
            'recording-failed' { $ledger.recording_failed = $true }
            'packet-recording-failed' { $ledger.monitor_packet_recording_failed = $true }
            'poll-failure' { $ledger.observations[0].poll_succeeded = $false }
            'fake-ready' { $ledger.observations[0].monitor_status = 'READY' }
            'fake-sequence' { $ledger.observations[0].monitor_sequence = 1 }
            'fake-right-button' { $ledger.observations[0].right_button_pressed = $true }
            'received-end' { $ledger.observations[0].end_pressed = $true }
            'received-f8' { $ledger.observations[0].f8_pressed = $true }
            'ordinary-ready-decision' { $ledger.observations[0].decision = 'ready' }
            'empty-observations' { $ledger.observations = @() }
            'unexpected-packet' { $ledger.monitor_packets = @(@{ unexpected = $true }) }
            'noninteger-observation-drop' { $ledger.dropped_observation_count = [double]0.0 }
            'boolean-packet-drop' { $ledger.dropped_monitor_packet_count = $false }
            'noninteger-sequence' { $ledger.observations[0].monitor_sequence = [double]0.0 }
        }
        $caught = $false; $evidence = $null
        try { $evidence = Get-CompositeArmingEvidence $ledger $auto $eventMode }
        catch { $caught = $true }
        if ($caught -ne $reject -or
            (-not $caught -and $evidence.zero_packet_unknown_complete -ne $zeroComplete)) {
            throw "事件策略/零包未知证据判断无效：$case"
        }
        Write-Host "Launch 事件监控证据合同通过：$case"
    }
    $executionConsumer = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -ceq '$executionComplete'
    }, $true)
    # 只改变 monitor 这一维；其余实际完成表达式输入是满足旧合同的合成值。
    $probeExitCode = 0
    $report = @{ result = @{ state = 'completed'; complete = $true; stop_reason = 'normal_completion'
        cumulative_requested_x_counts = 0; cumulative_backend_completed_x_counts = 0 } } |
        ConvertTo-Json | ConvertFrom-Json
    $events = @(1); $samples = @(1); $completedPulses = 38; $expectedPulseCount = 38
    $safetyLedger = $eventLedgerFixture | ConvertFrom-Json; $safetyObservations = @($safetyLedger.observations)
    foreach ($eventMode in @($false, $true)) {
        $monitorPacketIdentityComplete = $false
        $compositeArmingEvidence = [pscustomobject]@{ zero_packet_unknown_complete = $eventMode }
        . ([scriptblock]::Create($executionConsumer.Extent.Text))
        if ($executionComplete -ne $eventMode -or $monitorPacketIdentityComplete) {
            throw '仅独立事件策略能完成零包取证，普通 packet identity 必须仍为 false'
        }
    }

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
        if (args.Length == 2 && args[0] == "--probe-fixture")
        {
            if (args[1] == "auto-empty-error") return 3;
            if (args[1] == "auto-large-output")
            {
                Console.Write(new string('出', 131072));
                Console.Error.Write(new string('错', 131072));
                return 0;
            }
            Console.WriteLine("有限composite自动武装：等待有效monitor，End/F8可急停。");
            if (args[1] == "auto-stderr")
            {
                Console.Error.WriteLine("Probe 自动取证失败详情：固定相位超界");
                return 3;
            }
            Console.WriteLine("Mouse Effect Probe 时间线完成");
            Console.Error.WriteLine("Probe 合成诊断不改变成功退出码");
            return 0;
        }
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

    foreach ($functionName in @('Invoke-Utf8NativeProcess', 'Invoke-BoundedCompositeProbe', 'Write-NewUtf8Json')) {
        $functionAst = $launchAst.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
        }, $true)
        if ($null -ne $functionAst) { . ([scriptblock]::Create($functionAst.Extent.Text)) }
    }
    $automaticProbeBranch = $launchAst.Find({
        param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Extent.Text.StartsWith('if ($boundedCompositeAutoArm) {') -and
            $node.Extent.Text.Contains('Invoke-BoundedCompositeProbe')
    }, $true)
    if ($null -eq $automaticProbeBranch) {
        # 原实现的实际入口：stderr 被 PS5 提升异常时本回归必须变红。
        $automaticProbeBranch = $launchAst.Find({
            param($node)
            $node -is [Management.Automation.Language.PipelineAst] -and
                $node.Extent.Text.StartsWith('& ([string]$task.files.probe_executable.path) @probeArguments 2>&1')
        }, $true)
    }
    if ($null -eq $automaticProbeBranch) { throw 'Launch 缺少可执行的自动 Probe 调用入口' }
    foreach ($name in @('auto-stderr', 'auto-success', 'auto-empty-error', 'auto-large-output', 'auto-start-failure')) {
        $caseRoot = Join-Path $fixtureRoot $name
        [void][IO.Directory]::CreateDirectory($caseRoot)
        $task = [pscustomobject]@{ files = [pscustomobject]@{
            probe_executable = [pscustomobject]@{ path = $(if ($name -eq 'auto-start-failure') {
                Join-Path $caseRoot 'absent probe.exe'
            } else { $fixtureExecutable }) }
        } }
        $probeArguments = @('--probe-fixture', $name)
        $boundedProbeOutputPath = Join-Path $caseRoot 'probe-output.json'
        $boundedCompositeAutoArm = $true
        $boundedCompositeEventMonitor = $false
        $operatorState = @{ monitor_seen = $false; terminal_seen = $false }
        $probeExitCode = -1
        $caught = $null
        $previousOutputEncoding = [Console]::OutputEncoding
        try {
            [Console]::OutputEncoding = [Text.Encoding]::ASCII
            $null = . ([scriptblock]::Create($automaticProbeBranch.Extent.Text)) 6>$null
        } catch { $caught = $_ }
        finally { [Console]::OutputEncoding = $previousOutputEncoding }
        if ($name -eq 'auto-start-failure') {
            if ($null -eq $caught -or -not $caught.Exception.Message.Contains('bounded composite probe 启动失败') -or
                $caught.Exception.Message.Contains('Seal')) { throw '自动 Probe 启动错误必须保留正确来源' }
            continue
        }
        if ($null -ne $caught -or -not (Test-Path -LiteralPath $boundedProbeOutputPath -PathType Leaf)) {
            throw "自动 Probe 不得因 stderr 提前丢失退出码/双流：$name；$caught"
        }
        $nativeOutput = Get-Content -LiteralPath $boundedProbeOutputPath -Raw -Encoding utf8 | ConvertFrom-Json
        $expectedExit = if ($name -in @('auto-stderr', 'auto-empty-error')) { 3 } else { 0 }
        if ($probeExitCode -ne $expectedExit -or $nativeOutput.exit_code -ne $expectedExit -or
            ($name -eq 'auto-stderr' -and -not $nativeOutput.stderr.Contains('Probe 自动取证失败详情：固定相位超界')) -or
            ($name -eq 'auto-success' -and (-not $nativeOutput.stdout.Contains('有限composite自动武装') -or
                -not $nativeOutput.stderr.Contains('Probe 合成诊断不改变成功退出码'))) -or
            ($name -eq 'auto-empty-error' -and ($nativeOutput.stdout -ne '' -or $nativeOutput.stderr -ne '')) -or
            ($name -eq 'auto-large-output' -and ($nativeOutput.stdout.Length -ne 131072 -or $nativeOutput.stderr.Length -ne 131072))) {
            throw "自动 Probe UTF-8/退出码/完整双流不一致：$name"
        }
        Write-Host "自动 Probe caller 合同通过：$name"
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
