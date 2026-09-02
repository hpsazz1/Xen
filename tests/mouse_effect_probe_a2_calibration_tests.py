import csv
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile

import cv2
import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "calibrate_mouse_effect_probe_a2.py"
SPEC = importlib.util.spec_from_file_location("probe_a2_calibration", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 Physical A2 dependency calibration")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _write_json(path: pathlib.Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _shift_image(image: np.ndarray, shift_x: float) -> np.ndarray:
    matrix = np.array([[1.0, 0.0, shift_x], [0.0, 1.0, 0.0]], dtype=np.float64)
    return cv2.warpAffine(
        image,
        matrix,
        (image.shape[1], image.shape[0]),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REFLECT_101,
    )


def _make_s1_session(
    root: pathlib.Path,
    role: str,
    run_uuid: str,
    source_clock_session_id: str,
    source_start_ns: int,
    shifts: list[float],
    *,
    degenerate: bool = False,
) -> pathlib.Path:
    run = root / role
    frame_root = run / "pixel-evidence" / "frames"
    frame_root.mkdir(parents=True)
    rng = np.random.default_rng(20260901)
    base = rng.integers(0, 256, (128, 256, 3), dtype=np.uint8)
    frames: list[dict] = []
    for index, shift in enumerate(shifts):
        bgr = base.copy() if degenerate else _shift_image(base, shift)
        # 两个 capture session 不共享完整 decoded frame，但 witness 内容保持同一 scope。
        if role == "validation":
            bgr[0, 0, 0] = np.uint8((int(bgr[0, 0, 0]) + 1) % 256)
        frame_path = frame_root / f"{index:06d}.png"
        expect(cv2.imwrite(str(frame_path), bgr), "S1 fixture PNG 写入失败")
        source_time_ns = source_start_ns + index * 4_000_000
        frames.append(
            {
                "index": index,
                "file": f"frames/{index:06d}.png",
                "width": 256,
                "height": 128,
                "bgr_sha256": hashlib.sha256(bgr.tobytes()).hexdigest(),
                "source_timestamp": 1_000_000 + index + source_start_ns,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_time_timing_valid": True,
                "source_clock_status": "VALID",
                "source_clock_session_id": source_clock_session_id,
                "source_clock_uncertainty_ms": 0.05,
                "source_clock_rtt_ms": 0.10,
                "source_clock_mapping_age_ms": 1.0,
                "source_clock_sample_count": 12,
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
            }
        )
    binding_sha = hashlib.sha256(f"binding-{role}".encode()).hexdigest()
    manifest = {
        "schema_version": 1,
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_source_name": "fixture-scope",
        "requested_frame_count": len(frames),
        "recorded_frame_count": len(frames),
        "source_binding": {
            "file": "source-binding.json",
            "sha256": binding_sha,
        },
        "frames": frames,
    }
    session = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_s1_session",
        "status": "RECORDED_UNANALYZED",
        "physical_output_capability": False,
        "probe_started": False,
        "mouse_opened": False,
        "actual_command_zero": True,
        "aim_off": True,
        "run_uuid": run_uuid,
        "run_role": role,
        "scope_id": "fixture-scope",
        "capture_process_session_id": f"capture-{role}",
        "manifest_sha256": "fixture-manifest-sha",
        "obs_source_binding_sha256": binding_sha,
        "probe_binding_sha256": binding_sha,
    }
    _write_json(run / "pixel-evidence" / "manifest.json", manifest)
    _write_json(run / "s1-session.json", session)
    return run


def _make_bracketed_static_s1_session(
    root: pathlib.Path,
    role: str,
    run_uuid: str,
    source_clock_session_id: str,
    source_start_ns: int,
    *,
    post_challenge_changes: bool = True,
    peak_hold_count: int = 0,
) -> pathlib.Path:
    run = root / role
    frame_root = run / "pixel-evidence" / "frames"
    frame_root.mkdir(parents=True)
    rng = np.random.default_rng(20260902)
    base = rng.integers(0, 256, (128, 256, 3), dtype=np.uint8)
    if role == "validation":
        # 新 capture asset 不复用 primary 的完整 decoded frame；
        # witness 内容仍属于同一 scope。
        base[0, 0, 0] = np.uint8((int(base[0, 0, 0]) + 1) % 256)

    pre_count = 8 + peak_hold_count
    settle_count = 4
    baseline_count = 32
    post_count = 8 + peak_hold_count
    pre_shifts = [
        0.0, 0.4, 0.8, 0.4,
        *([0.8] * peak_hold_count),
        0.0, -0.4, 0.0, 0.0,
    ]
    settle_shifts = [0.0] * settle_count
    baseline_shifts = [0.0] * baseline_count
    post_shifts = (
        [
            0.0, -0.4, -0.8, -0.4,
            *([-0.8] * peak_hold_count),
            0.0, 0.4, 0.0, 0.0,
        ]
        if post_challenge_changes else [0.0] * post_count
    )
    shifts = pre_shifts + settle_shifts + baseline_shifts + post_shifts

    frames: list[dict] = []
    for index, shift in enumerate(shifts):
        bgr = _shift_image(base, float(shift))
        frame_path = frame_root / f"{index:06d}.png"
        expect(cv2.imwrite(str(frame_path), bgr), "bracketed S1 PNG 写入失败")
        captured_ns = source_start_ns + index * 4_000_000
        frames.append(
            {
                "index": index,
                "file": f"frames/{index:06d}.png",
                "width": 256,
                "height": 128,
                "bgr_sha256": hashlib.sha256(bgr.tobytes()).hexdigest(),
                "captured_at_steady_ns": captured_ns,
                "source_timestamp": 1_000_000 + index + source_start_ns,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": captured_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_time_timing_valid": True,
                "source_clock_status": "VALID",
                "source_clock_session_id": source_clock_session_id,
                "source_clock_uncertainty_ms": 0.05,
                "source_clock_rtt_ms": 0.10,
                "source_clock_mapping_age_ms": 1.0,
                "source_clock_sample_count": 12,
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
                "duplication_recoveries": 0,
            }
        )

    pre_last = pre_count - 1
    settle_first = pre_count
    settle_last = settle_first + settle_count - 1
    baseline_first = settle_last + 1
    baseline_last = baseline_first + baseline_count - 1
    post_first = baseline_last + 1
    post_last = len(frames) - 1
    role_sign = 1 if role == "primary" else -1
    pre_dx = (
        [role_sign, 0, role_sign, 0]
        + [0] * peak_hold_count
        + [-role_sign, 0, -role_sign, 0]
    )
    post_dx = (
        [-role_sign, 0, -role_sign, 0]
        + [0] * peak_hold_count
        + [role_sign, 0, role_sign, 0]
    )
    dx_values = (
        pre_dx + [0] * settle_count + [0] * baseline_count + post_dx
    )
    sequence_sha = hashlib.sha256(
        f"fixture-s1-sequence-{role}".encode()
    ).hexdigest()
    profile = f"dependency_calibration_a2_s1_{role}"
    samples: list[dict] = []
    events: list[dict] = []
    cumulative = 0
    for index, (frame, dx) in enumerate(zip(frames, dx_values, strict=True)):
        if index <= pre_last:
            block_id = 1
            phase = (
                "hold" if 4 <= index < 4 + peak_hold_count
                else "pulse" if dx else "response"
            )
        elif index <= settle_last:
            block_id = 0
            phase = "guard"
        elif index <= baseline_last:
            block_id = 0
            phase = "baseline"
        else:
            block_id = 2
            post_index = index - post_first
            phase = (
                "hold" if 4 <= post_index < 4 + peak_hold_count
                else "pulse" if dx else "response"
            )
        cumulative += dx
        samples.append(
            {
                "sample_index": index,
                "block_id": block_id,
                "phase": phase,
                "dx_counts": dx,
                "dy_counts": 0,
            }
        )
        dispatched = dx != 0
        events.append(
            {
                "run_uuid": run_uuid,
                "sample_index": index,
                "block_id": block_id,
                "sequence_sha256": sequence_sha,
                "source_timestamp": frame["source_timestamp"],
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": frame["source_time_at_steady_ns"],
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_clock_status": "VALID",
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
                "nominal_dx_counts": dx,
                "nominal_dy_counts": 0,
                "requested_dx_counts": dx if dispatched else 0,
                "requested_dy_counts": 0,
                "dispatch_attempted": dispatched,
                "backend_succeeded": dispatched,
                "protocol_ack_received": dispatched,
                "cumulative_requested_x_counts": cumulative,
                "cumulative_backend_completed_x_counts": cumulative,
            }
        )
    sequence = {
        "schema": 3,
        "profile": profile,
        "request": {
            "challenge_pulse_count": 2,
            "challenge_stride_sample_count": 2,
            "settle_sample_count": settle_count,
            "baseline_sample_count": baseline_count,
            "run_role": role,
        },
        "blocks": [
            {
                "block_id": 1,
                "first_sample_index": 0,
                "sample_count": pre_count,
                "first_pulse_dx_counts": 1 if role == "primary" else -1,
                "second_pulse_dx_counts": -1 if role == "primary" else 1,
            },
            {
                "block_id": 2,
                "first_sample_index": post_first,
                "sample_count": post_count,
                "first_pulse_dx_counts": -1 if role == "primary" else 1,
                "second_pulse_dx_counts": 1 if role == "primary" else -1,
            },
        ],
        "samples": samples,
        "summary": {
            "net_x_counts": 0,
            "max_abs_prefix_x_counts": 2,
        },
        "sequence_sha256": sequence_sha,
    }
    if peak_hold_count > 0:
        sequence["request"]["peak_hold_sample_count"] = peak_hold_count
    sequence_path = run / "sequence.json"
    _write_json(sequence_path, sequence)
    sequence_file_sha = hashlib.sha256(sequence_path.read_bytes()).hexdigest()
    report = {
        "evidence_type": "backend_completed_command_to_visible_background_response",
        "run_uuid": run_uuid,
        "dispatch_mode": "physical_a",
        "profile": profile,
        "sequence_sha256": sequence_sha,
        "result": {
            "state": "completed",
            "stop_reason": "normal_completion",
            "complete": True,
            "consumed_sample_count": len(samples),
            "cumulative_requested_x_counts": 0,
            "cumulative_backend_completed_x_counts": 0,
            "events": events,
        },
    }
    report_path = run / "command-report.json"
    _write_json(report_path, report)
    report_sha = hashlib.sha256(report_path.read_bytes()).hexdigest()
    bracket = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_a2_s1_liveness_bracket",
        "physical_output_capability": True,
        "automated_input_generated": True,
        "input_backend": "kmbox_net",
        "manual_motion_required": False,
        "phase_join_basis": "command_event_source_timestamp_to_manifest",
        "sequence_sha256": sequence_sha,
        "sequence_file_sha256": sequence_file_sha,
        "command_report_sha256": report_sha,
        "policy": {
            "policy_id": "fixture-kmbox-bracket-policy-v1",
            "baseline_frame_count": baseline_count,
            "challenge_pulse_count": 2,
            "challenge_stride_sample_count": 2,
            "challenge_frames_eligible_for_estimands": False,
            "settle_frames_eligible_for_estimands": False,
        },
        "phases": [
            {
                "name": "PRE_LIVENESS_CHALLENGE",
                "first_sample_index": 0,
                "last_sample_index": pre_last,
            },
            {
                "name": "RELEASE_AND_SETTLE",
                "first_sample_index": settle_first,
                "last_sample_index": settle_last,
            },
            {
                "name": "BASELINE_ZERO",
                "first_sample_index": baseline_first,
                "last_sample_index": baseline_last,
            },
            {
                "name": "POST_LIVENESS_CHALLENGE",
                "first_sample_index": post_first,
                "last_sample_index": post_last,
            },
        ],
    }
    if peak_hold_count > 0:
        bracket["policy"]["peak_hold_sample_count"] = peak_hold_count
        bracket["policy"][
            "peak_hold_frames_eligible_for_estimands"
        ] = False
    bracket_path = run / "s1-liveness-bracket.json"
    _write_json(bracket_path, bracket)
    bracket_sha = hashlib.sha256(bracket_path.read_bytes()).hexdigest()
    binding_sha = hashlib.sha256(b"binding-bracketed-scope").hexdigest()
    manifest = {
        "schema_version": 1,
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_source_name": "fixture-scope",
        "requested_frame_count": len(frames),
        "recorded_frame_count": len(frames),
        "source_binding": {
            "file": "source-binding.json",
            "sha256": binding_sha,
        },
        "frames": frames,
    }
    manifest_path = run / "pixel-evidence" / "manifest.json"
    _write_json(manifest_path, manifest)
    session = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_a2_s1_session",
        "status": "RECORDED_UNANALYZED",
        "capture_mode": "bracketed_kmbox",
        "physical_output_capability": True,
        "probe_started": True,
        "mouse_opened": True,
        "actual_command_zero": False,
        "probe_command_zero": False,
        "baseline_actual_command_zero": True,
        "automated_kmbox_challenge": True,
        "challenge_frames_excluded_from_estimands": True,
        "aim_off": True,
        "run_uuid": run_uuid,
        "run_role": role,
        "scope_id": "fixture-bracketed-scope",
        "capture_process_session_id": f"capture-bracketed-{role}",
        "manifest_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        "obs_source_binding_sha256": binding_sha,
        "probe_binding_sha256": binding_sha,
        "sequence_sha256": sequence_sha,
        "sequence_file_sha256": sequence_file_sha,
        "command_report_sha256": report_sha,
        "liveness_bracket_sha256": bracket_sha,
    }
    _write_json(run / "s1-session.json", session)
    return run


def test_s0_uses_untouched_holdout_and_calibrates_spatial_operations() -> None:
    result = MODULE.build_synthetic_calibration()
    expect(result["status"] == "VALID", "S0 合成校准必须通过预注册留出")
    expect(result["physical_output_capability"] is False,
           "S0 产物不得具备物理输出能力")
    expect(result["holdout_contract"]["random_case_split_allowed"] is False,
           "S0 必须按完整 texture/shift case 留出")
    expect(result["holdout_contract"]["holdout_used_for_interval"] is False,
           "untouched holdout 不得参与算法误差区间拟合")
    interval = result["spatial_error_interval_px"]
    expect(np.isfinite(interval["lower"]) and np.isfinite(interval["upper"])
           and interval["lower"] <= interval["upper"],
           "S0 必须给有限有序的 signed spatial error interval")
    expect(result["tail_detection"]["all_injected_tails_detected"] is True,
           "S0 必须能检出预注册的有限 injected tail")
    expect(result["mask_margin"]["internal_invalid_region_detected"] is True,
           "S0 必须覆盖内部 invalid/occluder，而不只检查图像边缘")


def test_s1_requires_two_nonoverlapping_nondegenerate_capture_sessions() -> None:
    shifts = [
        0.00, 0.04, -0.02, 0.03, -0.01, 0.05, -0.03, 0.02,
        0.00, -0.04, 0.01, -0.02, 0.03, -0.05, 0.02, 0.00,
    ] * 4
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "clock-primary",
            10_000_000_000,
            shifts,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "clock-validation",
            20_000_000_000,
            shifts,
        )
        result, rows = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "VALID" and len(rows) == 128,
               "两个独立 S1 session 的非退化零输入块必须建立有效 noise 证据")
        expect(result["physical_output_capability"] is False
               and result["probe_started"] is False
               and result["mouse_opened"] is False,
               "S1 必须证明无 probe、无 Mouse、无物理输出")
        audit = result["operational_independence"]
        expect(audit["distinct_run_uuid"] is True
               and audit["distinct_capture_process_session"] is True
               and audit["distinct_source_clock_session"] is True
               and audit["source_time_ranges_overlap"] is False
               and audit["decoded_frame_hash_overlap_count"] == 0,
               "S1 primary/validation 不得共享 session、时间窗或 decoded frame")
        expect(result["noise_model"]["iid_claimed"] is False,
               "有限 S1 数据不得声称 i.i.d.")
        expect(result["noise_model"]["left"]["nondegenerate"] is True
               and result["noise_model"]["right"]["nondegenerate"] is True,
               "左右 witness 的去 nuisance residual 都必须非退化")
        plan = MODULE.derive_dependency_calibration_plan(
            MODULE.build_synthetic_calibration(),
            result,
            observed_lag_reference=4,
            candidate_horizons=(4, 8, 16, 32),
        )
        expect(plan["status"] == "VALID_OFFLINE_PLAN"
               and plan["physical_output_capability"] is False
               and plan["physical_launch_authorized"] is False,
               "S0/S1 green 只能生成离线 A2 plan，不得授权 Launch")
        request = plan["sequence_request"]
        expect(request["block_count"] == 4
               and request["guard_sample_count"] > 32
               and request["response_sample_count"] >= 32
               and request["sample_count"] < 2400
               and request["max_abs_prefix_x_counts"] == 1,
               "A2 repetition/guard 必须由覆盖目标与 S1 cadence 派生且仍在短证据容量内")
        expect(plan["roles"]["p_cal"]["profile"]
               != plan["roles"]["p_holdout"]["profile"],
               "P-CAL/P-HOLDOUT 必须预注册不同 profile/方向角色")
        expect(plan["derivation"]["reused_physical_a_guard_or_gain"] is False
               and plan["derivation"]["fixed_speed_used"] is False,
               "A2 plan 不得复用 A1 guard/gain 或固定速度换算")


def test_s1_rejects_reused_or_degenerate_frames_without_epsilon() -> None:
    shifts = [0.0] * 32
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-red-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "clock-primary",
            10_000_000_000,
            shifts,
            degenerate=True,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "clock-validation",
            20_000_000_000,
            shifts,
            degenerate=True,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "INVALID"
               and "NONDEGENERATE_NOISE_MISSING" in result["invalid_reasons"],
               "重复 PNG/全零 residual 必须保持 red，不得填 epsilon")
        expect(result["noise_model"]["artificial_epsilon_added"] is False,
               "S1 绝不允许伪造正宽 noise interval")


def test_s1_allows_separate_timing_observations_of_the_same_clock_epoch() -> None:
    shifts = [
        0.00, 0.04, -0.02, 0.03, -0.01, 0.05, -0.03, 0.02,
        0.00, -0.04, 0.01, -0.02, 0.03, -0.05, 0.02, 0.00,
    ] * 4
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-clock-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "shared-clock-epoch",
            10_000_000_000,
            shifts,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "shared-clock-epoch",
            20_000_000_000,
            shifts,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "VALID",
               "不同 capture/timing observation 不应被稳定 clock epoch 误判为复用")
        audit = result["operational_independence"]
        expect(audit["distinct_source_clock_session"] is False
               and audit["distinct_timing_observation_session"] is True,
               "报告必须分开记录 clock epoch 与本次 timing observation 身份")


def test_s1_accepts_bracketed_resolution_censored_static_baseline() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-bracketed-") as directory:
        root = pathlib.Path(directory)
        primary = _make_bracketed_static_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "shared-clock-epoch",
            10_000_000_000,
            peak_hold_count=2,
        )
        validation = _make_bracketed_static_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "shared-clock-epoch",
            20_000_000_000,
            peak_hold_count=2,
        )
        result, rows = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "VALID"
               and result["measurement_state"]
               == "VALID_BRACKETED_CENSORED_ZERO",
               "活性括号与 Type B null interval 应允许数字静态 baseline")
        expect(len(rows) == 64
               and all(row["phase"] == "BASELINE_ZERO" for row in rows),
               "challenge/settle frame 必须全部排除，只输出 baseline rows")
        bracket = result["liveness_bracket"]
        expect(bracket["policy_frozen_across_runs"] is True
               and bracket["challenge_frames_excluded_from_estimands"] is True
               and bracket["automated_kmbox_challenge"] is True
               and bracket["fixed_pixel_speed_used_as_gate"] is False
               and bracket["primary"]["pre_command_ledger_pass"] is True
               and bracket["primary"]["post_command_ledger_pass"] is True
               and bracket["primary"]["pre_image_change_pass"] is True
               and bracket["primary"]["post_image_change_pass"] is True
               and bracket["validation"]["pre_command_ledger_pass"] is True
               and bracket["validation"]["post_command_ledger_pass"] is True
               and bracket["validation"]["pre_image_change_pass"] is True
               and bracket["validation"]["post_image_change_pass"] is True,
               "primary/validation 都必须有独立 pre/post decoded-image 活性证据")
        for witness in ("left", "right"):
            model = result["noise_model"][witness]
            expect(model["nondegenerate"] is False
                   and model["variability_resolved"] is False
                   and model["observed_interval_px"] == [0.0, 0.0]
                   and model["noise_distribution_claimed"] is False
                   and model["artificial_epsilon_added"] is False,
                   "静态 baseline 只能报告 resolution-censored zero")
            null_interval = model["null_displacement_interval_px"]
            expect(np.isfinite(null_interval[0])
                   and np.isfinite(null_interval[1])
                   and null_interval[0] < 0.0 < null_interval[1],
                   "Type B null interval 必须来自非人为的近零 shift 校准")
        plan = MODULE.derive_dependency_calibration_plan(
            MODULE.build_synthetic_calibration(),
            result,
            observed_lag_reference=4,
            candidate_horizons=(4, 8, 16, 32),
        )
        state = plan["dependency_state_after_plan"]
        expect(state["zero_command_disturbance_bound"]
               == "GREEN_S1_BRACKETED_CENSORED_ZERO"
               and state["independent_nondegenerate_noise"]
               == "NOT_CLAIMED_STATIC_DIGITAL_SCOPE",
               "plan 必须使用 null interval，不得偷称 nondegenerate noise")


def test_s1_legacy_bracket_without_peak_hold_remains_readable() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-legacy-") as directory:
        root = pathlib.Path(directory)
        primary = _make_bracketed_static_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "shared-clock-epoch",
            10_000_000_000,
        )
        validation = _make_bracketed_static_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "shared-clock-epoch",
            20_000_000_000,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        legacy_sequence = json.loads(
            (primary / "sequence.json").read_text(encoding="utf-8")
        )
        legacy_bracket = json.loads(
            (primary / "s1-liveness-bracket.json").read_text(
                encoding="utf-8"
            )
        )
        expect(
            result["status"] == "VALID"
            and "peak_hold_sample_count"
            not in legacy_sequence["request"]
            and "peak_hold_sample_count" not in legacy_bracket["policy"],
            "缺少 peak hold 字段的历史 schema 3/bracket policy 必须继续可读",
        )


def test_s1_bracket_rejects_missing_post_change_and_baseline_command() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-bracket-red-") as directory:
        root = pathlib.Path(directory)
        primary = _make_bracketed_static_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "shared-clock-epoch",
            10_000_000_000,
            post_challenge_changes=False,
            peak_hold_count=2,
        )
        validation = _make_bracketed_static_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "shared-clock-epoch",
            20_000_000_000,
            peak_hold_count=2,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "INVALID"
               and "PRIMARY_POST_LIVENESS_CHALLENGE_MISSING"
               in result["invalid_reasons"]
               and result["liveness_bracket"]["primary"]
               ["post_command_ledger_pass"] is True,
               "固定 command/ACK 不能替代 post decoded-image change")

        report_path = validation / "command-report.json"
        report = json.loads(report_path.read_text(encoding="utf-8"))
        baseline_event = report["result"]["events"][14]
        baseline_event["nominal_dx_counts"] = 1
        baseline_event["requested_dx_counts"] = 1
        baseline_event["dispatch_attempted"] = True
        baseline_event["backend_succeeded"] = True
        baseline_event["protocol_ack_received"] = True
        _write_json(report_path, report)
        report_sha = hashlib.sha256(report_path.read_bytes()).hexdigest()
        bracket_path = validation / "s1-liveness-bracket.json"
        bracket = json.loads(bracket_path.read_text(encoding="utf-8"))
        bracket["command_report_sha256"] = report_sha
        _write_json(bracket_path, bracket)
        session_path = validation / "s1-session.json"
        session = json.loads(session_path.read_text(encoding="utf-8"))
        session["command_report_sha256"] = report_sha
        session["liveness_bracket_sha256"] = hashlib.sha256(
            bracket_path.read_bytes()
        ).hexdigest()
        _write_json(session_path, session)
        try:
            MODULE.analyze_zero_input_sessions(
                primary,
                validation,
                (16, 16, 80, 96),
                (160, 16, 80, 96),
                block_count=4,
            )
            raise AssertionError("baseline 非零 command 必须 fail closed")
        except ValueError as exception:
            expect("command ledger" in str(exception),
                   "重哈希也不得掩盖 sequence/baseline command 不守恒")


def test_p_cal_candidate_freezes_tail_mapping_margin_and_gain_before_holdout() -> None:
    scope_id = "a2-candidate-scope"
    run_uuid = "11111111-2222-4333-8444-555555555555"
    sequence_sha256 = "a" * 64
    synthetic = {
        "evidence_type": "mouse_effect_probe_a2_s0_synthetic_calibration",
        "status": "VALID",
        "physical_output_capability": False,
        "spatial_error_interval_px": {"lower": -0.1, "upper": 0.1},
    }
    zero_input = {
        "evidence_type": "mouse_effect_probe_a2_s1_zero_input_calibration",
        "status": "VALID",
        "physical_output_capability": False,
        "scope_id": scope_id,
        "noise_model": {
            "left": {"null_displacement_interval_px": [-0.05, 0.05]},
            "right": {"null_displacement_interval_px": [-0.05, 0.05]},
        },
    }
    plan = {
        "evidence_type": "mouse_effect_probe_a2_dependency_calibration_plan",
        "status": "VALID_OFFLINE_PLAN",
        "physical_output_capability": False,
        "physical_launch_authorized": False,
        "scope_id": scope_id,
        "sequence_request": {
            "response_sample_count": 5,
            "guard_sample_count": 8,
            "block_count": 4,
            "nonzero_transition_count": 8,
            "max_abs_prefix_x_counts": 1,
            "dy_counts_required": 0,
        },
        "derivation": {
            "observed_lag_reference": 2,
            "maximum_candidate_horizon": 4,
            "source_mapping_ambiguity_frames": 1,
            "fixed_speed_used": False,
        },
        "roles": {
            "p_holdout": {
                "profile": "dependency_calibration_a2_p_holdout",
                "direction_order": [-1, 1, 1, -1],
                "uses_holdout_for_tuning": False,
            }
        },
    }
    task = {
        "evidence_type": "mouse_effect_probe_a2_task",
        "status": "PREPARED",
        "run_role": "p-cal",
        "profile": "dependency_calibration_a2_p_cal",
        "scope_id": scope_id,
        "run_uuid": run_uuid,
        "sequence_sha256": sequence_sha256,
        "expected_nonzero_transition_count": 8,
    }
    pulse_responses = []
    rows = []
    for pulse_index in range(8):
        command = 1 if pulse_index % 2 == 0 else -1
        shifted = -0.5 * command
        reference_state = f"reference-{pulse_index}"
        shifted_state = f"shifted-{pulse_index}"
        pulse_responses.append({
            "pulse_index": pulse_index,
            "block_id": pulse_index // 2 + 1,
            "command_dx_counts": command,
            "direction_contract_matches": True,
            "joint_exact_change_observed": True,
            "onset": {
                "last_joint_unchanged_frame_lag": 1,
                "first_changed_frame_lag": 2,
            },
        })
        for lag in range(1, 6):
            changed = lag >= 2
            rows.append({
                "pulse_index": pulse_index,
                "block_id": pulse_index // 2 + 1,
                "command_dx_counts": command,
                "frame_lag": lag,
                "phase": "response",
                "left_exact_changed": changed,
                "right_exact_changed": changed,
                "left_state_sha256": shifted_state if changed else reference_state,
                "right_state_sha256": shifted_state if changed else reference_state,
                "left_dx_px": shifted if changed else 0.0,
                "right_dx_px": shifted if changed else 0.0,
            })
    physical = {
        "evidence_type": "mouse_effect_probe_a2_physical_background_response",
        "status": "VALID",
        "invalid_reasons": [],
        "physical_output_capability": False,
        "visible_effect_analyzed": True,
        "machine_visible_effect_observed": True,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "profile": "dependency_calibration_a2_p_cal",
        "run_role": "p-cal",
        "a2_dependency_gate_claimed": False,
        "run_binding": {
            "run_uuid": run_uuid,
            "activation_epoch": 100,
            "sequence_sha256": sequence_sha256,
            "sidecar_manifest_file_sha256": "1" * 64,
            "analyzer_file_sha256": "2" * 64,
            "capture_source_name": "fixture-source",
        },
        "geometry": {
            "image_width": 256,
            "image_height": 128,
            "left_roi": {"x": 16, "y": 16, "width": 80, "height": 96},
            "right_roi": {"x": 160, "y": 16, "width": 80, "height": 96},
        },
        "backend_completed_pulse_count": 8,
        "pulse_responses": pulse_responses,
        "paired_closure": [
            {"block_id": index + 1, "exact_witness_return": True}
            for index in range(4)
        ],
        "whole_sequence_closure": {
            "net_requested_x_counts": 0,
            "net_backend_completed_x_counts": 0,
            "exact_witness_return": True,
        },
    }
    observation = """# Physical A2 人工观察

- 左右 witness 一致性：一致
- 遮挡/scene cut：不存在
- 异常/急停：无异常或急停
- 人工结论：看到轻微视角偏移；没移动鼠标/WASD
"""
    candidate = MODULE.build_physical_candidate(
        synthetic,
        zero_input,
        plan,
        task,
        physical,
        rows,
        observation_text=observation,
        observation_sha256="b" * 64,
    )
    expect(candidate["status"] == "VALID_P_CAL_CANDIDATE",
           "完整 P-CAL 必须冻结供独立 P-HOLDOUT 使用的 candidate")
    expect(candidate["a2_dependency_gate_claimed"] is False
           and candidate["holdout_required"] is True,
           "P-CAL candidate 不能冒充 A2 dependency green")
    expect(candidate["tail_support"]["tail_upper_observed_lag"] == 3
           and candidate["tail_support"]["tail_censored"] is False,
           "tail candidate 必须由完整 response window 的最后 exact state change 派生")
    expect(candidate["mapping_uncertainty"]["upper_px"] == 0.65
           and candidate["mapping_uncertainty"]["fixed_speed_used"] is False,
           "mapping 必须用 source-frame hull，不得把时间乘固定速度")
    expect(candidate["single_count_gain_upper_scope"]["candidate_upper_px"] == 0.65,
           "gain candidate 必须包含 spatial 与 zero-input interval 的保守上端")
    expect(candidate["witness_occlusion_margin"]["usable_margin_lower_px"] == 15.35
           and candidate["physical_b_prefix_candidate"]["allowed_prefix_counts"] == 22,
           "几何额度必须从完整 support、mapping 与 gain candidate 计算")

    candidate_sha256 = "c" * 64
    input_sha256 = {
        "synthetic_calibration": "d" * 64,
        "zero_input_calibration": "e" * 64,
        "calibration_plan": "f" * 64,
        "calibration_response": "1" * 64,
    }
    candidate["input_files"] = {
        "synthetic_calibration": {
            "sha256": input_sha256["synthetic_calibration"]
        },
        "zero_input_calibration": {
            "sha256": input_sha256["zero_input_calibration"]
        },
        "calibration_plan": {
            "sha256": input_sha256["calibration_plan"]
        },
        "physical_response": {
            "sha256": input_sha256["calibration_response"]
        },
    }
    holdout_uuid = "66666666-7777-4888-8999-aaaaaaaaaaaa"
    holdout_sequence_sha256 = "3" * 64
    holdout_task = json.loads(json.dumps(task))
    holdout_task.update({
        "run_role": "p-holdout",
        "profile": "dependency_calibration_a2_p_holdout",
        "run_uuid": holdout_uuid,
        "sequence_sha256": holdout_sequence_sha256,
        "calibration": {"p_cal_candidate_sha256": candidate_sha256},
    })
    holdout_observation = f"""# Physical A2 人工观察

- Run UUID：`{holdout_uuid}`
- Run role：`p-holdout`
- 左右 witness 一致性：一致
- 遮挡/scene cut：不存在
- 异常/急停：无异常或急停
- 人工结论：看到轻微视角偏移；没移动鼠标/WASD
"""
    holdout_physical = json.loads(json.dumps(physical))
    holdout_physical.update({
        "run_role": "p-holdout",
        "profile": "dependency_calibration_a2_p_holdout",
    })
    holdout_physical["run_binding"].update({
        "run_uuid": holdout_uuid,
        "activation_epoch": 200,
        "sequence_sha256": holdout_sequence_sha256,
        "sidecar_manifest_file_sha256": "4" * 64,
    })
    holdout_rows = json.loads(json.dumps(rows))
    holdout_first_directions = (-1, 1, 1, -1)
    for pulse_index, response in enumerate(holdout_physical["pulse_responses"]):
        block_index = pulse_index // 2
        command = (
            holdout_first_directions[block_index]
            if pulse_index % 2 == 0
            else -holdout_first_directions[block_index]
        )
        response["command_dx_counts"] = command
        for row in holdout_rows:
            if int(row["pulse_index"]) != pulse_index:
                continue
            row["command_dx_counts"] = command
            if int(row["frame_lag"]) >= 2:
                row["left_dx_px"] = -0.45 * command
                row["right_dx_px"] = -0.45 * command
    decision = MODULE.evaluate_physical_holdout(
        candidate,
        synthetic,
        zero_input,
        plan,
        physical,
        holdout_task,
        holdout_physical,
        holdout_rows,
        observation_text=holdout_observation,
        observation_sha256="5" * 64,
        candidate_sha256=candidate_sha256,
        input_sha256=input_sha256,
    )
    expect(decision["status"] == "A2_DEPENDENCY_GREEN"
           and decision["a2_dependency_gate_claimed"] is True,
           "独立 P-HOLDOUT 全部不越冻结 candidate 时才能形成 A2 dependency green")
    expect(decision["candidate_values_changed"] is False
           and decision["holdout_used_for_tuning"] is False
           and decision["physical_b_authorized"] is False,
           "holdout green 不得回调 candidate 或自动授权 Physical B")

    try:
        MODULE.evaluate_physical_holdout(
            candidate,
            synthetic,
            zero_input,
            plan,
            physical,
            holdout_task,
            holdout_physical,
            holdout_rows,
            observation_text=observation,
            observation_sha256="5" * 64,
            candidate_sha256=candidate_sha256,
            input_sha256=input_sha256,
        )
    except ValueError as exception:
        expect("Observation" in str(exception),
               "错 Run observation 必须以人工证据绑定原因拒绝")
    else:
        raise AssertionError("未绑定 P-HOLDOUT Run 身份的 observation 不得进入裁决")

    missing_source_calibration = json.loads(json.dumps(physical))
    missing_source_holdout = json.loads(json.dumps(holdout_physical))
    missing_source_calibration["run_binding"].pop("capture_source_name")
    missing_source_holdout["run_binding"].pop("capture_source_name")
    try:
        MODULE.evaluate_physical_holdout(
            candidate,
            synthetic,
            zero_input,
            plan,
            missing_source_calibration,
            holdout_task,
            missing_source_holdout,
            holdout_rows,
            observation_text=holdout_observation,
            observation_sha256="5" * 64,
            candidate_sha256=candidate_sha256,
            input_sha256=input_sha256,
        )
    except ValueError as exception:
        expect("analyzer/source" in str(exception),
               "缺失 capture source 身份必须以独立证据原因拒绝")
    else:
        raise AssertionError("空 capture source 不得冒充同源独立证据")

    exceeded_rows = json.loads(json.dumps(holdout_rows))
    for row in exceeded_rows:
        if int(row["frame_lag"]) >= 2:
            command = int(row["command_dx_counts"])
            row["left_dx_px"] = -0.8 * command
            row["right_dx_px"] = -0.8 * command
    red_decision = MODULE.evaluate_physical_holdout(
        candidate,
        synthetic,
        zero_input,
        plan,
        physical,
        holdout_task,
        holdout_physical,
        exceeded_rows,
        observation_text=holdout_observation,
        observation_sha256="5" * 64,
        candidate_sha256=candidate_sha256,
        input_sha256=input_sha256,
    )
    expect(red_decision["status"] == "A2_DEPENDENCY_RED"
           and red_decision["a2_dependency_gate_claimed"] is False
           and "SINGLE_COUNT_GAIN_UPPER_SCOPE_EXCEEDED"
           in red_decision["invalid_reasons"],
           "held-out gain 超过冻结 candidate 时必须持久化 A2 red")
    expect(red_decision["candidate_values_changed"] is False
           and red_decision["holdout_used_for_tuning"] is False,
           "holdout red 也不得扩大 candidate 或并回 P-CAL")

    with tempfile.TemporaryDirectory(prefix="xen-a2-p-cal-candidate-cli-") as directory:
        root = pathlib.Path(directory)
        inputs = {
            "synthetic-calibration.json": synthetic,
            "zero-input-calibration.json": zero_input,
            "plan.json": plan,
            "task.json": task,
            "physical-response.json": physical,
        }
        for name, value in inputs.items():
            _write_json(root / name, value)
        rows_path = root / "physical-response.csv"
        with rows_path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        observation_path = root / "OBSERVATION.md"
        observation_path.write_text(observation, encoding="utf-8")
        output_path = root / "candidate.json"
        exit_code = MODULE.main([
            "candidate",
            "--synthetic-calibration", str(root / "synthetic-calibration.json"),
            "--zero-input-calibration", str(root / "zero-input-calibration.json"),
            "--calibration-plan", str(root / "plan.json"),
            "--task", str(root / "task.json"),
            "--physical-response", str(root / "physical-response.json"),
            "--physical-rows-csv", str(rows_path),
            "--observation", str(observation_path),
            "--output", str(output_path),
        ])
        emitted = json.loads(output_path.read_text(encoding="utf-8"))
        expect(exit_code == 0
               and emitted["status"] == "VALID_P_CAL_CANDIDATE"
               and emitted["input_files"]["calibration_plan"]["sha256"]
               == hashlib.sha256((root / "plan.json").read_bytes()).hexdigest(),
               "candidate CLI 必须原子发布并绑定全部输入身份")

        emitted_candidate_sha256 = hashlib.sha256(
            output_path.read_bytes()
        ).hexdigest()
        holdout_task["calibration"]["p_cal_candidate_sha256"] = (
            emitted_candidate_sha256
        )
        _write_json(root / "holdout-task.json", holdout_task)
        _write_json(root / "holdout-response.json", holdout_physical)
        holdout_observation_path = root / "HOLDOUT-OBSERVATION.md"
        holdout_observation_path.write_text(
            holdout_observation, encoding="utf-8"
        )
        holdout_rows_path = root / "holdout-response.csv"
        with holdout_rows_path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(
                output, fieldnames=list(holdout_rows[0].keys())
            )
            writer.writeheader()
            writer.writerows(holdout_rows)
        decision_path = root / "holdout-decision.json"
        holdout_exit_code = MODULE.main([
            "holdout",
            "--candidate", str(output_path),
            "--synthetic-calibration", str(root / "synthetic-calibration.json"),
            "--zero-input-calibration", str(root / "zero-input-calibration.json"),
            "--calibration-plan", str(root / "plan.json"),
            "--calibration-response", str(root / "physical-response.json"),
            "--holdout-task", str(root / "holdout-task.json"),
            "--holdout-response", str(root / "holdout-response.json"),
            "--holdout-rows-csv", str(holdout_rows_path),
            "--observation", str(holdout_observation_path),
            "--output", str(decision_path),
        ])
        emitted_decision = json.loads(
            decision_path.read_text(encoding="utf-8")
        )
        expect(holdout_exit_code == 0
               and emitted_decision["status"] == "A2_DEPENDENCY_GREEN"
               and emitted_decision["input_files"]["candidate"]["sha256"]
               == emitted_candidate_sha256,
               "holdout CLI 必须原子发布并绑定 candidate 与全部输入身份")

    censored_rows = json.loads(json.dumps(rows))
    for row in censored_rows:
        if int(row["pulse_index"]) == 0 and int(row["frame_lag"]) >= 4:
            row["left_state_sha256"] = "late-left-state"
            row["right_state_sha256"] = "late-right-state"
    try:
        MODULE.build_physical_candidate(
            synthetic,
            zero_input,
            plan,
            task,
            physical,
            censored_rows,
            observation_text=observation,
            observation_sha256="b" * 64,
        )
    except ValueError as exception:
        expect("tail" in str(exception).lower(),
               "到达 candidate horizon 的变化必须以 tail-censored 原因拒绝")
    else:
        raise AssertionError("tail 到达 horizon 时不得生成 P-CAL candidate")


if __name__ == "__main__":
    test_s0_uses_untouched_holdout_and_calibrates_spatial_operations()
    test_s1_requires_two_nonoverlapping_nondegenerate_capture_sessions()
    test_s1_rejects_reused_or_degenerate_frames_without_epsilon()
    test_s1_allows_separate_timing_observations_of_the_same_clock_epoch()
    test_s1_accepts_bracketed_resolution_censored_static_baseline()
    test_s1_legacy_bracket_without_peak_hold_remains_readable()
    test_s1_bracket_rejects_missing_post_change_and_baseline_command()
    test_p_cal_candidate_freezes_tail_mapping_margin_and_gain_before_holdout()
    print("Mouse Effect Probe A2 dependency calibration 测试全部通过。")
