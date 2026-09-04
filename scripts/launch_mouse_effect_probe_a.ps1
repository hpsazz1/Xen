param(
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$physicalAConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
$physicalBConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT"
$physicalBHoldoutConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT"
$physicalBMagnitudePrimaryConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT"
$physicalBMagnitudeHoldoutConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_HOLDOUT_SENDS_REAL_KMBOX_INPUT"
if (-not $AllowPhysicalOutput.IsPresent -or
    $PhysicalOutputConfirmation -notin @(
        $physicalAConfirmation, $physicalBConfirmation,
        $physicalBHoldoutConfirmation,
        $physicalBMagnitudePrimaryConfirmation,
        $physicalBMagnitudeHoldoutConfirmation)) {
    throw "Physical probe 会发送真实 KMBOX 输入，必须同时提供物理输出开关和与 task 匹配的固定确认令牌。"
}
$confirmation = $PhysicalOutputConfirmation

function Get-FileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Assert-FileEvidence([object]$Evidence, [string]$Description) {
    if ($null -eq $Evidence -or
        [string]::IsNullOrWhiteSpace([string]$Evidence.path) -or
        -not (Test-Path -LiteralPath ([string]$Evidence.path) -PathType Leaf)) {
        throw "$Description 不存在"
    }
    $item = Get-Item -LiteralPath ([string]$Evidence.path)
    if ([uint64]$item.Length -ne [uint64]$Evidence.size -or
        (Get-FileSha256 $item.FullName) -ne [string]$Evidence.sha256) {
        throw "$Description 的 size/SHA 与 Prepare 不一致"
    }
}

function Quote-NativeArgument([string]$Value) {
    if ($Value.Contains('"')) {
        throw "原生命令参数不得包含双引号。"
    }
    return '"' + $Value + '"'
}

function ConvertTo-PhysicalProbeOperatorCue([string]$Line) {
    if ($Line.StartsWith("KMBOX monitor 已就绪")) {
        return '【按住右键】5 秒内按住并持续保持；直到看到“现在松开右键”。'
    }
    if ($Line.StartsWith("Mouse Effect Probe 时间线完成")) {
        return "【现在松开右键】命令阶段完成；正在整理证据。"
    }
    if ($Line.StartsWith("Mouse Effect Probe 未正常完成")) {
        return "【现在松开右键】命令阶段停止；正在整理证据。"
    }
    return $null
}

function Get-PhysicalEventFrameCoverage(
        [object]$Event,
        [object]$Sample,
        [Collections.Generic.HashSet[int64]]$FrameTimestamps,
        [bool]$AllowUnmatchedBaseline,
        [uint64]$BaselineSampleCount) {
    $matched = $FrameTimestamps.Contains([int64]$Event.source_timestamp)
    $sampleIndex = [uint64]$Sample.sample_index
    $unmatchedBaselineAllowed =
        -not $matched -and
        $AllowUnmatchedBaseline -and
        [string]$Sample.phase -eq "baseline" -and
        [int]$Sample.dx_counts -eq 0 -and
        [int]$Sample.dy_counts -eq 0 -and
        $sampleIndex -gt 0 -and
        $sampleIndex + 1 -lt $BaselineSampleCount -and
        -not [bool]$Event.dispatch_attempted -and
        [int]$Event.requested_dx_counts -eq 0 -and
        [int]$Event.requested_dy_counts -eq 0 -and
        -not [bool]$Event.backend_succeeded -and
        -not [bool]$Event.protocol_ack_received
    return [pscustomobject]@{
        matched = [bool]$matched
        unmatched_baseline_allowed = [bool]$unmatchedBaselineAllowed
        required_source_frame_missing =
            [bool](-not $matched -and -not $unmatchedBaselineAllowed)
    }
}

function Wait-SidecarIncoming(
        [Diagnostics.Process]$Process,
        [string]$Parent,
        [int]$TimeoutMilliseconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
            $Process.Refresh()
            if ($Process.HasExited) {
                throw "sidecar 在 incoming 就绪前退出，ExitCode=$($Process.ExitCode)"
            }
            $pattern = ".pixel-evidence.incoming-$($Process.Id)-*"
            $matches = @(Get-ChildItem -LiteralPath $Parent -Directory |
                Where-Object { $_.Name -like $pattern })
            if ($matches.Count -gt 1) {
                throw "当前 sidecar PID 匹配多个 incoming 目录"
            }
            if ($matches.Count -eq 1 -and
                (Test-Path -LiteralPath (
                    Join-Path $matches[0].FullName "source-binding.json") `
                    -PathType Leaf)) {
                return $matches[0].FullName
            }
            Start-Sleep -Milliseconds 25
        }
    } finally {
        $watch.Stop()
    }
    throw "等待 sidecar incoming 就绪超时"
}

function Test-SidecarPublishingStarted([string]$IncomingDirectory) {
    $framesDirectory = Join-Path $IncomingDirectory "frames"
    if (-not (Test-Path -LiteralPath $framesDirectory -PathType Container)) {
        return $false
    }
    return @(
        Get-ChildItem -LiteralPath $framesDirectory -File -Filter "*.png" |
            Select-Object -First 1
    ).Count -eq 1
}

function Get-SidecarPngCount([string]$IncomingDirectory) {
    $framesDirectory = Join-Path $IncomingDirectory "frames"
    if (-not (Test-Path -LiteralPath $framesDirectory -PathType Container)) {
        return [uint64]0
    }
    return [uint64]@(
        Get-ChildItem -LiteralPath $framesDirectory -File -Filter "*.png"
    ).Count
}

function Wait-SidecarCompletion(
        [Diagnostics.Process]$Process,
        [string]$IncomingDirectory,
        [int]$RecordingTimeoutMilliseconds,
        [int]$PublishingTimeoutMilliseconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $phase = "RECORDING"
    $publishingStartedMilliseconds = $null
    try {
        while ($true) {
            $Process.Refresh()
            if ($Process.HasExited) {
                return [pscustomobject]@{
                    timed_out = $false
                    phase = $phase
                    elapsed_ms = [int64]$watch.ElapsedMilliseconds
                    publishing_started_ms = $publishingStartedMilliseconds
                    png_count = Get-SidecarPngCount $IncomingDirectory
                }
            }
            if ($phase -eq "RECORDING" -and
                (Test-SidecarPublishingStarted $IncomingDirectory)) {
                $phase = "PUBLISHING"
                $publishingStartedMilliseconds =
                    [int64]$watch.ElapsedMilliseconds
            }
            $phaseElapsedMilliseconds = if ($phase -eq "PUBLISHING") {
                [int64]$watch.ElapsedMilliseconds -
                    [int64]$publishingStartedMilliseconds
            } else {
                [int64]$watch.ElapsedMilliseconds
            }
            $phaseBudgetMilliseconds = if ($phase -eq "PUBLISHING") {
                $PublishingTimeoutMilliseconds
            } else {
                $RecordingTimeoutMilliseconds
            }
            if ($phaseElapsedMilliseconds -ge $phaseBudgetMilliseconds) {
                return [pscustomobject]@{
                    timed_out = $true
                    phase = $phase
                    elapsed_ms = [int64]$watch.ElapsedMilliseconds
                    publishing_started_ms = $publishingStartedMilliseconds
                    png_count = Get-SidecarPngCount $IncomingDirectory
                }
            }
            [void]$Process.WaitForExit(100)
        }
    } finally {
        $watch.Stop()
    }
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    if (Test-Path -LiteralPath $Path) {
        throw "JSON 发布目标已存在，拒绝覆盖：$Path"
    }
    $pending = "$Path.pending-$PID-$([guid]::NewGuid().ToString('N'))"
    $utf8 = [Text.UTF8Encoding]::new($false)
    try {
        [IO.File]::WriteAllText(
            $pending,
            ($Value | ConvertTo-Json -Depth 20) + "`n",
            $utf8)
        [IO.File]::Move($pending, $Path)
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

$resolvedRun = (Resolve-Path -LiteralPath $RunDirectory).Path
$taskPath = Join-Path $resolvedRun "task.json"
if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) {
    throw "缺少 Physical A task.json：$taskPath"
}
$task = Get-Content -LiteralPath $taskPath -Raw -Encoding utf8 |
    ConvertFrom-Json
$isA1Task =
    [int]$task.schema_version -eq 1 -and
    [string]$task.evidence_type -eq "mouse_effect_probe_a_task" -and
    [string]$task.profile -eq "sparse_pulse_a"
$isA2Task =
    [int]$task.schema_version -eq 2 -and
    [string]$task.evidence_type -eq "mouse_effect_probe_a2_task" -and
    [string]$task.profile -like "dependency_calibration_a2_*"
$isA2S1Task =
    [int]$task.schema_version -eq 3 -and
    [string]$task.evidence_type -eq "mouse_effect_probe_a2_s1_task" -and
    [string]$task.profile -like "dependency_calibration_a2_s1_*"
$isBPrimaryTask =
    [int]$task.schema_version -eq 5 -and
    [string]$task.evidence_type -eq "mouse_effect_probe_b_task" -and
    [string]$task.profile -eq "physical_b_prbs_primary"
$isBHoldoutTask =
    [int]$task.schema_version -eq 7 -and
    [string]$task.evidence_type -eq "mouse_effect_probe_b_task" -and
    [string]$task.profile -eq "physical_b_prbs_holdout"
$isBMagnitudePrimaryTask =
    [int]$task.schema_version -eq 8 -and
    [string]$task.evidence_type -eq
        "mouse_effect_probe_b_command_magnitude_task" -and
    [string]$task.profile -eq
        "physical_b_command_magnitude_primary"
$isBMagnitudeHoldoutTask =
    [int]$task.schema_version -eq 9 -and
    [string]$task.evidence_type -eq
        "mouse_effect_probe_b_command_magnitude_task" -and
    [string]$task.profile -eq
        "physical_b_command_magnitude_holdout"
$isBMagnitudeTask =
    $isBMagnitudePrimaryTask -or $isBMagnitudeHoldoutTask
$isBTask = $isBPrimaryTask -or $isBHoldoutTask -or $isBMagnitudeTask
$expectedDispatchMode = if ($isBTask) { "physical_b" } else { "physical_a" }
$expectedConfirmation = if ($isBMagnitudeHoldoutTask) {
    $physicalBMagnitudeHoldoutConfirmation
} elseif ($isBMagnitudePrimaryTask) {
    $physicalBMagnitudePrimaryConfirmation
} elseif ($isBHoldoutTask) {
    $physicalBHoldoutConfirmation
} elseif ($isBPrimaryTask) {
    $physicalBConfirmation
} else {
    $physicalAConfirmation
}
if ((-not $isA1Task -and -not $isA2Task -and -not $isA2S1Task -and
        -not $isBTask) -or
    [string]$task.status -ne "PREPARED" -or
    [string]$task.dispatch_mode -ne $expectedDispatchMode -or
    -not [bool]$task.physical_output_capability -or
    [string]$task.run_directory -ne $resolvedRun -or
    [string]$task.physical_output_confirmation -ne
        $expectedConfirmation -or
    $confirmation -ne $expectedConfirmation) {
    throw "Physical probe task 身份或授权合同无效"
}
if ($isA2Task) {
    $expectedProfile = if ([string]$task.run_role -eq "p-cal") {
        "dependency_calibration_a2_p_cal"
    } elseif ([string]$task.run_role -eq "p-holdout") {
        "dependency_calibration_a2_p_holdout"
    } else {
        throw "Physical A2 run_role 非法"
    }
    if ([string]$task.profile -ne $expectedProfile -or
        [uint64]$task.expected_nonzero_transition_count -eq 0) {
        throw "Physical A2 role/profile/transition 合同无效"
    }
}
if ($isA2S1Task) {
    $expectedProfile = if ([string]$task.run_role -eq "primary") {
        "dependency_calibration_a2_s1_primary"
    } elseif ([string]$task.run_role -eq "validation") {
        "dependency_calibration_a2_s1_validation"
    } else {
        throw "Physical A2 S1 run_role 非法"
    }
    if ([string]$task.profile -ne $expectedProfile -or
        [uint64]$task.expected_nonzero_transition_count -eq 0 -or
        [string]$task.liveness_policy.policy_id -ne
            "a2-s1-kmbox-bracket-peak-hold-v1" -or
        [uint64]$task.liveness_policy.peak_hold_sample_count -eq 0 -or
        [bool]$task.liveness_policy.challenge_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.peak_hold_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.settle_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.fixed_pixel_speed_used_as_gate) {
        throw "Physical A2 S1 role/profile/liveness policy 合同无效"
    }
}
if ($isBTask) {
    if ($isBPrimaryTask -and (
        [string]$task.run_role -ne "primary" -or
        [string]$task.profile -ne "physical_b_prbs_primary" -or
        [uint64]$task.sequence_sample_count -ne 800 -or
        [uint64]$task.expected_nonzero_transition_count -eq 0 -or
        [uint64]$task.max_abs_prefix_x_counts -ne 1 -or
        -not [bool]$task.requires_user_frontend_launch -or
        [bool]$task.cross_run_holdout_prepare_authorized -or
        [bool]$task.safety.cross_run_holdout_prepare_authorized -or
        [string]$task.f0.identification_input_definition -ne
            "cumulative_position_counts" -or
        [string]$task.f0.actuator_audit_input -ne
            "completed_command_dx_counts" -or
        [uint64]$task.f0.core_delay_samples -ne 4 -or
        (@($task.f0.tail_lengths) -join ",") -ne "0,1,2,4,8" -or
        [string]$task.f0.nuisance_fit_rows -ne
            "exact_dedicated_pre_guard_only" -or
        -not [bool]$task.f0.selection_used_for_single_refit -or
        [bool]$task.f0.confirmation_used_for_refit -or
        [bool]$task.f0.holdout_used_for_tuning -or
        -not [bool]$task.f0.input_forced_validation_required)) {
        throw "Physical B Primary role/F0/holdout 安全合同无效"
    }
    if ($isBHoldoutTask -and (
        [string]$task.run_role -ne "cross_run_holdout" -or
        [string]$task.profile -ne "physical_b_prbs_holdout" -or
        [uint64]$task.sequence_sample_count -ne 288 -or
        [uint64]$task.expected_nonzero_transition_count -ne 68 -or
        [uint64]$task.max_abs_prefix_x_counts -ne 1 -or
        -not [bool]$task.requires_user_frontend_launch -or
        -not [bool]$task.cross_run_holdout_prepare_authorized -or
        -not [bool]$task.safety.cross_run_holdout_prepare_authorized -or
        [bool]$task.holdout_used_for_tuning -or
        [bool]$task.holdout.holdout_used_for_tuning -or
        -not [bool]$task.holdout.input_forced_required -or
        -not [bool]$task.holdout.output_free_run_required -or
        -not [bool]$task.holdout.delay_and_tail_are_frozen -or
        [string]::IsNullOrWhiteSpace(
            [string]$task.holdout.independence_contract_semantic_sha256) -or
        [uint64]$task.holdout.recurrence_feedback_mask -ne 51 -or
        [uint64]$task.holdout.recurrence_phase -ne 21 -or
        [string]$task.run_uuid -eq [string]$task.primary.run_uuid -or
        [uint64]$task.activation_epoch -eq
            [uint64]$task.primary.activation_epoch)) {
        throw "Physical B cross-Run holdout role/F1/independence 安全合同无效"
    }
    if ($isBMagnitudePrimaryTask -and (
        [string]$task.run_role -ne "primary" -or
        [uint64]$task.sequence_sample_count -ne 1684 -or
        [uint64]$task.expected_nonzero_transition_count -ne 20 -or
        [uint64]$task.max_abs_prefix_x_counts -ne 13 -or
        -not [bool]$task.requires_user_frontend_launch -or
        [string]$task.dynamics_policy.policy_id -ne
            "b-command-magnitude-primary-v1" -or
        [string]$task.dynamics_policy.input_definition -ne
            "backend_completed_relative_command_dx_counts" -or
        (@($task.dynamics_policy.primary_estimation_amplitudes) -join ",") -ne
            "1,4,13" -or
        (@($task.dynamics_policy.within_run_confirmation_amplitudes) -join ",") -ne
            "2,8" -or
        [bool]$task.dynamics_policy.validation_used_for_refit -or
        [bool]$task.dynamics_policy.new_production_gain_claimed -or
        -not [bool]$task.dynamics_policy.
            cross_run_holdout_required_before_candidate -or
        [bool]$task.dynamics_policy.fixed_pixel_speed_used_as_gate -or
        [uint64]$task.safety.max_abs_pulse_counts -ne 13 -or
        [uint64]$task.safety.max_abs_prefix_x_counts -ne 13 -or
        -not [bool]$task.safety.manual_mouse_motion_or_wasd_forbidden)) {
        throw "Physical B command-magnitude Primary 合同无效"
    }
    if ($isBMagnitudeHoldoutTask) {
        throw "Physical B command-magnitude Holdout 尚无已发布 Prepare 合同"
    }
}

$fileEntries = @(
        [pscustomobject]@{ value = $task.files.launch_script; name = "Launch script" },
        [pscustomobject]@{ value = $task.files.probe_executable; name = "probe executable" },
        [pscustomobject]@{ value = $task.files.sidecar_executable; name = "sidecar executable" },
        [pscustomobject]@{ value = $task.files.opencv_runtime; name = "OpenCV runtime" },
        [pscustomobject]@{ value = $task.files.ndi_runtime; name = "NDI runtime" },
        [pscustomobject]@{ value = $task.files.ndi_license; name = "NDI license" },
        [pscustomobject]@{ value = $task.files.config; name = "config.ini" },
        [pscustomobject]@{ value = $task.files.sequence; name = "sequence" },
        [pscustomobject]@{ value = $task.files.probe_binding; name = "probe binding" },
        [pscustomobject]@{ value = $task.files.obs_source_binding; name = "OBS source binding" })
if ($isA2Task) {
    $fileEntries += @(
        [pscustomobject]@{ value = $task.files.sequence_executable; name = "sequence executable" },
        [pscustomobject]@{ value = $task.files.physical_analyzer; name = "physical analyzer" },
        [pscustomobject]@{ value = $task.files.dependency_calibrator; name = "dependency calibrator" },
        [pscustomobject]@{ value = $task.files.synthetic_calibration; name = "S0 calibration" },
        [pscustomobject]@{ value = $task.files.zero_input_calibration; name = "S1 calibration" },
        [pscustomobject]@{ value = $task.files.calibration_plan; name = "A2 calibration plan" })
}
if ($isA2S1Task) {
    $fileEntries += @(
        [pscustomobject]@{ value = $task.files.sequence_executable; name = "sequence executable" },
        [pscustomobject]@{ value = $task.files.dependency_calibrator; name = "dependency calibrator" })
}
if ($isBPrimaryTask) {
    $fileEntries += @(
        [pscustomobject]@{ value = $task.files.sequence_executable; name = "sequence executable" },
        [pscustomobject]@{ value = $task.files.prbs_designer; name = "PRBS designer" },
        [pscustomobject]@{ value = $task.files.physical_b_analyzer; name = "Physical B analyzer" },
        [pscustomobject]@{ value = $task.files.primary_f0; name = "Primary F0" },
        [pscustomobject]@{ value = $task.files.offline_design; name = "offline design" },
        [pscustomobject]@{ value = $task.files.a2_dependency_decision; name = "A2 decision" })
}
if ($isBHoldoutTask) {
    $fileEntries += @(
        [pscustomobject]@{ value = $task.files.sequence_executable; name = "sequence executable" },
        [pscustomobject]@{ value = $task.files.physical_b_analyzer; name = "Primary analyzer" },
        [pscustomobject]@{ value = $task.files.holdout_analyzer; name = "holdout analyzer" },
        [pscustomobject]@{ value = $task.files.holdout_plan; name = "holdout plan" },
        [pscustomobject]@{ value = $task.files.primary_analysis; name = "Primary analysis" },
        [pscustomobject]@{ value = $task.files.primary_command_report; name = "Primary command report" },
        [pscustomobject]@{ value = $task.files.offline_design; name = "offline design" })
}
if ($isBMagnitudeTask) {
    $fileEntries += @(
        [pscustomobject]@{ value = $task.files.sequence_executable; name = "sequence executable" },
        [pscustomobject]@{ value = $task.files.magnitude_analyzer; name = "magnitude analyzer" },
        [pscustomobject]@{ value = $task.files.a2_magnitude_analysis; name = "A2 magnitude analysis" },
        [pscustomobject]@{ value = $task.files.b0_fidelity_evaluation; name = "B0 fidelity evaluation" })
}
foreach ($entry in $fileEntries) {
    Assert-FileEvidence $entry.value $entry.name
}
if ($isBMagnitudePrimaryTask) {
    $a2Magnitude = Get-Content -LiteralPath `
        ([string]$task.files.a2_magnitude_analysis.path) `
        -Raw -Encoding utf8 | ConvertFrom-Json
    $b0Fidelity = Get-Content -LiteralPath `
        ([string]$task.files.b0_fidelity_evaluation.path) `
        -Raw -Encoding utf8 | ConvertFrom-Json
    if ([string]$a2Magnitude.status -ne
            "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" -or
        [bool]$a2Magnitude.physical_output_capability -or
        [int]$a2Magnitude.physical_dispatch_count -ne 0 -or
        [bool]$a2Magnitude.production_aim_changed -or
        -not [bool]$a2Magnitude.evaluation.
            f1_deleted_for_magnitude_domain -or
        [bool]$a2Magnitude.evaluation.new_production_gain_claimed -or
        [string]$b0Fidelity.status -ne
            "BASELINE_REPLAY_FIDELITY_INVALID" -or
        [bool]$b0Fidelity.physical_output_capability -or
        [int]$b0Fidelity.physical_dispatch_count -ne 0 -or
        [bool]$b0Fidelity.production_aim_changed) {
        throw "Physical B command-magnitude 输入证据合同无效"
    }
}
if ($isBPrimaryTask) {
    $primaryF0 = Get-Content -LiteralPath `
        ([string]$task.files.primary_f0.path) -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([string]$primaryF0.evidence_type -ne
            "mouse_effect_probe_physical_b_primary_f0" -or
        [string]$primaryF0.status -ne
            "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE" -or
        [bool]$primaryF0.physical_output_capability -or
        [bool]$primaryF0.physical_b_launch_authorized -or
        [bool]$primaryF0.production_aim_changed -or
        -not [bool]$primaryF0.physical_b_primary_prepare_gate.ready -or
        [bool]$primaryF0.cross_run_holdout.prepare_allowed -or
        [string]$primaryF0.analyzer.file_sha256 -ne
            [string]$task.files.physical_b_analyzer.sha256 -or
        [string]$primaryF0.analysis_contract.contract_semantic_sha256 -ne
            [string]$task.f0.analysis_contract_semantic_sha256 -or
        [string]$primaryF0.f0_semantic_sha256 -ne
            [string]$task.f0.semantic_sha256 -or
        [string]$primaryF0.source_offline_design.file_sha256 -ne
            [string]$task.files.offline_design.sha256 -or
        [string]$primaryF0.source_a2_dependency_decision.file_sha256 -ne
            [string]$task.files.a2_dependency_decision.sha256) {
        throw "Physical B Primary F0 内容或源 artifact 绑定无效"
    }
}
if ($isBHoldoutTask) {
    $holdoutPlan = Get-Content -LiteralPath `
        ([string]$task.files.holdout_plan.path) -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([int]$holdoutPlan.schema_version -ne 2 -or
        [string]$holdoutPlan.evidence_type -ne
            "mouse_effect_probe_physical_b_holdout_plan" -or
        [string]$holdoutPlan.status -ne
            "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE" -or
        [bool]$holdoutPlan.physical_output_capability -or
        [bool]$holdoutPlan.physical_b_launch_authorized -or
        [bool]$holdoutPlan.production_aim_changed -or
        [bool]$holdoutPlan.holdout_used_for_tuning -or
        [int]$holdoutPlan.contract.schema_version -ne 2 -or
        [bool]$holdoutPlan.contract.different_source_clock_session_required -or
        -not [bool]$holdoutPlan.contract.same_source_clock_session_allowed -or
        -not [bool]$holdoutPlan.contract.different_timing_observation_required -or
        -not [bool]$holdoutPlan.contract.nonoverlapping_source_time_ranges_required -or
        -not [bool]$holdoutPlan.contract.event_frame_source_clock_session_match_required -or
        [string]$holdoutPlan.contract.contract_semantic_sha256 -ne
            [string]$task.holdout.independence_contract_semantic_sha256 -or
        [string]$holdoutPlan.bindings.holdout_analyzer_file_sha256 -ne
            [string]$task.files.holdout_analyzer.sha256 -or
        [string]$holdoutPlan.bindings.primary_analyzer_file_sha256 -ne
            [string]$task.files.physical_b_analyzer.sha256 -or
        [string]$holdoutPlan.bindings.primary_analysis_file_sha256 -ne
            [string]$task.files.primary_analysis.sha256 -or
        [string]$holdoutPlan.bindings.primary_command_report_file_sha256 -ne
            [string]$task.files.primary_command_report.sha256 -or
        [string]$holdoutPlan.bindings.offline_design_file_sha256 -ne
            [string]$task.files.offline_design.sha256 -or
        [string]$holdoutPlan.primary.run_uuid -ne
            [string]$task.primary.run_uuid -or
        [uint64]$holdoutPlan.primary.activation_epoch -ne
            [uint64]$task.primary.activation_epoch -or
        [string]$holdoutPlan.primary.source_clock_session_id -ne
            [string]$task.primary.source_clock_session_id -or
        [string]$holdoutPlan.primary.timing_observation.identity_basis -ne
            "run_uuid_activation_epoch_source_time_range" -or
        [string]$holdoutPlan.primary.timing_observation.identity_basis -ne
            [string]$task.primary.timing_observation.identity_basis -or
        [string]$holdoutPlan.primary.timing_observation.source_clock_session_id -ne
            [string]$task.primary.timing_observation.source_clock_session_id -or
        [int64]$holdoutPlan.primary.timing_observation.source_time_at_steady_ns.first -ne
            [int64]$task.primary.timing_observation.source_time_at_steady_ns.first -or
        [int64]$holdoutPlan.primary.timing_observation.source_time_at_steady_ns.last -ne
            [int64]$task.primary.timing_observation.source_time_at_steady_ns.last -or
        [int64]$holdoutPlan.primary.timing_observation.source_timestamp.first -ne
            [int64]$task.primary.timing_observation.source_timestamp.first -or
        [int64]$holdoutPlan.primary.timing_observation.source_timestamp.last -ne
            [int64]$task.primary.timing_observation.source_timestamp.last -or
        [string]$holdoutPlan.primary.f1_semantic_sha256 -ne
            [string]$task.primary.f1_semantic_sha256 -or
        [string]$holdoutPlan.sequence.profile -ne
            "physical_b_prbs_holdout" -or
        [uint64]$holdoutPlan.sequence.sample_count -ne 288 -or
        [uint64]$holdoutPlan.sequence.block_count -ne 2 -or
        [uint64]$holdoutPlan.sequence.expected_nonzero_transition_count -ne 68 -or
        [uint64]$holdoutPlan.sequence.lfsr.feedback_mask -ne 51 -or
        [uint64]$holdoutPlan.sequence.lfsr.phase -ne 21 -or
        [string]$holdoutPlan.holdout_plan_semantic_sha256 -ne
            [string]$task.holdout.plan_semantic_sha256) {
        throw "Physical B holdout plan/F1/source artifact 绑定无效"
    }
}
if ((Get-FileSha256 $PSCommandPath) -ne
        [string]$task.files.launch_script.sha256) {
    throw "当前 Launch script 不是 Prepare 固化的精确字节"
}

$forbiddenProcesses = @(
    Get-Process -Name Xen, XenLauncher, XenMouseBenchmark, XenMouseEffectProbe `
        -ErrorAction SilentlyContinue)
if ($forbiddenProcesses.Count -ne 0) {
        throw "Physical probe 要求正常 Aim 与其他 Mouse 工具关闭"
}

$pixelOutput = Join-Path $resolvedRun "pixel-evidence"
$reportPath = Join-Path $resolvedRun "command-report.json"
$safetyLedgerPath = Join-Path $resolvedRun "safety-ledger.json"
$launchSummaryPath = Join-Path $resolvedRun "launch-summary.json"
$s1BracketPath = Join-Path $resolvedRun "s1-liveness-bracket.json"
$s1SessionPath = Join-Path $resolvedRun "s1-session.json"
$sidecarLifecyclePath = Join-Path $resolvedRun "sidecar-lifecycle.json"
$sidecarStdout = Join-Path $resolvedRun "pixel-sidecar.stdout.log"
$sidecarStderr = Join-Path $resolvedRun "pixel-sidecar.stderr.log"
foreach ($path in @(
        $pixelOutput, $reportPath, $safetyLedgerPath, $launchSummaryPath,
        $s1BracketPath, $s1SessionPath,
        $sidecarLifecyclePath,
        $sidecarStdout, $sidecarStderr)) {
    if (Test-Path -LiteralPath $path) {
        throw "Physical A 输出已存在，拒绝重复 Launch：$path"
    }
}

$capture = $task.capture
$sidecarArguments = @(
    "--ndi-source", (Quote-NativeArgument ([string]$capture.source_name)),
    "--binding", (Quote-NativeArgument ([string]$task.files.probe_binding.path)),
    "--output", (Quote-NativeArgument $pixelOutput),
    "--frames", [string]$task.sidecar.frames,
    "--max-seconds", [string]$task.sidecar.max_seconds,
    "--frame-layout", [string]$capture.frame_layout,
    "--source-width", [string]$capture.source_width,
    "--source-height", [string]$capture.source_height,
    "--roi-width", [string]$capture.roi_width,
    "--roi-height", [string]$capture.roi_height,
    "--discovery-timeout-ms", [string]$capture.discovery_timeout_ms,
    "--receive-timeout-ms", [string]$capture.receive_timeout_ms,
    "--disconnect-timeout-ms", [string]$capture.disconnect_timeout_ms,
    "--clock-sync-url", (Quote-NativeArgument ([string]$capture.clock_sync_url)),
    "--clock-sync-interval-ms", [string]$capture.clock_sync_interval_ms,
    "--clock-sync-timeout-ms", [string]$capture.clock_sync_timeout_ms,
    "--clock-mapping-max-age-ms", [string]$capture.clock_mapping_max_age_ms,
    "--require-source-timing"
)
if (-not [bool]$capture.center_roi) {
    $sidecarArguments += @(
        "--roi-x", [string]$capture.roi_x,
        "--roi-y", [string]$capture.roi_y)
}
if ([bool]$capture.require_frame_metadata) {
    $sidecarArguments += "--require-frame-metadata"
}

Write-Host '【准备】保持右键松开；等待“按住右键”。'
$sidecarProcess = Start-Process -FilePath `
    ([string]$task.files.sidecar_executable.path) `
    -WorkingDirectory (Split-Path -Parent `
        ([string]$task.files.sidecar_executable.path)) `
    -ArgumentList ($sidecarArguments -join " ") -WindowStyle Hidden `
    -RedirectStandardOutput $sidecarStdout `
    -RedirectStandardError $sidecarStderr -PassThru
# Windows PowerShell 5 在重定向输出时可能于子进程退出后丢失 ExitCode；
# 启动后立即取得句柄，使后续 WaitForExit/ExitCode 保持同一进程身份。
[void]$sidecarProcess.Handle

try {
    $incoming = Wait-SidecarIncoming $sidecarProcess $resolvedRun 10000
    $copiedBinding = Join-Path $incoming "source-binding.json"
    if ((Get-FileSha256 $copiedBinding) -ne
        [string]$task.files.probe_binding.sha256) {
        throw "sidecar binding copy 与 Prepare SHA 不一致"
    }

    $probeArguments = @(
        "--mode", $expectedDispatchMode.Replace("_", "-"),
        "--config", [string]$task.files.config.path,
        "--sequence", [string]$task.files.sequence.path,
        "--binding", [string]$task.files.probe_binding.path,
        "--binding-sha256", [string]$task.files.probe_binding.sha256,
        "--sidecar-pid", [string]$sidecarProcess.Id,
        "--sidecar-incoming", $incoming,
        "--report", $reportPath,
        "--safety-ledger", $safetyLedgerPath,
        "--run-uuid", [string]$task.run_uuid,
        "--activation-epoch", [string]$task.activation_epoch,
        "--max-seconds", [string]$task.sidecar.max_seconds,
        "--allow-physical-output",
        "--confirm-physical-output", $confirmation
    )
    $operatorState = @{
        monitor_seen = $false
        terminal_seen = $false
    }
    & ([string]$task.files.probe_executable.path) @probeArguments 2>&1 | ForEach-Object {
        $nativeLine = [string]$_
        $cue = ConvertTo-PhysicalProbeOperatorCue $nativeLine
        if (-not [string]::IsNullOrWhiteSpace($cue)) {
            $isMonitorCue = $nativeLine.StartsWith("KMBOX monitor 已就绪")
            $isTerminalCue =
                $nativeLine.StartsWith("Mouse Effect Probe 时间线完成") -or
                $nativeLine.StartsWith("Mouse Effect Probe 未正常完成")
            if (($isMonitorCue -and -not $operatorState.monitor_seen) -or
                ($isTerminalCue -and -not $operatorState.terminal_seen)) {
                Write-Host $cue
            }
            if ($isMonitorCue) {
                $operatorState.monitor_seen = $true
            }
            if ($isTerminalCue) {
                $operatorState.terminal_seen = $true
            }
        }
    }
    $probeExitCode = $LASTEXITCODE
    if (-not $operatorState.terminal_seen) {
        Write-Host "【现在松开右键】命令阶段已结束；正在核对证据。"
        $operatorState.terminal_seen = $true
    }

    $recordingShutdownGraceSeconds = 15
    $discoverySeconds = [int][Math]::Ceiling(
        [double]$task.capture.discovery_timeout_ms / 1000.0)
    $recordingWaitSeconds = [int]$task.sidecar.max_seconds +
        $discoverySeconds + $recordingShutdownGraceSeconds
    $publishingMaxSeconds = if (
            $null -ne $task.sidecar.PSObject.Properties[
                "publishing_max_seconds"]) {
        [int]$task.sidecar.publishing_max_seconds
    } else {
        60
    }
    if ($recordingWaitSeconds -le 0 -or
        $recordingWaitSeconds -gt 180 -or
        $publishingMaxSeconds -le 0 -or $publishingMaxSeconds -gt 300) {
        throw "Physical A sidecar recording/publishing 时限合同非法"
    }
    $sidecarWait = Wait-SidecarCompletion `
        $sidecarProcess $incoming `
        ($recordingWaitSeconds * 1000) `
        ($publishingMaxSeconds * 1000)
    $forcedStop = $false
    $forcedStopCompleted = $false
    if ([bool]$sidecarWait.timed_out) {
        $forcedStop = $true
        $sidecarProcess.Refresh()
        if (-not $sidecarProcess.HasExited) {
            Stop-Process -Id $sidecarProcess.Id -Force
            $forcedStopCompleted = $sidecarProcess.WaitForExit(5000)
        } else {
            $forcedStopCompleted = $true
        }
    }
    $lifecyclePngCount = [uint64]$sidecarWait.png_count
    if (-not [bool]$sidecarWait.timed_out) {
        $publishedFramesDirectory = Join-Path $pixelOutput "frames"
        if (Test-Path -LiteralPath $publishedFramesDirectory -PathType Container) {
            $lifecyclePngCount = [uint64]@(
                Get-ChildItem -LiteralPath $publishedFramesDirectory `
                    -File -Filter "*.png"
            ).Count
        }
    }
    $sidecarLifecycle = [ordered]@{
        schema_version = 1
        evidence_type = "mouse_effect_probe_sidecar_lifecycle"
        physical_output_capability = $false
        run_uuid = [string]$task.run_uuid
        status = if ([bool]$sidecarWait.timed_out) { "TIMEOUT" } else { "EXITED" }
        phase = [string]$sidecarWait.phase
        elapsed_ms = [int64]$sidecarWait.elapsed_ms
        recording_wait_seconds = $recordingWaitSeconds
        publishing_max_seconds = $publishingMaxSeconds
        publishing_started_ms = $sidecarWait.publishing_started_ms
        png_count_at_completion = $lifecyclePngCount
        forced_stop = $forcedStop
        forced_stop_completed = $forcedStopCompleted
    }
    Write-NewUtf8Json $sidecarLifecyclePath $sidecarLifecycle
    if ([bool]$sidecarWait.timed_out) {
        $stderrTail = if (Test-Path -LiteralPath $sidecarStderr -PathType Leaf) {
            @(Get-Content -LiteralPath $sidecarStderr -Tail 1 `
                -ErrorAction SilentlyContinue) -join " "
        } else { "" }
        if ([string]::IsNullOrWhiteSpace($stderrTail)) {
            $stderrTail = "<empty>"
        }
        throw ("等待本次 Physical A sidecar 退出超时：phase=" +
            "$($sidecarWait.phase)；png_count=$($sidecarWait.png_count)；" +
            "stderr_tail=$stderrTail")
    }
    $sidecarProcess.Refresh()
    if ($sidecarProcess.ExitCode -ne 0) {
        throw "XenCaptureEvidence 失败，ExitCode=$($sidecarProcess.ExitCode)"
    }
    $manifestPath = Join-Path $pixelOutput "manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "Physical A 缺少 sidecar manifest 或 command report"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $safetyLedgerPath -PathType Leaf)) {
        throw "Physical A 缺少 safety monitor ledger"
    }
    $safetyLedger = Get-Content -LiteralPath $safetyLedgerPath `
        -Raw -Encoding utf8 | ConvertFrom-Json
    $safetyObservations = @($safetyLedger.observations)
    $safetyMonitorPackets = @($safetyLedger.monitor_packets)
    if ([uint64]$safetyLedger.schema_version -ne 2 -or
        [string]$safetyLedger.evidence_type -ne
            "mouse_effect_probe_safety_monitor_ledger" -or
        [bool]$safetyLedger.physical_output_capability -or
        [string]$safetyLedger.run_uuid -ne [string]$task.run_uuid -or
        [string]$safetyLedger.input_backend -ne "kmbox_net") {
        throw "Physical A safety monitor ledger 身份无效"
    }
    $acceptedMonitorSequences =
        [Collections.Generic.HashSet[uint64]]::new()
    $monitorPacketIdentityComplete =
        -not [bool]$safetyLedger.monitor_packet_recording_failed -and
        [uint64]$safetyLedger.dropped_monitor_packet_count -eq 0 -and
        $safetyMonitorPackets.Count -gt 0
    foreach ($packetIdentity in $safetyMonitorPackets) {
        $datagramSize = [uint64]$packetIdentity.datagram_size
        $packetSequenceBefore =
            [uint64]$packetIdentity.monitor_sequence_before
        $packetSequenceAfter =
            [uint64]$packetIdentity.monitor_sequence_after
        $packetSequence = $packetSequenceAfter
        $acceptedAsState = [bool]$packetIdentity.accepted_as_monitor_state
        $payloadSha256 = [string]$packetIdentity.payload_sha256
        if ([int64]$packetIdentity.received_at_steady_ns -le 0 -or
            [int]$packetIdentity.source_address_size -le 0 -or
            -not [bool]$packetIdentity.source_endpoint_valid -or
            [string]::IsNullOrWhiteSpace(
                [string]$packetIdentity.source_ipv4) -or
            [uint64]$packetIdentity.source_port -eq 0 -or
            [uint64]$packetIdentity.monitor_local_port -eq 0 -or
            [string]::IsNullOrWhiteSpace(
                [string]$packetIdentity.configured_device_ipv4) -or
            [uint64]$packetIdentity.configured_device_port -eq 0 -or
            [bool]$packetIdentity.source_port_matches_configured_device -ne
                ([uint64]$packetIdentity.source_port -eq
                    [uint64]$packetIdentity.configured_device_port) -or
            $payloadSha256 -notmatch '^[0-9a-f]{64}$' -or
            [bool]$packetIdentity.exact_monitor_packet_size -ne
                ($datagramSize -eq 20) -or
            [uint64]$packetIdentity.monitor_sequence -ne
                $packetSequenceAfter) {
            $monitorPacketIdentityComplete = $false
        }
        if ($acceptedAsState) {
            if ($packetSequence -eq 0 -or $datagramSize -lt 20 -or
                $packetSequenceAfter -ne ($packetSequenceBefore + 1) -or
                -not [bool]$packetIdentity.source_ip_matches_configured_device -or
                -not [bool]$packetIdentity.mouse_report_id_present -or
                -not [bool]$packetIdentity.mouse_buttons_present -or
                -not [bool]$packetIdentity.keyboard_report_id_present -or
                -not [bool]$packetIdentity.keyboard_modifiers_present -or
                -not $acceptedMonitorSequences.Add($packetSequence)) {
                $monitorPacketIdentityComplete = $false
            }
        } elseif ($packetSequenceBefore -ne $packetSequenceAfter -or
            [uint64]$packetIdentity.monitor_sequence -ne 0) {
            $monitorPacketIdentityComplete = $false
        }
    }
    foreach ($safetyObservation in $safetyObservations) {
        if ([bool]$safetyObservation.state_valid) {
            $observationSequence =
                [uint64]$safetyObservation.monitor_sequence
            if ($observationSequence -eq 0 -or
                -not $acceptedMonitorSequences.Contains(
                    $observationSequence)) {
                $monitorPacketIdentityComplete = $false
            }
        }
    }
    $terminalMonitorSequence = if ($safetyObservations.Count -gt 0) {
        [uint64]$safetyObservations[-1].monitor_sequence
    } else { [uint64]0 }
    $terminalMonitorPacketPayloadSha256 = ""
    if ($terminalMonitorSequence -gt 0) {
        foreach ($packetIdentity in $safetyMonitorPackets) {
            if ([bool]$packetIdentity.accepted_as_monitor_state -and
                [uint64]$packetIdentity.monitor_sequence_after -eq
                    $terminalMonitorSequence) {
                $terminalMonitorPacketPayloadSha256 =
                    [string]$packetIdentity.payload_sha256
                break
            }
        }
    }
    $sequence = Get-Content -LiteralPath `
        ([string]$task.files.sequence.path) -Raw -Encoding utf8 |
        ConvertFrom-Json
    $frames = @($manifest.frames)
    $events = @($report.result.events)
    $samples = @($sequence.samples)
    if ([string]$manifest.evidence_type -ne "output_off_capture" -or
        [bool]$manifest.physical_output_capability -or
        [string]$manifest.capture_source_name -ne
            [string]$capture.source_name -or
        [uint64]$manifest.recorded_frame_count -ne
            [uint64]$task.sidecar.frames -or
        $frames.Count -ne [uint64]$task.sidecar.frames -or
        [string]$manifest.source_binding.sha256 -ne
            [string]$task.files.probe_binding.sha256 -or
        [string]$report.run_uuid -ne [string]$task.run_uuid -or
        [string]$report.dispatch_mode -ne $expectedDispatchMode -or
        [string]$report.profile -ne [string]$task.profile -or
        [string]$sequence.profile -ne [string]$task.profile -or
        [string]$report.sequence_sha256 -ne
            [string]$task.sequence_sha256) {
        throw "Physical probe manifest/report 顶层身份无效"
    }
    if ($isBPrimaryTask) {
        $sequenceBlocks = @($sequence.blocks)
        $blockRoles = @($sequenceBlocks | ForEach-Object {
            [string]$_.role
        })
        $blockPolarities = @($sequenceBlocks | ForEach-Object {
            [string]$_.polarity
        })
        if ([int]$sequence.schema -ne 5 -or
            [string]$sequence.profile -ne "physical_b_prbs_primary" -or
            [string]$sequence.request.offline_sequence_semantic_sha256 -ne
                [string]$task.offline_sequence_semantic_sha256 -or
            [uint64]$sequence.request.guard_sample_count -ne 32 -or
            [uint64]$sequence.request.lfsr_order -ne 6 -or
            [uint64]$sequence.request.feedback_mask -ne 39 -or
            [uint64]$sequence.request.seed -ne 1 -or
            [uint64]$sequence.request.phase -ne 49 -or
            $samples.Count -ne 800 -or
            $sequenceBlocks.Count -ne 6 -or
            [int64]$sequence.summary.net_x_counts -ne 0 -or
            [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
            ($blockRoles -join ",") -ne
                "estimation,estimation,selection,selection,confirmation,confirmation" -or
            ($blockPolarities -join ",") -ne
                "normal,inverted,normal,inverted,normal,inverted" -or
            @($sequenceBlocks | Where-Object {
                [uint64]$_.period_sample_count -ne 63 -or
                [uint64]$_.sample_count -ne 64
            }).Count -ne 0 -or
            @($samples | Where-Object {
                [int]$_.dx_counts -lt -1 -or [int]$_.dx_counts -gt 1 -or
                [int]$_.dy_counts -ne 0
            }).Count -ne 0) {
            throw "Physical B Primary sequence/F0/whole-block 合同无效"
        }
    }
    if ($isBHoldoutTask) {
        $sequenceBlocks = @($sequence.blocks)
        $blockRoles = @($sequenceBlocks | ForEach-Object {
            [string]$_.role
        })
        $blockPolarities = @($sequenceBlocks | ForEach-Object {
            [string]$_.polarity
        })
        if ([int]$sequence.schema -ne 5 -or
            [string]$sequence.profile -ne "physical_b_prbs_holdout" -or
            [string]$sequence.request.offline_sequence_semantic_sha256 -ne
                [string]$task.offline_sequence_semantic_sha256 -or
            [uint64]$sequence.request.guard_sample_count -ne 32 -or
            [uint64]$sequence.request.lfsr_order -ne 6 -or
            [uint64]$sequence.request.feedback_mask -ne 51 -or
            [uint64]$sequence.request.seed -ne 1 -or
            [uint64]$sequence.request.phase -ne 21 -or
            $samples.Count -ne 288 -or
            $sequenceBlocks.Count -ne 2 -or
            [int64]$sequence.summary.net_x_counts -ne 0 -or
            [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
            ($blockRoles -join ",") -ne
                "cross_run_holdout,cross_run_holdout" -or
            ($blockPolarities -join ",") -ne "normal,inverted" -or
            [uint64]$sequenceBlocks[0].first_sample_index -ne 32 -or
            [uint64]$sequenceBlocks[1].first_sample_index -ne 160 -or
            @($sequenceBlocks | Where-Object {
                [uint64]$_.period_sample_count -ne 63 -or
                [uint64]$_.sample_count -ne 64
            }).Count -ne 0 -or
            @($samples | Where-Object {
                [int]$_.dx_counts -lt -1 -or [int]$_.dx_counts -gt 1 -or
                [int]$_.dy_counts -ne 0
            }).Count -ne 0) {
            throw "Physical B holdout sequence/F1/whole-block 合同无效"
        }
    }
    if ($isBMagnitudePrimaryTask) {
        $sequenceBlocks = @($sequence.blocks)
        $blockRoles = @($sequenceBlocks | ForEach-Object {
            [string]$_.role
        })
        $blockPolarities = @($sequenceBlocks | ForEach-Object {
            [string]$_.polarity
        })
        $amplitudeOrder = @($sequenceBlocks | ForEach-Object {
            [int]$_.amplitude_counts
        })
        if ([int]$sequence.schema -ne 6 -or
            [string]$sequence.profile -ne
                "physical_b_command_magnitude_primary" -or
            [string]$sequence.request.run_role -ne "primary" -or
            [uint64]$sequence.request.baseline_sample_count -ne 64 -or
            [uint64]$sequence.request.response_sample_count -ne 48 -or
            [uint64]$sequence.request.guard_sample_count -ne 32 -or
            $samples.Count -ne 1684 -or
            $sequenceBlocks.Count -ne 10 -or
            [int64]$sequence.summary.net_x_counts -ne 0 -or
            [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 13 -or
            ($amplitudeOrder -join ",") -ne
                "1,1,4,4,13,13,2,2,8,8" -or
            ($blockRoles -join ",") -ne
                "estimation,estimation,estimation,estimation,estimation,estimation,confirmation,confirmation,confirmation,confirmation" -or
            ($blockPolarities -join ",") -ne
                "normal,inverted,normal,inverted,normal,inverted,normal,inverted,normal,inverted" -or
            @($samples | Where-Object {
                [int]$_.dy_counts -ne 0 -or
                ([math]::Abs([int]$_.dx_counts) -notin
                    @(0, 1, 2, 4, 8, 13))
            }).Count -ne 0) {
            throw "Physical B command-magnitude sequence 合同无效"
        }
        for ($blockIndex = 0;
             $blockIndex -lt $sequenceBlocks.Count;
             $blockIndex++) {
            $block = $sequenceBlocks[$blockIndex]
            $first = 64 + 162 * $blockIndex
            $direction = if ([string]$block.polarity -eq "normal") {
                1
            } else {
                -1
            }
            $amplitude = [int]$block.amplitude_counts
            if ([uint64]$block.first_sample_index -ne $first -or
                [uint64]$block.sample_count -ne 162 -or
                [int]$samples[$first + 32].dx_counts -ne
                    $direction * $amplitude -or
                [int]$samples[$first + 81].dx_counts -ne
                    -$direction * $amplitude) {
                throw "Physical B command-magnitude block/pulse/return 边界无效"
            }
        }
    }

    $frameTimestamps = [Collections.Generic.HashSet[int64]]::new()
    $frameSourceSessions = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $frameSessionByTimestamp = @{}
    $pngVerified = 0
    foreach ($frame in $frames) {
        if (-not [bool]$frame.source_timestamp_valid -or
            -not [bool]$frame.source_time_timing_valid -or
            [string]$frame.source_clock_status -ne "VALID" -or
            [uint64]$frame.source_dropped_frames -ne 0 -or
            [uint64]$frame.transport_dropped_frames -ne 0 -or
            [uint64]$frame.transport_invalid_packets -ne 0) {
            throw "Physical A sidecar 存在无效 source timing/drop frame"
        }
        $frameTimestamp = [int64]$frame.source_timestamp
        $frameSession = [string]$frame.source_clock_session_id
        if ($frameTimestamp -le 0 -or
            -not $frameTimestamps.Add($frameTimestamp) -or
            [string]::IsNullOrWhiteSpace($frameSession)) {
            throw "Physical A sidecar source timestamp/session 非法或重复"
        }
        [void]$frameSourceSessions.Add($frameSession)
        $frameSessionByTimestamp[$frameTimestamp] = $frameSession
        $pngPath = Join-Path $pixelOutput ([string]$frame.file)
        if ((Get-FileSha256 $pngPath) -ne [string]$frame.png_sha256) {
            throw "Physical A sidecar PNG SHA 不匹配：$pngPath"
        }
        $pngVerified++
    }
    if ($isBHoldoutTask -and $frameSourceSessions.Count -ne 1) {
        throw "Physical B holdout sidecar source session 不唯一"
    }

    $matchedEvents = 0
    $unmatchedBaselineEvents = 0
    $maxUnmatchedBaselineEvents = if ($isA2S1Task -or $isBTask) {
        0
    } else {
        1
    }
    $allowUnmatchedBaseline = $isA1Task -or $isA2Task
    $baselineSampleCount = if ($allowUnmatchedBaseline) {
        [uint64]$sequence.request.baseline_sample_count
    } else {
        [uint64]0
    }
    $completedPulses = 0
    $crossRunIndependence = $null
    $holdoutSourceSessions = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $holdoutFirstSteadyNs = [int64]0
    $holdoutLastSteadyNs = [int64]0
    $holdoutFirstSourceTimestamp = [int64]0
    $holdoutLastSourceTimestamp = [int64]0
    foreach ($event in $events) {
        $sampleIndex = [int]$event.sample_index
        if ($sampleIndex -lt 0 -or $sampleIndex -ge $samples.Count) {
            throw "Physical A event sample_index 越界"
        }
        $sample = $samples[$sampleIndex]
        if ([int]$event.nominal_dx_counts -ne [int]$sample.dx_counts -or
            [int]$event.nominal_dy_counts -ne 0 -or
            [int]$event.requested_dy_counts -ne 0 -or
            [uint64]$event.source_dropped_frames -ne 0 -or
            [uint64]$event.transport_dropped_frames -ne 0 -or
            [uint64]$event.transport_invalid_packets -ne 0) {
            throw "Physical A event 的序列/timing/零 Y 合同无效"
        }
        $coverage = Get-PhysicalEventFrameCoverage `
            $event $sample $frameTimestamps $allowUnmatchedBaseline `
            $baselineSampleCount
        if ([bool]$coverage.required_source_frame_missing) {
            throw "Physical A event 缺少必须的同 source timestamp sidecar frame"
        }
        if ([bool]$coverage.matched) {
            $matchedEvents++
        } elseif ([bool]$coverage.unmatched_baseline_allowed) {
            $unmatchedBaselineEvents++
        }
        if ($isBHoldoutTask) {
            $eventSession = [string]$event.source_clock_session_id
            $eventSteadyNs = [int64]$event.source_time_at_steady_ns
            $eventSourceTimestamp = [int64]$event.source_timestamp
            if ([string]::IsNullOrWhiteSpace($eventSession) -or
                $eventSteadyNs -le 0 -or $eventSourceTimestamp -le 0 -or
                ($holdoutLastSteadyNs -gt 0 -and
                    $eventSteadyNs -le $holdoutLastSteadyNs) -or
                ($holdoutLastSourceTimestamp -gt 0 -and
                    $eventSourceTimestamp -le $holdoutLastSourceTimestamp) -or
                -not $frameSessionByTimestamp.ContainsKey(
                    $eventSourceTimestamp) -or
                [string]$frameSessionByTimestamp[$eventSourceTimestamp] -ne
                    $eventSession) {
                throw "Physical B holdout event/frame source session 或时间顺序无效"
            }
            if ($holdoutFirstSteadyNs -eq 0) {
                $holdoutFirstSteadyNs = $eventSteadyNs
                $holdoutFirstSourceTimestamp = $eventSourceTimestamp
            }
            $holdoutLastSteadyNs = $eventSteadyNs
            $holdoutLastSourceTimestamp = $eventSourceTimestamp
            [void]$holdoutSourceSessions.Add($eventSession)
        }
        if ([int]$sample.dx_counts -eq 0) {
            if ([bool]$event.dispatch_attempted -or
                [int]$event.requested_dx_counts -ne 0 -or
                [bool]$event.backend_succeeded -or
                [bool]$event.protocol_ack_received) {
                throw "Physical A 零 sample 被伪造成实际命令"
            }
        } else {
            if (-not [bool]$event.dispatch_attempted -or
                [int]$event.requested_dx_counts -ne [int]$sample.dx_counts -or
                -not [bool]$event.backend_succeeded -or
                -not [bool]$event.protocol_ack_received) {
                throw "Physical A pulse 缺少 request/backend/ACK 守恒"
            }
            $completedPulses++
        }
    }
    if ($unmatchedBaselineEvents -gt $maxUnmatchedBaselineEvents) {
        throw "Physical A baseline 的未观测零事件超过预注册上限"
    }
    if ($isBHoldoutTask) {
        # same stable source clock session is allowed；它是 server epoch，
        # cross-Run 独立性由 Run/activation 与两种 source 时间窗共同证明。
        if ($holdoutSourceSessions.Count -ne 1 -or
            $frameSourceSessions.Count -ne 1) {
            throw "Physical B holdout Run 内 source session 不唯一"
        }
        $holdoutSourceSession = @($holdoutSourceSessions)[0]
        if ($holdoutSourceSession -ne @($frameSourceSessions)[0]) {
            throw "Physical B holdout event/frame source session 不一致"
        }
        $primaryObservation = $task.primary.timing_observation
        $primaryFirstSteadyNs =
            [int64]$primaryObservation.source_time_at_steady_ns.first
        $primaryLastSteadyNs =
            [int64]$primaryObservation.source_time_at_steady_ns.last
        $primaryFirstSourceTimestamp =
            [int64]$primaryObservation.source_timestamp.first
        $primaryLastSourceTimestamp =
            [int64]$primaryObservation.source_timestamp.last
        if ([string]$primaryObservation.identity_basis -ne
                "run_uuid_activation_epoch_source_time_range" -or
            [string]$primaryObservation.source_clock_session_id -ne
                [string]$task.primary.source_clock_session_id -or
            $primaryFirstSteadyNs -le 0 -or
            $primaryLastSteadyNs -lt $primaryFirstSteadyNs -or
            $primaryFirstSourceTimestamp -le 0 -or
            $primaryLastSourceTimestamp -lt $primaryFirstSourceTimestamp) {
            throw "Physical B holdout Primary timing observation 无效"
        }
        $sourceTimeRangesOverlap = -not (
            $primaryLastSteadyNs -lt $holdoutFirstSteadyNs -or
            $holdoutLastSteadyNs -lt $primaryFirstSteadyNs)
        $sourceTimestampRangesOverlap = -not (
            $primaryLastSourceTimestamp -lt $holdoutFirstSourceTimestamp -or
            $holdoutLastSourceTimestamp -lt $primaryFirstSourceTimestamp)
        if ($sourceTimeRangesOverlap -or $sourceTimestampRangesOverlap) {
            throw "Physical B holdout source timing observation 时间窗重叠"
        }
        $holdoutObservation = [ordered]@{
            identity_basis = "run_uuid_activation_epoch_source_time_range"
            source_clock_session_id = $holdoutSourceSession
            source_time_at_steady_ns = [ordered]@{
                first = $holdoutFirstSteadyNs
                last = $holdoutLastSteadyNs
            }
            source_timestamp = [ordered]@{
                first = $holdoutFirstSourceTimestamp
                last = $holdoutLastSourceTimestamp
            }
        }
        $crossRunIndependence = [ordered]@{
            identity_basis = "run_uuid_activation_epoch_source_time_range"
            different_run_uuid = $true
            different_activation_epoch = $true
            different_timing_observation = $true
            different_source_clock_session =
                $holdoutSourceSession -ne
                    [string]$task.primary.source_clock_session_id
            same_source_clock_session_allowed = $true
            source_time_ranges_overlap = $false
            source_timestamp_ranges_overlap = $false
            primary = $primaryObservation
            holdout = $holdoutObservation
        }
    }

    $expectedPulseCount = if ($isA2Task -or $isA2S1Task -or $isBTask) {
        [uint64]$task.expected_nonzero_transition_count
    } else {
        [uint64]4
    }
    $executionComplete =
        $probeExitCode -eq 0 -and
        [string]$report.result.state -eq "completed" -and
        [bool]$report.result.complete -and
        [string]$report.result.stop_reason -eq "normal_completion" -and
        $events.Count -eq $samples.Count -and
        $completedPulses -eq $expectedPulseCount -and
        -not [bool]$safetyLedger.recording_failed -and
        [uint64]$safetyLedger.dropped_observation_count -eq 0 -and
        $safetyObservations.Count -gt 0 -and
        $monitorPacketIdentityComplete -and
        [int64]$report.result.cumulative_requested_x_counts -eq 0 -and
        [int64]$report.result.cumulative_backend_completed_x_counts -eq 0
    $summary = [ordered]@{
        schema_version = if ($isBMagnitudeTask) {
            6
        } elseif ($isBHoldoutTask) {
            5
        } elseif ($isBTask) {
            4
        } elseif ($isA2S1Task) {
            3
        } elseif ($isA2Task) {
            2
        } else {
            1
        }
        evidence_type = if ($isBMagnitudeTask) {
            "mouse_effect_probe_b_command_magnitude_launch"
        } elseif ($isA2S1Task) {
            "mouse_effect_probe_a2_s1_launch"
        } elseif ($isA2Task) {
            "mouse_effect_probe_a2_launch"
        } elseif ($isBTask) {
            "mouse_effect_probe_b_launch"
        } else {
            "mouse_effect_probe_a_launch"
        }
        status = if ($executionComplete) {
            "RECORDED_UNANALYZED"
        } else {
            "STOPPED_UNANALYZED"
        }
        run_uuid = [string]$task.run_uuid
        activation_epoch = [uint64]$task.activation_epoch
        sequence_sha256 = [string]$task.sequence_sha256
        profile = [string]$task.profile
        expected_nonzero_transition_count = $expectedPulseCount
        command_report_sha256 = [string]$report.report_sha256
        safety_ledger_sha256 = Get-FileSha256 $safetyLedgerPath
        safety_monitor_terminal_decision =
            [string]$safetyLedger.terminal_decision
        safety_monitor_observation_count =
            [uint64]$safetyObservations.Count
        safety_monitor_dropped_observation_count =
            [uint64]$safetyLedger.dropped_observation_count
        safety_monitor_recording_failed =
            [bool]$safetyLedger.recording_failed
        safety_monitor_packet_count =
            [uint64]$safetyMonitorPackets.Count
        safety_monitor_dropped_packet_count =
            [uint64]$safetyLedger.dropped_monitor_packet_count
        safety_monitor_packet_recording_failed =
            [bool]$safetyLedger.monitor_packet_recording_failed
        safety_monitor_packet_identity_complete =
            [bool]$monitorPacketIdentityComplete
        safety_monitor_terminal_sequence = $terminalMonitorSequence
        safety_monitor_terminal_packet_payload_sha256 =
            $terminalMonitorPacketPayloadSha256
        sidecar_manifest_sha256 = Get-FileSha256 $manifestPath
        sidecar_lifecycle_sha256 = Get-FileSha256 $sidecarLifecyclePath
        stop_reason = [string]$report.result.stop_reason
        command_event_count = [uint64]$events.Count
        source_timestamp_matched_event_count = [uint64]$matchedEvents
        source_timestamp_unmatched_baseline_event_count =
            [uint64]$unmatchedBaselineEvents
        source_timestamp_unmatched_baseline_event_limit =
            [uint64]$maxUnmatchedBaselineEvents
        backend_completed_pulse_count = [uint64]$completedPulses
        sidecar_frame_count = [uint64]$frames.Count
        png_hash_verified_count = [uint64]$pngVerified
        requested_net_x_counts = [int64]$report.result.cumulative_requested_x_counts
        backend_completed_net_x_counts = [int64]$report.result.cumulative_backend_completed_x_counts
        visible_effect_analyzed = $false
        human_observation_received = $false
    }
    if ($isA2Task -or $isA2S1Task -or $isBTask) {
        $summary.run_role = [string]$task.run_role
        $summary.scope_id = [string]$task.scope_id
    }
    if ($isBMagnitudeTask) {
        $summary.validation_used_for_refit = $false
        $summary.new_production_gain_claimed = $false
    }
    if ($isBHoldoutTask) {
        $summary.cross_run_independence = $crossRunIndependence
    }
    Write-NewUtf8Json $launchSummaryPath $summary
    if (-not $executionComplete) {
        throw "Physical probe 已停止且未补偿：stop_reason=$($report.result.stop_reason)"
    }
    if ($isA2S1Task) {
        $request = $sequence.request
        $policy = $task.liveness_policy
        $challengePulseCount = [uint64]$request.challenge_pulse_count
        $challengeStride = [uint64]$request.challenge_stride_sample_count
        $peakHoldCount = [uint64]$request.peak_hold_sample_count
        $settleCount = [uint64]$request.settle_sample_count
        $baselineCount = [uint64]$request.baseline_sample_count
        $challengeCount =
            2 * $challengePulseCount * $challengeStride + $peakHoldCount
        $expectedSamples = 2 * $challengeCount + $settleCount + $baselineCount
        if ($challengePulseCount -ne
                [uint64]$policy.challenge_pulse_count -or
            $challengeStride -ne
                [uint64]$policy.challenge_stride_sample_count -or
            $peakHoldCount -eq 0 -or
            $peakHoldCount -ne [uint64]$policy.peak_hold_sample_count -or
            [bool]$policy.peak_hold_frames_eligible_for_estimands -or
            $settleCount -ne [uint64]$policy.settle_sample_count -or
            $baselineCount -ne [uint64]$policy.baseline_frame_count -or
            $expectedSamples -ne $samples.Count) {
            throw "Physical A2 S1 sequence/request/policy 容量不守恒"
        }

        $holdSamples = @($samples | Where-Object {
            [string]$_.phase -eq "hold"
        })
        if ($holdSamples.Count -ne 2 * $peakHoldCount) {
            throw "Physical A2 S1 peak hold sample_count 不守恒"
        }
        foreach ($holdSample in $holdSamples) {
            $holdIndex = [int]$holdSample.sample_index
            $holdEvent = $events[$holdIndex]
            if ([int]$holdSample.dx_counts -ne 0 -or
                [int]$holdSample.dy_counts -ne 0 -or
                [int]$holdEvent.sample_index -ne $holdIndex -or
                [int]$holdEvent.nominal_dx_counts -ne 0 -or
                [int]$holdEvent.requested_dx_counts -ne 0 -or
                [bool]$holdEvent.dispatch_attempted -or
                [bool]$holdEvent.backend_succeeded -or
                [bool]$holdEvent.protocol_ack_received) {
                throw "Physical A2 S1 peak hold 不是未 dispatch 的精确零命令平台"
            }
        }

        $phaseDefinitions = @(
            [ordered]@{
                name = "PRE_LIVENESS_CHALLENGE"
                first_sample_index = [uint64]0
                last_sample_index = [uint64]($challengeCount - 1)
            },
            [ordered]@{
                name = "RELEASE_AND_SETTLE"
                first_sample_index = [uint64]$challengeCount
                last_sample_index = [uint64](
                    $challengeCount + $settleCount - 1)
            },
            [ordered]@{
                name = "BASELINE_ZERO"
                first_sample_index = [uint64](
                    $challengeCount + $settleCount)
                last_sample_index = [uint64](
                    $challengeCount + $settleCount + $baselineCount - 1)
            },
            [ordered]@{
                name = "POST_LIVENESS_CHALLENGE"
                first_sample_index = [uint64](
                    $challengeCount + $settleCount + $baselineCount)
                last_sample_index = [uint64]($samples.Count - 1)
            }
        )
        foreach ($phase in $phaseDefinitions) {
            $first = [int]$phase.first_sample_index
            $last = [int]$phase.last_sample_index
            $phaseSamples = @($samples[$first..$last])
            $phaseEvents = @($events[$first..$last])
            if ($phaseSamples.Count -ne $phaseEvents.Count -or
                [int]$phaseEvents[0].sample_index -ne $first -or
                [int]$phaseEvents[-1].sample_index -ne $last) {
                throw "Physical A2 S1 phase sample/event 边界不一致"
            }
            if ([string]$phase.name -eq "RELEASE_AND_SETTLE" -and
                @($phaseSamples | Where-Object {
                    [string]$_.phase -ne "guard" -or
                    [int]$_.dx_counts -ne 0 -or [int]$_.dy_counts -ne 0
                }).Count -ne 0) {
                throw "Physical A2 S1 settle 不是精确零命令 guard"
            }
            if ([string]$phase.name -eq "BASELINE_ZERO" -and
                @($phaseSamples | Where-Object {
                    [string]$_.phase -ne "baseline" -or
                    [int]$_.dx_counts -ne 0 -or [int]$_.dy_counts -ne 0 -or
                    [bool]$phaseEvents[$_.sample_index - $first].dispatch_attempted
                }).Count -ne 0) {
                throw "Physical A2 S1 baseline 不是精确零命令窗口"
            }
        }

        $commandReportFileSha = Get-FileSha256 $reportPath
        $bracket = [ordered]@{
            schema_version = 2
            evidence_type = "mouse_effect_probe_a2_s1_liveness_bracket"
            physical_output_capability = $true
            automated_input_generated = $true
            input_backend = "kmbox_net"
            manual_motion_required = $false
            phase_join_basis =
                "command_event_source_timestamp_to_manifest"
            sequence_sha256 = [string]$sequence.sequence_sha256
            sequence_file_sha256 = [string]$task.files.sequence.sha256
            command_report_sha256 = $commandReportFileSha
            policy = [ordered]@{
                policy_id = [string]$policy.policy_id
                baseline_frame_count = $baselineCount
                challenge_pulse_count = $challengePulseCount
                challenge_stride_sample_count = $challengeStride
                peak_hold_sample_count = $peakHoldCount
                challenge_frames_eligible_for_estimands = $false
                peak_hold_frames_eligible_for_estimands = $false
                settle_frames_eligible_for_estimands = $false
            }
            phases = $phaseDefinitions
        }
        Write-NewUtf8Json $s1BracketPath $bracket

        $session = [ordered]@{
            schema_version = 2
            evidence_type = "mouse_effect_probe_a2_s1_session"
            status = "RECORDED_UNANALYZED"
            capture_mode = "bracketed_kmbox"
            physical_output_capability = $true
            probe_started = $true
            mouse_opened = $true
            actual_command_zero = $false
            probe_command_zero = $false
            baseline_actual_command_zero = $true
            automated_kmbox_challenge = $true
            challenge_frames_excluded_from_estimands = $true
            peak_hold_frames_excluded_from_estimands = $true
            aim_off = $true
            run_uuid = [string]$task.run_uuid
            run_role = [string]$task.run_role
            scope_id = [string]$task.scope_id
            capture_process_session_id = [guid]::NewGuid().ToString()
            frame_count = [uint64]$frames.Count
            obs_source_binding_sha256 =
                [string]$task.files.obs_source_binding.sha256
            probe_binding_sha256 =
                [string]$task.files.probe_binding.sha256
            manifest_sha256 = Get-FileSha256 $manifestPath
            sidecar_lifecycle_sha256 = Get-FileSha256 $sidecarLifecyclePath
            sequence_sha256 = [string]$sequence.sequence_sha256
            sequence_file_sha256 = [string]$task.files.sequence.sha256
            command_report_sha256 = $commandReportFileSha
            liveness_bracket_sha256 = Get-FileSha256 $s1BracketPath
        }
        Write-NewUtf8Json $s1SessionPath $session
    }
    Write-Host "【记录完成】本次尚未分析可见效果。请反馈：是否看到视角偏移；是否移动鼠标/WASD；是否异常或急停。"
} finally {
    if ($null -ne $sidecarProcess) {
        $sidecarProcess.Refresh()
        if (-not $sidecarProcess.HasExited) {
            Stop-Process -Id $sidecarProcess.Id -Force
        }
        $sidecarProcess.Dispose()
    }
}
