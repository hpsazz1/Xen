$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$workflow = Join-Path $PSScriptRoot "invoke_live_game_acceptance.ps1"
$testRoot = Join-Path $repositoryRoot (
    "cache\live-game-workflow-test-{0}" -f [guid]::NewGuid().ToString("N"))
$fixedModel = Join-Path $repositoryRoot `
    "build-matrix-final-cpu\Release\models\14wv11.onnx"
$failures = 0

. (Join-Path $PSScriptRoot "aim_report.ps1")

function Expect {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { return }
    ++$script:failures
    Write-Error "[失败] $Message" -ErrorAction Continue
}

function Invoke-ExpectedFailure {
    param([scriptblock]$Action, [string]$Message)
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Expect $failed $Message
}

function New-SyntheticAimSample {
    param(
        [uint64]$Sequence = 1,
        [bool]$LeadActive = $false,
        [double]$LeadX = 0.0,
        [double]$FinalX = 200.0,
        [bool]$PredictionOutside = $false
    )

    return [ordered]@{
        sequence = $Sequence
        aim_status = "SUCCESS"
        mouse_sent = $false
        aim_has_target = $true
        aim_has_command = $true
        aim_track_id = 7
        aim_track_state = "CONFIRMED"
        aim_track_predicted = $false
        aim_lead_active = $LeadActive
        aim_base_point_inside_box = $true
        aim_prediction_point_outside_box = $PredictionOutside
        aim_command_toward_target = $true
        aim_control_center_x = 160.0
        aim_control_center_y = 160.0
        aim_acquisition_range_radius = 144.0
        aim_active_range_radius = 144.0
        aim_range_locked = $false
        aim_range_allows_control = $true
        aim_box = @(150.0, 100.0, 230.0, 220.0)
        aim_matched_observation_valid = $true
        aim_matched_observation_box = @(150.0, 100.0, 230.0, 220.0)
        aim_matched_observation_head_only = $false
        aim_matched_observation_aim_from_head = $true
        aim_base_point = @(200.0, 140.0)
        aim_delay_compensated_point = @(200.0, 140.0)
        aim_final_point = @($FinalX, 140.0)
        aim_velocity = @(250.0, 0.0)
        aim_lead = @($LeadX, 0.0)
        aim_delay_compensation_active = $false
        aim_delay_compensation = @(0.0, 0.0)
        aim_delay_compensation_ms_x = 0.0
        aim_delay_compensation_ms_y = 0.0
        aim_delay_compensation_ms = 0.0
        aim_observation_age_ms = 20.0
        aim_command = @(20, -10)
        person_detection_count = 1
        head_detection_count = 1
        max_person_confidence = 0.864
        max_head_confidence = 0.871
        detection_count_by_class = @(1, 1, 0, 0)
        max_confidence_by_class = @(0.864, 0.871, 0.0, 0.0)
    }
}

$outsidePrediction = New-SyntheticAimSample -LeadActive $true `
    -LeadX 40.0 -FinalX 240.0 -PredictionOutside $true
$outsideSummary = Get-XenAimReportSummary -Samples @($outsidePrediction) `
    -PredictionEnabled on -MaxPredictionLeadPercent 35.0
Expect ([bool]$outsideSummary.contract_valid -and
        $outsideSummary.prediction_point_outside_box_frames -eq 1 -and
        $outsideSummary.violations.lead_limit_frames -eq 0 -and
        [bool]$outsideSummary.contract.prediction_points_may_leave_selected_box) `
    "预测点允许出框，未超过最大提前距离时必须通过"
$outsidePrediction.aim_lead = @(60.0, 0.0)
$outsidePrediction.aim_final_point = @(260.0, 140.0)
$tooFarSummary = Get-XenAimReportSummary -Samples @($outsidePrediction) `
    -PredictionEnabled on -MaxPredictionLeadPercent 35.0
Expect (-not [bool]$tooFarSummary.contract_valid -and
        $tooFarSummary.violations.lead_limit_frames -eq 1) `
    "预测点出框不是失败，超过最大提前距离才必须失败"

$delayCompensated = New-SyntheticAimSample -LeadActive $false `
    -LeadX 0.0 -FinalX 206.0 -PredictionOutside $false
$delayCompensated.aim_delay_compensation_active = $true
$delayCompensated.aim_delay_compensation = @(6.0, 0.0)
$delayCompensated.aim_delay_compensation_ms_x = 6.0
$delayCompensated.aim_delay_compensation_ms_y = 6.0
$delayCompensated.aim_delay_compensation_ms = 6.0
$delayCompensated.aim_delay_compensated_point = @(206.0, 140.0)
$delaySummary = Get-XenAimReportSummary -Samples @($delayCompensated) `
    -PredictionEnabled off -MaxPredictionLeadPercent 35.0
Expect ([bool]$delaySummary.contract_valid -and
        $delaySummary.violations.lead_vector_consistency_frames -eq 0 -and
        $delaySummary.violations.prediction_disabled_lead_frames -eq 0) `
    "prediction 关闭时合法延迟补偿不得误报为提前量"
$delayCompensated.aim_delay_compensated_point = @(205.0, 140.0)
$invalidDelaySummary = Get-XenAimReportSummary `
    -Samples @($delayCompensated) -PredictionEnabled off `
    -MaxPredictionLeadPercent 35.0
Expect (-not [bool]$invalidDelaySummary.contract_valid -and
        $invalidDelaySummary.violations.lead_vector_consistency_frames -eq 1) `
    "延迟补偿点与基础点加补偿向量不一致时必须拒绝"

try {
    $observationRun = Join-Path $testRoot "observation"
    & $workflow -Mode Prepare -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    Expect (Test-Path -LiteralPath (
        Join-Path $observationRun "TASK.md")) "应生成 TASK.md"
    $task = Get-Content -LiteralPath (
        Join-Path $observationRun "task.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $config = Get-Content -LiteralPath (
        Join-Path $observationRun "config.ini") -Raw -Encoding UTF8
    Expect ($task.stage -eq "DetectionStatic" -and
            -not [bool]$task.physical_output) `
        "静止检测任务必须禁用物理输出"
    Expect ($config -match '(?m)^model_path=14wv11\.onnx$' -and
            $config -match '(?m)^backend=cpu$' -and
            $config -match '(?m)^allow_send_input=false$' -and
            $config -match
                '(?m)^open_detached_preview_on_start=true$') `
        "观察任务必须固定模型、CPU、置顶预览和禁用物理输出"
    Expect ($config -match '(?m)^person_class_ids=0,2$' -and
            $config -match '(?m)^head_class_ids=1,3$' -and
            $config -match '(?m)^acquisition_range_percent=90\.000000$' -and
            $config -match '(?m)^enable_prediction=false$' -and
            $config -match
                '(?m)^max_prediction_lead_percent=35\.000000$' -and
            $config -match '(?m)^roi_width=320$' -and
            $config -match '(?m)^roi_height=320$') `
        "固定模型必须使用完整 Aim 配置、320x320 ROI 和 CT/T 头身类别"
    Expect ($config -match '(?m)^aim_hold_virtual_keys=2$' -and
            $config -match '(?m)^emergency_virtual_keys=35$' -and
            $config -match '(?m)^runtime_toggle_virtual_keys=119$' -and
            $task.safety.aim_hold_virtual_keys[0] -eq 2 -and
            $task.safety.emergency_virtual_keys[0] -eq 35 -and
            $task.safety.runtime_toggle_virtual_keys[0] -eq 119) `
        "任务配置和身份记录必须固定同一组多键快捷键基准"
    $taskMarkdown = Get-Content -LiteralPath (
        Join-Path $observationRun "TASK.md") -Raw -Encoding UTF8
    $expectedLaunchPrefix =
        'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Mode Launch' -f
        $workflow
    Expect ($taskMarkdown -match [regex]::Escape($expectedLaunchPrefix) -and
            $taskMarkdown -match '~~~powershell' -and
            $taskMarkdown -match
                'Xen 进程退出后才会汇总并输出“自动报告已汇总”' -and
            $taskMarkdown -match '按 F8 启动 Runtime' -and
            $taskMarkdown -match '再次按 F8 停止' -and
            $taskMarkdown -match 'C0～C3' -and
            $taskMarkdown -notmatch 'C0～C4') `
        "任务单必须提供绝对命令、四类别说明和 F8 启停、退出、汇总顺序"
    Expect ($task.view_mode -eq "fixed") `
        "静止检测必须固定人物视角"

    foreach ($stage in @(
            "DetectionMoveLeft", "DetectionMoveRight", "DetectionShuttle",
            "DetectionSuperJump")) {
        $movementRun = Join-Path $testRoot $stage
        & $workflow -Mode Prepare -Stage $stage `
            -RunDirectory $movementRun | Out-Null
        $movementTask = Get-Content -LiteralPath (
            Join-Path $movementRun "task.json") -Raw -Encoding UTF8 |
            ConvertFrom-Json
        Expect ($movementTask.view_mode -eq "fixed" -and
                -not [bool]$movementTask.physical_output) `
            "$stage 必须固定人物视角并禁用物理输出"
    }
    $unauthorizedRun = Join-Path $testRoot "unauthorized-control"
    Invoke-ExpectedFailure {
        & $workflow -Mode Prepare -Stage ControlAcquire `
            -RunDirectory $unauthorizedRun | Out-Null
    } "控制环节缺少双重授权时必须失败"
    Expect (-not (Test-Path -LiteralPath $unauthorizedRun)) `
        "授权失败不得留下任务目录"

    $controlRun = Join-Path $testRoot "authorized-control"
    & $workflow -Mode Prepare -Stage ControlAcquire `
        -RunDirectory $controlRun -AllowPhysicalOutput `
        -PhysicalOutputConfirmation `
            XEN_LIVE_GAME_ACCEPTANCE_SENDS_REAL_INPUT | Out-Null
    $controlConfig = Get-Content -LiteralPath (
        Join-Path $controlRun "config.ini") -Raw -Encoding UTF8
    Expect ($controlConfig -match '(?m)^allow_send_input=true$') `
        "完成双重授权的控制任务才允许生成物理输出配置"

    $runtimeDirectory = Join-Path $observationRun "cache\runtime"
    New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null
    $reportPath = Join-Path $runtimeDirectory "synthetic.json"
    $csvPath = Join-Path $runtimeDirectory "synthetic.csv"
    $synthetic = [ordered]@{
        schema = 17
        session_id = "synthetic"
        model_path = $fixedModel
        provider = "CPUExecutionProvider"
        capture_backend = "DESKTOP_DUPLICATION"
        mouse_backend = "win32_send_input"
        performance_probes_enabled = $false
        sample_count = 1200
        successful_samples = 1200
        failed_samples = 0
        report_samples_dropped = 0
        runtime_samples_dropped = 0
        coverage = [ordered]@{
            available = $true
            warmup_start_overwritten_frames = 0
            warmup_end_overwritten_frames = 0
            formal_end_overwritten_frames = 0
            startup = [ordered]@{
                sample_count = 1
                first_sequence = 1
                last_sequence = 1
                runtime_overwritten_frames = 0
                sequence_gaps = 0
                trailing_runtime_overwritten_frames = 0
                counter_matches_sequence_gaps = $true
            }
            warmup = [ordered]@{
                sample_count = 100
                first_sequence = 2
                last_sequence = 101
                runtime_overwritten_frames = 0
                sequence_gaps = 0
                trailing_runtime_overwritten_frames = 0
                counter_matches_sequence_gaps = $true
            }
            formal = [ordered]@{
                sample_count = 1200
                first_sequence = 102
                last_sequence = 1301
                runtime_overwritten_frames = 0
                sequence_gaps = 0
                trailing_runtime_overwritten_frames = 0
                counter_matches_sequence_gaps = $true
            }
        }
        ndi_video_queue_depth = [ordered]@{
            sample_count = 0
            mean_frames = 0.0
            p50_frames = 0.0
            p95_frames = 0.0
            p99_frames = 0.0
            max_frames = 0.0
        }
        final_snapshot = [ordered]@{
            provider = "CPUExecutionProvider"
            source_width = 2560
            source_height = 1440
            encoded_width = 2560
            encoded_height = 1440
            roi_x = 1120
            roi_y = 560
            roi_width = 320
            roi_height = 320
            source_pixels_per_pixel_x = 1.0
            source_pixels_per_pixel_y = 1.0
            output_allowed_by_config = $false
            preview_enabled = $true
            preview_sampled_frames = 120
            preview_dropped_frames = 0
            failed_frames = 0
            last_error = ""
            mouse_commands = 0
        }
        timing = [ordered]@{
            total = [ordered]@{
                sample_count = 1200
                mean_ms = 4.0
                p50_ms = 3.8
                p95_ms = 5.0
                p99_ms = 6.0
                max_ms = 7.0
            }
        }
        samples = @(for ($index = 0; $index -lt 1200; ++$index) {
            New-SyntheticAimSample -Sequence ([uint64]($index + 102))
        })
    }
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText(
        $csvPath, "# synthetic`n",
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $summary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect ([bool]$summary.automatic_complete -and
            $summary.sample_count -eq 1200 -and
            $summary.failed_samples -eq 0 -and
            $summary.detection_observability.classes[0].name -eq "ct_body" -and
            $summary.detection_observability.classes[0].confidence.p50 -eq
                0.864 -and
            $summary.detection_observability.classes[2].confidence.detected_frames `
                -eq 0 -and
            [bool]$summary.aim_observability.contract_valid -and
            $summary.aim_observability.precomputed_command_frames -eq 1200 -and
            [bool]$summary.aim_observability.contract.prediction_points_may_leave_selected_box) `
        "合法 schema 17 报告应完成检测和 Aim 自动汇总"

    $synthetic.schema = 16
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $schema16Summary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect ([bool]$schema16Summary.automatic_complete) `
        "Collect 必须兼容已存在的 schema 16 Run"
    $synthetic.schema = 17

    $synthetic.samples[0].aim_base_point_inside_box = $false
    $synthetic.samples[0].aim_base_point = @(140.0, 140.0)
    $synthetic.samples[0].aim_final_point = @(140.0, 140.0)
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $baseOutsideSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$baseOutsideSummary.automatic_complete -and
            ($baseOutsideSummary.failures -join "`n") -match
                '基础追踪点在目标框外') `
        "基础追踪点出框必须拒绝自动通过"
    $synthetic.samples[0] = New-SyntheticAimSample -Sequence 102

    $synthetic.schema = 15
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $oldSchemaSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$oldSchemaSummary.automatic_complete -and
            ($oldSchemaSummary.failures -join "`n") -match
                '报告 schema 不是 16 或 17') `
        "旧 schema 报告必须拒绝自动通过"
    $synthetic.schema = 17

    $synthetic.performance_probes_enabled = $true
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $probeSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$probeSummary.automatic_complete -and
            ($probeSummary.failures -join "`n") -match
                '必须显式关闭性能探针') `
        "开启性能探针的报告必须拒绝作为实机验收证据"
    $synthetic.performance_probes_enabled = $false

    $synthetic.final_snapshot.preview_sampled_frames = 0
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $noPreviewSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$noPreviewSummary.automatic_complete -and
            ($noPreviewSummary.failures -join "`n") -match
                '未形成独立置顶检测预览采样') `
        "GUI 报告预览采样为零时必须拒绝自动通过"
    $synthetic.final_snapshot.preview_sampled_frames = 120
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))

    Copy-Item -LiteralPath $reportPath -Destination (
        Join-Path $runtimeDirectory "duplicate.json")
    Copy-Item -LiteralPath $csvPath -Destination (
        Join-Path $runtimeDirectory "duplicate.csv")
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $duplicateSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$duplicateSummary.automatic_complete -and
            ($duplicateSummary.failures -join "`n") -match
                '必须且只能生成一份') `
        "单个环节存在多份 Runtime 报告时必须拒绝自动通过"
    Remove-Item -LiteralPath (
        Join-Path $runtimeDirectory "duplicate.json") -Force
    Remove-Item -LiteralPath (
        Join-Path $runtimeDirectory "duplicate.csv") -Force

    Add-Content -LiteralPath (Join-Path $observationRun "config.ini") `
        -Value "# tampered" -Encoding UTF8
    Invoke-ExpectedFailure {
        & $workflow -Mode Collect -Stage DetectionStatic `
            -RunDirectory $observationRun | Out-Null
    } "配置发生变化后必须拒绝再次汇总"
} finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $resolvedCacheRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $repositoryRoot "cache"))
    if ($resolvedTestRoot.StartsWith(
            $resolvedCacheRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTestRoot) -like
            "live-game-workflow-test-*") {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}

if ($failures -ne 0) {
    throw "实机测试工作流回归失败数：$failures"
}
Write-Host "实机测试工作流回归全部通过。"
