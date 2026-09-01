param(
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$confirmation = "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT"
if (-not $AllowPhysicalOutput.IsPresent -or
    $PhysicalOutputConfirmation -ne $confirmation) {
    throw "Physical A 会发送真实 KMBOX 输入，必须同时提供物理输出开关和固定确认令牌。"
}

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
if ((-not $isA1Task -and -not $isA2Task -and -not $isA2S1Task) -or
    [string]$task.status -ne "PREPARED" -or
    [string]$task.dispatch_mode -ne "physical_a" -or
    -not [bool]$task.physical_output_capability -or
    [string]$task.run_directory -ne $resolvedRun -or
    [string]$task.physical_output_confirmation -ne $confirmation) {
    throw "Physical A task 身份或授权合同无效"
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
        [bool]$task.liveness_policy.challenge_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.settle_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.fixed_pixel_speed_used_as_gate) {
        throw "Physical A2 S1 role/profile/liveness policy 合同无效"
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
foreach ($entry in $fileEntries) {
    Assert-FileEvidence $entry.value $entry.name
}
if ((Get-FileSha256 $PSCommandPath) -ne
        [string]$task.files.launch_script.sha256) {
    throw "当前 Launch script 不是 Prepare 固化的精确字节"
}

$forbiddenProcesses = @(
    Get-Process -Name Xen, XenLauncher, XenMouseBenchmark, XenMouseEffectProbe `
        -ErrorAction SilentlyContinue)
if ($forbiddenProcesses.Count -ne 0) {
    throw "Physical A 要求正常 Aim 与其他 Mouse 工具关闭"
}

$pixelOutput = Join-Path $resolvedRun "pixel-evidence"
$reportPath = Join-Path $resolvedRun "command-report.json"
$launchSummaryPath = Join-Path $resolvedRun "launch-summary.json"
$s1BracketPath = Join-Path $resolvedRun "s1-liveness-bracket.json"
$s1SessionPath = Join-Path $resolvedRun "s1-session.json"
$sidecarStdout = Join-Path $resolvedRun "pixel-sidecar.stdout.log"
$sidecarStderr = Join-Path $resolvedRun "pixel-sidecar.stderr.log"
foreach ($path in @(
        $pixelOutput, $reportPath, $launchSummaryPath,
        $s1BracketPath, $s1SessionPath,
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

$probeLabel = if ($isA2S1Task) {
    "A2 S1 $([string]$task.run_role) 固定 cadence X-only 活性 bracket"
} elseif ($isA2Task) {
    "A2 依赖校准 $([string]$task.run_role) ±1 X probe"
} else {
    "A 级稀疏 ±1 X probe"
}
Write-Host "即将执行 $probeLabel。启动命令时请先保持右键松开；probe 提示 monitor 已就绪后，再在 5 秒内按住右键并持续保持。随后松开右键、End 或 F8 会立即停发，且不会补偿。"
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
        "--mode", "physical-a",
        "--config", [string]$task.files.config.path,
        "--sequence", [string]$task.files.sequence.path,
        "--binding", [string]$task.files.probe_binding.path,
        "--binding-sha256", [string]$task.files.probe_binding.sha256,
        "--sidecar-pid", [string]$sidecarProcess.Id,
        "--sidecar-incoming", $incoming,
        "--report", $reportPath,
        "--run-uuid", [string]$task.run_uuid,
        "--activation-epoch", [string]$task.activation_epoch,
        "--max-seconds", [string]$task.sidecar.max_seconds,
        "--allow-physical-output",
        "--confirm-physical-output", $confirmation
    )
    & ([string]$task.files.probe_executable.path) @probeArguments
    $probeExitCode = $LASTEXITCODE

    if (-not $sidecarProcess.WaitForExit(
            [int](($task.sidecar.max_seconds + 15) * 1000))) {
        Stop-Process -Id $sidecarProcess.Id -Force
        throw "等待本次 Physical A sidecar 退出超时"
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
        [string]$report.dispatch_mode -ne "physical_a" -or
        [string]$report.profile -ne [string]$task.profile -or
        [string]$sequence.profile -ne [string]$task.profile -or
        [string]$report.sequence_sha256 -ne
            [string]$task.sequence_sha256) {
        throw "Physical A manifest/report 顶层身份无效"
    }

    $frameTimestamps = [Collections.Generic.HashSet[int64]]::new()
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
        [void]$frameTimestamps.Add([int64]$frame.source_timestamp)
        $pngPath = Join-Path $pixelOutput ([string]$frame.file)
        if ((Get-FileSha256 $pngPath) -ne [string]$frame.png_sha256) {
            throw "Physical A sidecar PNG SHA 不匹配：$pngPath"
        }
        $pngVerified++
    }

    $matchedEvents = 0
    $completedPulses = 0
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
            [uint64]$event.transport_invalid_packets -ne 0 -or
            -not $frameTimestamps.Contains([int64]$event.source_timestamp)) {
            throw "Physical A event 的序列/source/零 Y 合同无效"
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
        $matchedEvents++
    }

    $expectedPulseCount = if ($isA2Task -or $isA2S1Task) {
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
        [int64]$report.result.cumulative_requested_x_counts -eq 0 -and
        [int64]$report.result.cumulative_backend_completed_x_counts -eq 0
    $summary = [ordered]@{
        schema_version = if ($isA2S1Task) { 3 } elseif ($isA2Task) { 2 } else { 1 }
        evidence_type = if ($isA2S1Task) {
            "mouse_effect_probe_a2_s1_launch"
        } elseif ($isA2Task) {
            "mouse_effect_probe_a2_launch"
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
        sidecar_manifest_sha256 = Get-FileSha256 $manifestPath
        stop_reason = [string]$report.result.stop_reason
        command_event_count = [uint64]$events.Count
        source_timestamp_matched_event_count = [uint64]$matchedEvents
        backend_completed_pulse_count = [uint64]$completedPulses
        sidecar_frame_count = [uint64]$frames.Count
        png_hash_verified_count = [uint64]$pngVerified
        requested_net_x_counts = [int64]$report.result.cumulative_requested_x_counts
        backend_completed_net_x_counts = [int64]$report.result.cumulative_backend_completed_x_counts
        visible_effect_analyzed = $false
        human_observation_received = $false
    }
    if ($isA2Task -or $isA2S1Task) {
        $summary.run_role = [string]$task.run_role
        $summary.scope_id = [string]$task.scope_id
    }
    Write-NewUtf8Json $launchSummaryPath $summary
    if (-not $executionComplete) {
        throw "Physical A 已停止且未补偿：stop_reason=$($report.result.stop_reason)"
    }
    if ($isA2S1Task) {
        $request = $sequence.request
        $policy = $task.liveness_policy
        $challengePulseCount = [uint64]$request.challenge_pulse_count
        $challengeStride = [uint64]$request.challenge_stride_sample_count
        $settleCount = [uint64]$request.settle_sample_count
        $baselineCount = [uint64]$request.baseline_sample_count
        $challengeCount = 2 * $challengePulseCount * $challengeStride
        $expectedSamples = 2 * $challengeCount + $settleCount + $baselineCount
        if ($challengePulseCount -ne
                [uint64]$policy.challenge_pulse_count -or
            $challengeStride -ne
                [uint64]$policy.challenge_stride_sample_count -or
            $settleCount -ne [uint64]$policy.settle_sample_count -or
            $baselineCount -ne [uint64]$policy.baseline_frame_count -or
            $expectedSamples -ne $samples.Count) {
            throw "Physical A2 S1 sequence/request/policy 容量不守恒"
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
                challenge_frames_eligible_for_estimands = $false
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
            sequence_sha256 = [string]$sequence.sequence_sha256
            sequence_file_sha256 = [string]$task.files.sequence.sha256
            command_report_sha256 = $commandReportFileSha
            liveness_bracket_sha256 = Get-FileSha256 $s1BracketPath
        }
        Write-NewUtf8Json $s1SessionPath $session
    }
    Write-Host ("Physical A 已完整记录：pulse/backend/ACK=" +
        "$completedPulses/$expectedPulseCount/$expectedPulseCount；" +
        "sidecar=$($frames.Count)/$($frames.Count)；尚未分析 visible effect。")
    Write-Host "请松开右键，并把方向/可见性/异常或急停情况直接发回当前对话。"
} finally {
    if ($null -ne $sidecarProcess) {
        $sidecarProcess.Refresh()
        if (-not $sidecarProcess.HasExited) {
            Stop-Process -Id $sidecarProcess.Id -Force
        }
        $sidecarProcess.Dispose()
    }
}
