#!/usr/bin/env python3
"""Physical A2 的 S0 合成校准与 S1 零输入分块校准。

本入口只读取/生成像素证据，不打开 Capture、Probe 或 Mouse。真实 Physical
tail、遮挡轨迹、source-frame 像素包络和 scope gain upper 不在这里推断。
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import pathlib
import sys
from collections.abc import Iterable, Sequence

import cv2
import numpy as np


Roi = tuple[int, int, int, int]


_BRACKET_PHASE_NAMES = (
    "PRE_LIVENESS_CHALLENGE",
    "RELEASE_AND_SETTLE",
    "BASELINE_ZERO",
    "POST_LIVENESS_CHALLENGE",
)
_BRACKET_PRIMARY_SHIFTS_PX = (
    -1.5,
    -1.0,
    -0.75,
    -0.5,
    -0.25,
    0.0,
    0.25,
    0.5,
    0.75,
    1.0,
    1.5,
)
_BRACKET_VALIDATION_SHIFTS_PX = (
    -1.25,
    -0.625,
    -0.375,
    -0.125,
    0.125,
    0.375,
    0.625,
    1.25,
)


def _distribution(values: Iterable[float]) -> dict:
    data = np.asarray(list(values), dtype=np.float64)
    if data.size == 0 or not np.all(np.isfinite(data)):
        raise ValueError("分布必须包含有限样本")
    ordered = np.sort(data)

    def percentile(fraction: float) -> float:
        if ordered.size == 1:
            return float(ordered[0])
        position = fraction * float(ordered.size - 1)
        lower = int(math.floor(position))
        upper = int(math.ceil(position))
        weight = position - lower
        return float(ordered[lower] * (1.0 - weight) + ordered[upper] * weight)

    return {
        "count": int(ordered.size),
        "mean": float(np.mean(ordered)),
        "min": float(ordered[0]),
        "p50": percentile(0.50),
        "p95": percentile(0.95),
        "p99": percentile(0.99),
        "max": float(ordered[-1]),
    }


def _array_sha256(value: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(value).tobytes()).hexdigest()


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_roi(roi: Roi, width: int, height: int, name: str) -> None:
    x, y, roi_width, roi_height = roi
    if (
        x < 0
        or y < 0
        or roi_width <= 1
        or roi_height <= 1
        or x + roi_width > width
        or y + roi_height > height
    ):
        raise ValueError(f"{name} 超出图像或尺寸不足")


def _roi(gray: np.ndarray, roi: Roi) -> np.ndarray:
    x, y, width, height = roi
    return np.ascontiguousarray(gray[y : y + height, x : x + width])


def _measure_translation(
    previous: np.ndarray, current: np.ndarray, window: np.ndarray
) -> dict:
    # OpenCV 在无需 DFT padding 时可能原地处理输入；每次调用使用独立副本。
    shift, response = cv2.phaseCorrelate(
        previous.copy(), current.copy(), window.copy()
    )
    result = {
        "dx_px": float(shift[0]),
        "dy_px": float(shift[1]),
        "response": float(response),
    }
    if not all(math.isfinite(value) for value in result.values()):
        raise ValueError("phaseCorrelate 返回非有限值")
    return result


def _synthetic_texture(seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    image = rng.integers(0, 256, (320, 320), dtype=np.uint8)
    cv2.rectangle(image, (12, 20), (112, 294), 32 + seed % 128, 2)
    cv2.circle(image, (245, 155), 61, 200 - seed % 80, 3)
    cv2.line(image, (0, 60 + seed % 40), (319, 260), 240, 2)
    return image.astype(np.float32)


def _warp_horizontal(image: np.ndarray, shift: float) -> np.ndarray:
    transform = np.array(
        [[1.0, 0.0, shift], [0.0, 1.0, 0.0]], dtype=np.float64
    )
    return cv2.warpAffine(
        image,
        transform,
        (image.shape[1], image.shape[0]),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REFLECT_101,
    )


def _synthetic_shift_cases(
    seeds: Sequence[int], role: str, left_roi: Roi, right_roi: Roi
) -> list[dict]:
    shifts = (-1.5, -1.0, -0.5, -0.25, 0.25, 0.5, 1.0, 1.5)
    rows: list[dict] = []
    for seed in seeds:
        texture = _synthetic_texture(seed)
        texture_sha = _array_sha256(texture)
        reference_left = _roi(texture, left_roi)
        reference_right = _roi(texture, right_roi)
        left_window = cv2.createHanningWindow(
            (left_roi[2], left_roi[3]), cv2.CV_32F
        )
        right_window = cv2.createHanningWindow(
            (right_roi[2], right_roi[3]), cv2.CV_32F
        )
        for shift in shifts:
            moved = _warp_horizontal(texture, shift)
            for witness, reference, current, window in (
                ("left", reference_left, _roi(moved, left_roi), left_window),
                ("right", reference_right, _roi(moved, right_roi), right_window),
            ):
                measured = _measure_translation(reference, current, window)
                rows.append(
                    {
                        "role": role,
                        "asset_seed": seed,
                        "asset_sha256": texture_sha,
                        "truth_shift_px": shift,
                        "witness": witness,
                        "estimated_shift_px": measured["dx_px"],
                        "signed_error_px": measured["dx_px"] - shift,
                        "phase_response": measured["response"],
                    }
                )
    return rows


def _horizontal_containment_margin(mask: np.ndarray, roi: Roi) -> int:
    x, y, width, height = roi
    maximum = max(mask.shape[1], 1)
    margin = 0
    for candidate in range(maximum + 1):
        valid = True
        for delta in (-candidate, candidate):
            left = x + delta
            right = left + width
            if (
                left < 0
                or right > mask.shape[1]
                or not np.all(mask[y : y + height, left:right])
            ):
                valid = False
                break
        if not valid:
            return max(0, candidate - 1)
        margin = candidate
    return margin


def build_synthetic_calibration() -> dict:
    left_roi: Roi = (16, 48, 96, 224)
    right_roi: Roi = (208, 48, 96, 224)
    estimation = _synthetic_shift_cases(
        tuple(range(20260901, 20260909)), "estimation", left_roi, right_roi
    )
    holdout = _synthetic_shift_cases(
        (20261001, 20261002), "untouched_holdout", left_roi, right_roi
    )
    estimation_errors = [float(row["signed_error_px"]) for row in estimation]
    lower = min(estimation_errors)
    upper = max(estimation_errors)
    holdout_exceedances = [
        row
        for row in holdout
        if float(row["signed_error_px"]) < lower
        or float(row["signed_error_px"]) > upper
    ]

    tail_truth = (0.0, 0.0, -0.14, -0.29, -0.41, -0.50, -0.50, -0.50)
    texture = _synthetic_texture(20261101)
    window = cv2.createHanningWindow(
        (left_roi[2], left_roi[3]), cv2.CV_32F
    )
    observed: list[float] = []
    previous = _roi(_warp_horizontal(texture, tail_truth[0]), left_roi)
    for shift in tail_truth[1:]:
        current = _roi(_warp_horizontal(texture, shift), left_roi)
        observed.append(_measure_translation(previous, current, window)["dx_px"])
        previous = current
    truth_increments = np.diff(np.asarray(tail_truth, dtype=np.float64))
    tail_errors = np.asarray(observed) - truth_increments
    tail_detected = bool(
        np.all(tail_errors >= lower)
        and np.all(tail_errors <= upper)
        and abs(observed[-1]) <= max(abs(lower), abs(upper))
    )

    mask = np.ones((320, 320), dtype=np.uint8)
    mask[48:272, 101:111] = 0
    left_margin = _horizontal_containment_margin(mask, left_roi)
    right_margin = _horizontal_containment_margin(mask, right_roi)
    left_image_edge = min(left_roi[0], 320 - left_roi[0] - left_roi[2])
    internal_invalid_detected = left_margin < left_image_edge

    invalid_reasons: list[str] = []
    if holdout_exceedances:
        invalid_reasons.append("SYNTHETIC_SPATIAL_HOLDOUT_EXCEEDED")
    if not tail_detected:
        invalid_reasons.append("SYNTHETIC_TAIL_NOT_DETECTED")
    if not internal_invalid_detected:
        invalid_reasons.append("INTERNAL_INVALID_MASK_NOT_DETECTED")
    return {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_s0_synthetic_calibration",
        "status": "VALID" if not invalid_reasons else "INVALID",
        "invalid_reasons": invalid_reasons,
        "physical_output_capability": False,
        "probe_started": False,
        "mouse_opened": False,
        "production_aim_changed": False,
        "method": {
            "name": "opencv_phase_correlate",
            "opencv_version": cv2.__version__,
            "input": "generated_grayscale_float32_full_frame_then_fixed_roi",
            "window": "opencv_createHanningWindow_CV_32F",
            "warp": "opencv_warpAffine_INTER_LINEAR_BORDER_REFLECT_101",
            "response_used_as_probability": False,
        },
        "holdout_contract": {
            "random_case_split_allowed": False,
            "estimation_asset_count": 8,
            "untouched_holdout_asset_count": 2,
            "holdout_used_for_interval": False,
            "holdout_exceedance_count": len(holdout_exceedances),
        },
        "spatial_error_interval_px": {
            "lower": lower,
            "upper": upper,
            "coverage_method": "hull_of_preregistered_estimation_texture_shift_cases",
            "artificial_epsilon_added": False,
        },
        "tail_detection": {
            "truth_position_px": list(tail_truth),
            "truth_increment_px": [float(value) for value in truth_increments],
            "estimated_increment_px": observed,
            "all_injected_tails_detected": tail_detected,
            "physical_tail_claimed": False,
        },
        "mask_margin": {
            "left_horizontal_containment_margin_px": left_margin,
            "right_horizontal_containment_margin_px": right_margin,
            "internal_invalid_region_detected": internal_invalid_detected,
            "physical_occlusion_margin_claimed": False,
        },
        "cases": estimation + holdout,
    }


def _read_json(path: pathlib.Path, description: str) -> dict:
    if not path.is_file():
        raise ValueError(f"{description} 不存在")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{description} 不是 JSON object")
    return value


def _load_liveness_bracket(run: pathlib.Path, session: dict) -> dict:
    bracket_path = run / "s1-liveness-bracket.json"
    sequence_path = run / "sequence.json"
    report_path = run / "command-report.json"
    bracket = _read_json(bracket_path, "S1 liveness bracket")
    sequence = _read_json(sequence_path, "S1 liveness sequence")
    report = _read_json(report_path, "S1 command report")
    expected_sha256 = str(session.get("liveness_bracket_sha256", "")).lower()
    if not expected_sha256 or _file_sha256(bracket_path) != expected_sha256:
        raise ValueError("S1 liveness bracket SHA-256 不匹配")
    if (
        _file_sha256(sequence_path)
        != str(session.get("sequence_file_sha256", "")).lower()
        or _file_sha256(report_path)
        != str(session.get("command_report_sha256", "")).lower()
    ):
        raise ValueError("S1 sequence/command report 文件 SHA-256 不匹配")
    policy = bracket.get("policy")
    phases = bracket.get("phases")
    role = str(session.get("run_role", ""))
    expected_profile = f"dependency_calibration_a2_s1_{role}"
    request = sequence.get("request")
    samples = sequence.get("samples")
    events = report.get("result", {}).get("events")
    if (
        bracket.get("schema_version") != 2
        or bracket.get("evidence_type")
        != "mouse_effect_probe_a2_s1_liveness_bracket"
        or bracket.get("physical_output_capability") is not True
        or bracket.get("automated_input_generated") is not True
        or bracket.get("input_backend") != "kmbox_net"
        or bracket.get("manual_motion_required") is not False
        or bracket.get("phase_join_basis")
        != "command_event_source_timestamp_to_manifest"
        or bracket.get("sequence_sha256") != session.get("sequence_sha256")
        or bracket.get("sequence_file_sha256")
        != session.get("sequence_file_sha256")
        or bracket.get("command_report_sha256")
        != session.get("command_report_sha256")
        or not isinstance(policy, dict)
        or not str(policy.get("policy_id", ""))
        or int(policy.get("baseline_frame_count", 0)) <= 0
        or int(policy.get("challenge_pulse_count", 0)) <= 0
        or int(policy.get("challenge_stride_sample_count", 0)) <= 0
        or policy.get("challenge_frames_eligible_for_estimands") is not False
        or policy.get("settle_frames_eligible_for_estimands") is not False
        or not isinstance(phases, list)
        or len(phases) != len(_BRACKET_PHASE_NAMES)
        or sequence.get("schema") != 3
        or sequence.get("profile") != expected_profile
        or sequence.get("sequence_sha256") != session.get("sequence_sha256")
        or not isinstance(request, dict)
        or request.get("run_role") != role
        or int(request.get("challenge_pulse_count", 0))
        != int(policy.get("challenge_pulse_count", -1))
        or int(request.get("challenge_stride_sample_count", 0))
        != int(policy.get("challenge_stride_sample_count", -1))
        or int(request.get("baseline_sample_count", 0))
        != int(policy.get("baseline_frame_count", -1))
        or report.get("evidence_type")
        != "backend_completed_command_to_visible_background_response"
        or report.get("run_uuid") != session.get("run_uuid")
        or report.get("dispatch_mode") != "physical_a"
        or report.get("profile") != expected_profile
        or report.get("sequence_sha256") != session.get("sequence_sha256")
        or report.get("result", {}).get("state") != "completed"
        or report.get("result", {}).get("stop_reason") != "normal_completion"
        or report.get("result", {}).get("complete") is not True
        or int(report.get("result", {}).get(
            "cumulative_requested_x_counts", 1)) != 0
        or int(report.get("result", {}).get(
            "cumulative_backend_completed_x_counts", 1)) != 0
        or not isinstance(samples, list)
        or not isinstance(events, list)
        or len(samples) != len(events)
        or int(report.get("result", {}).get("consumed_sample_count", -1))
        != len(samples)
    ):
        raise ValueError("S1 liveness bracket 契约无效")

    normalized_phases: list[dict] = []
    previous_last = -1
    for expected_name, phase in zip(_BRACKET_PHASE_NAMES, phases, strict=True):
        if not isinstance(phase, dict) or phase.get("name") != expected_name:
            raise ValueError("S1 liveness bracket phase 顺序或名称无效")
        first = int(phase.get("first_sample_index", -1))
        last = int(phase.get("last_sample_index", -1))
        if first < 0 or last < first or first != previous_last + 1:
            raise ValueError("S1 liveness bracket phase 时间区间无效")
        previous_last = last
        normalized_phases.append({
            "name": expected_name,
            "first_sample_index": first,
            "last_sample_index": last,
        })
    if previous_last != len(samples) - 1:
        raise ValueError("S1 liveness bracket phase 未精确覆盖 sequence")

    pulse_count = int(request["challenge_pulse_count"])
    stride = int(request["challenge_stride_sample_count"])
    settle_count = int(request["settle_sample_count"])
    baseline_count = int(request["baseline_sample_count"])

    def challenge_dx(first_direction: int) -> list[int]:
        values: list[int] = []
        for direction in (first_direction, -first_direction):
            for _ in range(pulse_count):
                values.append(direction)
                values.extend([0] * (stride - 1))
        return values

    role_sign = 1 if role == "primary" else -1
    expected_dx = (
        challenge_dx(role_sign)
        + [0] * settle_count
        + [0] * baseline_count
        + challenge_dx(-role_sign)
    )
    if len(expected_dx) != len(samples):
        raise ValueError("S1 liveness sequence sample_count 与 request 不守恒")

    normalized_events: list[dict] = []
    cumulative = 0
    source_timestamps: set[int] = set()
    for index, (sample, event, expected) in enumerate(
        zip(samples, events, expected_dx, strict=True)
    ):
        if not isinstance(sample, dict) or not isinstance(event, dict):
            raise ValueError("S1 liveness sample/event 不是 object")
        dx = int(sample.get("dx_counts", 2))
        dy = int(sample.get("dy_counts", 1))
        timestamp = int(event.get("source_timestamp", -1))
        dispatched = dx != 0
        if (
            int(sample.get("sample_index", -1)) != index
            or int(event.get("sample_index", -1)) != index
            or event.get("run_uuid") != session.get("run_uuid")
            or event.get("sequence_sha256") != session.get("sequence_sha256")
            or int(event.get("block_id", -1))
            != int(sample.get("block_id", -2))
            or dx != expected
            or dy != 0
            or int(event.get("nominal_dx_counts", 2)) != dx
            or int(event.get("nominal_dy_counts", 1)) != 0
            or int(event.get("requested_dy_counts", 1)) != 0
            or event.get("source_timestamp_valid") is not True
            or event.get("source_time_basis") != "NDI_SDK_SUBMISSION"
            or event.get("source_clock_status") != "VALID"
            or int(event.get("source_dropped_frames", -1)) != 0
            or int(event.get("transport_dropped_frames", -1)) != 0
            or int(event.get("transport_invalid_packets", -1)) != 0
            or timestamp < 0
            or timestamp in source_timestamps
            or bool(event.get("dispatch_attempted")) != dispatched
            or int(event.get("requested_dx_counts", 2))
            != (dx if dispatched else 0)
            or bool(event.get("backend_succeeded")) != dispatched
            or bool(event.get("protocol_ack_received")) != dispatched
        ):
            raise ValueError("S1 liveness command ledger 不守恒")
        source_timestamps.add(timestamp)
        cumulative += dx
        if (
            int(event.get("cumulative_requested_x_counts", cumulative))
            != cumulative
            or int(event.get(
                "cumulative_backend_completed_x_counts", cumulative))
            != cumulative
        ):
            raise ValueError("S1 liveness command ledger 累计值不守恒")
        normalized_events.append(event)
    if cumulative != 0:
        raise ValueError("S1 liveness sequence 未回到 X anchor")

    phase_events: dict[str, list[dict]] = {}
    for phase in normalized_phases:
        phase_samples = samples[
            phase["first_sample_index"] : phase["last_sample_index"] + 1
        ]
        phase_slice = normalized_events[
            phase["first_sample_index"] : phase["last_sample_index"] + 1
        ]
        phase_events[phase["name"]] = phase_slice
        if phase["name"] == "RELEASE_AND_SETTLE" and any(
            item.get("phase") != "guard" or int(item.get("dx_counts", 1)) != 0
            for item in phase_samples
        ):
            raise ValueError("S1 settle phase 不是精确零命令 guard")
        if phase["name"] == "BASELINE_ZERO" and any(
            item.get("phase") != "baseline"
            or int(item.get("dx_counts", 1)) != 0
            for item in phase_samples
        ):
            raise ValueError("S1 baseline phase 不是精确零命令 baseline")
    return {
        "path": bracket_path,
        "document": bracket,
        "policy": policy,
        "phases": normalized_phases,
        "sequence_path": sequence_path,
        "report_path": report_path,
        "sequence": sequence,
        "report": report,
        "phase_events": phase_events,
    }


def _load_s1_session(run: pathlib.Path) -> dict:
    resolved = run.resolve()
    session_path = resolved / "s1-session.json"
    manifest_path = resolved / "pixel-evidence" / "manifest.json"
    session = _read_json(session_path, "S1 session")
    manifest = _read_json(manifest_path, "S1 manifest")
    frames = manifest.get("frames")
    capture_mode = str(session.get("capture_mode", "continuous_zero_input"))
    common_contract_invalid = (
        session.get("evidence_type") != "mouse_effect_probe_a2_s1_session"
        or session.get("status") != "RECORDED_UNANALYZED"
        or session.get("aim_off") is not True
        or manifest.get("evidence_type") != "output_off_capture"
        or manifest.get("physical_output_capability") is not False
        or not isinstance(frames, list)
        or len(frames) < 8
        or int(manifest.get("requested_frame_count", -1)) != len(frames)
        or int(manifest.get("recorded_frame_count", -1)) != len(frames)
    )
    continuous_contract_valid = (
        capture_mode == "continuous_zero_input"
        and session.get("physical_output_capability") is False
        and session.get("probe_started") is False
        and session.get("mouse_opened") is False
        and session.get("actual_command_zero") is True
        and manifest.get("source_binding", {}).get("sha256")
        == session.get("obs_source_binding_sha256")
    )
    bracketed_contract_valid = (
        capture_mode == "bracketed_kmbox"
        and session.get("schema_version") == 2
        and session.get("physical_output_capability") is True
        and session.get("probe_started") is True
        and session.get("mouse_opened") is True
        and session.get("actual_command_zero") is False
        and session.get("probe_command_zero") is False
        and session.get("baseline_actual_command_zero") is True
        and session.get("automated_kmbox_challenge") is True
        and session.get("challenge_frames_excluded_from_estimands") is True
        and manifest.get("source_binding", {}).get("sha256")
        == session.get("probe_binding_sha256")
    )
    if common_contract_invalid or not (
        continuous_contract_valid or bracketed_contract_valid
    ):
        raise ValueError("S1 session 未证明有效的 output-off capture/零命令 baseline 合同")
    decoded: list[tuple[dict, np.ndarray]] = []
    frame_root = manifest_path.parent.resolve()
    previous_source_time = -1
    for index, frame in enumerate(frames):
        if (
            not isinstance(frame, dict)
            or int(frame.get("index", -1)) != index
            or frame.get("source_timestamp_valid") is not True
            or frame.get("source_time_timing_valid") is not True
            or frame.get("source_clock_status") != "VALID"
            or frame.get("source_time_basis") != "NDI_SDK_SUBMISSION"
            or int(frame.get("source_dropped_frames", -1)) != 0
            or int(frame.get("transport_dropped_frames", -1)) != 0
            or int(frame.get("transport_invalid_packets", -1)) != 0
        ):
            raise ValueError("S1 frame timing/drop 身份无效")
        source_time = int(frame["source_time_at_steady_ns"])
        if source_time <= previous_source_time:
            raise ValueError("S1 source time 不严格递增")
        previous_source_time = source_time
        frame_path = (frame_root / str(frame.get("file", ""))).resolve()
        if not frame_path.is_relative_to(frame_root) or not frame_path.is_file():
            raise ValueError("S1 frame 路径逃逸或不存在")
        bgr = cv2.imread(str(frame_path), cv2.IMREAD_COLOR)
        if bgr is None or bgr.ndim != 3:
            raise ValueError("S1 frame 不能解码为 BGR")
        if _array_sha256(bgr) != str(frame.get("bgr_sha256", "")):
            raise ValueError("S1 decoded BGR SHA-256 不匹配")
        decoded.append((frame, bgr))
    all_decoded = decoded
    bracket = None
    phase_decoded: dict[str, list[tuple[dict, np.ndarray]]] = {}
    if capture_mode == "bracketed_kmbox":
        bracket = _load_liveness_bracket(resolved, session)
        frames_by_source_timestamp: dict[int, tuple[dict, np.ndarray]] = {}
        for item in all_decoded:
            timestamp = int(item[0]["source_timestamp"])
            if timestamp in frames_by_source_timestamp:
                raise ValueError("S1 manifest source_timestamp 无法唯一 join")
            frames_by_source_timestamp[timestamp] = item
        for phase_name, events in bracket["phase_events"].items():
            joined: list[tuple[dict, np.ndarray]] = []
            for event in events:
                item = frames_by_source_timestamp.get(
                    int(event["source_timestamp"])
                )
                if item is None:
                    raise ValueError("S1 command event 无对应 manifest frame")
                joined.append(item)
            phase_decoded[phase_name] = joined
        baseline_candidates = phase_decoded["BASELINE_ZERO"]
        baseline_frame_count = int(bracket["policy"]["baseline_frame_count"])
        if len(baseline_candidates) != baseline_frame_count:
            raise ValueError("S1 liveness bracket baseline frame 数不精确")
        decoded = baseline_candidates
        phase_decoded["BASELINE_ZERO"] = decoded
    return {
        "run": resolved,
        "session_path": session_path,
        "manifest_path": manifest_path,
        "session": session,
        "manifest": manifest,
        "decoded": decoded,
        "all_decoded": all_decoded,
        "capture_mode": capture_mode,
        "liveness_bracket": bracket,
        "phase_decoded": phase_decoded,
    }


def _linear_residual(values: Sequence[float]) -> tuple[np.ndarray, np.ndarray]:
    data = np.asarray(values, dtype=np.float64)
    positions = np.linspace(0.0, 1.0, data.size, dtype=np.float64)
    design = np.column_stack((np.ones(data.size), positions))
    coefficients, _, _, _ = np.linalg.lstsq(design, data, rcond=None)
    residual = data - design @ coefficients
    return coefficients, residual


def _session_rows(
    loaded: dict, left_roi: Roi, right_roi: Roi, block_count: int
) -> tuple[list[dict], dict]:
    decoded = loaded["decoded"]
    height, width = decoded[0][1].shape[:2]
    _validate_roi(left_roi, width, height, "left ROI")
    _validate_roi(right_roi, width, height, "right ROI")
    if any(item[1].shape[:2] != (height, width) for item in decoded):
        raise ValueError("S1 Run 内图像几何改变")
    if block_count < 2 or len(decoded) % block_count != 0:
        raise ValueError("S1 frame 必须能精确分成至少两个非重叠 block")
    block_size = len(decoded) // block_count
    if block_size < 8:
        raise ValueError("S1 每个非重叠 block 至少需要 8 frame")

    left_window = cv2.createHanningWindow(
        (left_roi[2], left_roi[3]), cv2.CV_32F
    )
    right_window = cv2.createHanningWindow(
        (right_roi[2], right_roi[3]), cv2.CV_32F
    )
    rows: list[dict] = []
    left_hashes: set[str] = set()
    right_hashes: set[str] = set()
    left_residuals: list[float] = []
    right_residuals: list[float] = []
    block_summaries: list[dict] = []
    for block_index in range(block_count):
        block = decoded[block_index * block_size : (block_index + 1) * block_size]
        left_images: list[np.ndarray] = []
        right_images: list[np.ndarray] = []
        for _, bgr in block:
            gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
            left = _roi(gray, left_roi)
            right = _roi(gray, right_roi)
            left_images.append(left)
            right_images.append(right)
            left_hashes.add(_array_sha256(left))
            right_hashes.add(_array_sha256(right))

        left_dx = [0.0]
        right_dx = [0.0]
        for index in range(1, block_size):
            left_dx.append(_measure_translation(
                left_images[index - 1], left_images[index], left_window
            )["dx_px"])
            right_dx.append(_measure_translation(
                right_images[index - 1], right_images[index], right_window
            )["dx_px"])
        left_position = np.cumsum(np.asarray(left_dx, dtype=np.float64))
        right_position = np.cumsum(np.asarray(right_dx, dtype=np.float64))
        left_coefficients, left_residual = _linear_residual(left_position)
        right_coefficients, right_residual = _linear_residual(right_position)
        left_residuals.extend(float(value) for value in left_residual)
        right_residuals.extend(float(value) for value in right_residual)
        block_summaries.append(
            {
                "block_id": block_index + 1,
                "first_frame_index": int(block[0][0]["index"]),
                "frame_count": block_size,
                "nuisance_columns": ["constant_1", "linear_block_fraction_0_to_1"],
                "left_coefficients": [float(value) for value in left_coefficients],
                "right_coefficients": [float(value) for value in right_coefficients],
                "left_residual_interval_px": [
                    float(np.min(left_residual)), float(np.max(left_residual))
                ],
                "right_residual_interval_px": [
                    float(np.min(right_residual)), float(np.max(right_residual))
                ],
            }
        )
        role = str(loaded["session"]["run_role"])
        for local_index, (frame, _) in enumerate(block):
            rows.append(
                {
                    "run_role": role,
                    "run_uuid": str(loaded["session"]["run_uuid"]),
                    "block_id": block_index + 1,
                    "frame_index": int(frame["index"]),
                    "source_timestamp": int(frame["source_timestamp"]),
                    "source_time_at_steady_ns": int(frame["source_time_at_steady_ns"]),
                    "phase": (
                        "BASELINE_ZERO"
                        if loaded["capture_mode"] == "bracketed_kmbox"
                        else "ZERO_INPUT"
                    ),
                    "left_x_hat_px": float(left_position[local_index]),
                    "left_d_hat_px": float(left_dx[local_index]),
                    "left_residual_px": float(left_residual[local_index]),
                    "right_x_hat_px": float(right_position[local_index]),
                    "right_d_hat_px": float(right_dx[local_index]),
                    "right_residual_px": float(right_residual[local_index]),
                }
            )
    return rows, {
        "frame_count": len(decoded),
        "block_count": block_count,
        "block_size": block_size,
        "left_unique_witness_frame_count": len(left_hashes),
        "right_unique_witness_frame_count": len(right_hashes),
        "left_residuals": left_residuals,
        "right_residuals": right_residuals,
        "blocks": block_summaries,
    }


def _nondegenerate(values: Sequence[float], unique_frames: int) -> bool:
    if unique_frames < 2 or len(values) < 2:
        return False
    data = np.asarray(values, dtype=np.float64)
    return bool(np.all(np.isfinite(data)) and float(np.max(data)) > float(np.min(data)))


def _challenge_image_change(
    loaded: dict, phase_name: str, left_roi: Roi, right_roi: Roi
) -> dict:
    decoded = loaded["phase_decoded"].get(phase_name, [])
    left_hashes: set[str] = set()
    right_hashes: set[str] = set()
    for _, bgr in decoded:
        gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
        _validate_roi(left_roi, gray.shape[1], gray.shape[0], "left ROI")
        _validate_roi(right_roi, gray.shape[1], gray.shape[0], "right ROI")
        left_hashes.add(_array_sha256(_roi(gray, left_roi)))
        right_hashes.add(_array_sha256(_roi(gray, right_roi)))
    return {
        "frame_count": len(decoded),
        "left_unique_frame_count": len(left_hashes),
        "right_unique_frame_count": len(right_hashes),
        "image_change_pass": len(left_hashes) >= 2 and len(right_hashes) >= 2,
    }


def _resolution_shift_rows(
    loaded: dict, roi: Roi, witness: str, shifts: Sequence[float], role: str
) -> list[dict]:
    _, baseline_bgr = loaded["decoded"][0]
    gray = cv2.cvtColor(baseline_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
    reference = _roi(gray, roi)
    window = cv2.createHanningWindow((roi[2], roi[3]), cv2.CV_32F)
    rows: list[dict] = []
    for truth_shift in shifts:
        moved = _roi(_warp_horizontal(gray, float(truth_shift)), roi)
        measured = _measure_translation(reference, moved, window)
        rows.append(
            {
                "role": role,
                "witness": witness,
                "truth_shift_px": float(truth_shift),
                "estimated_shift_px": measured["dx_px"],
                "signed_error_px": measured["dx_px"] - float(truth_shift),
                "phase_response": measured["response"],
            }
        )
    return rows


def _bracketed_null_interval(
    primary: dict, validation: dict, roi: Roi, witness: str
) -> dict:
    estimation = _resolution_shift_rows(
        primary, roi, witness, _BRACKET_PRIMARY_SHIFTS_PX, "primary_estimation"
    )
    holdout = _resolution_shift_rows(
        validation,
        roi,
        witness,
        _BRACKET_VALIDATION_SHIFTS_PX,
        "validation_holdout",
    )
    primary_errors = [float(row["signed_error_px"]) for row in estimation]
    signed_lower = min(primary_errors)
    signed_upper = max(primary_errors)
    error_radius = max(abs(value) for value in primary_errors)
    lower = -error_radius
    upper = error_radius
    exceedances = [
        row
        for row in holdout
        if float(row["signed_error_px"]) < lower
        or float(row["signed_error_px"]) > upper
    ]
    null_lower = -upper
    null_upper = -lower
    return {
        "nondegenerate": False,
        "variability_resolved": False,
        "observed_interval_px": [0.0, 0.0],
        "noise_distribution_claimed": False,
        "artificial_epsilon_added": False,
        "null_displacement_interval_px": [null_lower, null_upper],
        "coverage_method": (
            "actual_baseline_symmetric_primary_absolute_error_hull_with_untouched_"
            "validation_shift_exceedance_check"
        ),
        "primary_signed_error_interval_px": [lower, upper],
        "primary_observed_signed_error_interval_px": [
            signed_lower, signed_upper
        ],
        "validation_exceedance_count": len(exceedances),
        "validation_passed": len(exceedances) == 0,
        "resolution_calibration": {
            "primary_shifts_px": list(_BRACKET_PRIMARY_SHIFTS_PX),
            "validation_holdout_shifts_px": list(_BRACKET_VALIDATION_SHIFTS_PX),
            "random_case_split_allowed": False,
            "holdout_used_for_interval": False,
            "rows": estimation + holdout,
        },
    }


def analyze_zero_input_sessions(
    primary_run: pathlib.Path,
    validation_run: pathlib.Path,
    left_roi: Roi,
    right_roi: Roi,
    *,
    block_count: int,
) -> tuple[dict, list[dict]]:
    primary = _load_s1_session(pathlib.Path(primary_run))
    validation = _load_s1_session(pathlib.Path(validation_run))
    if primary["session"].get("run_role") != "primary" or \
            validation["session"].get("run_role") != "validation":
        raise ValueError("S1 session role 必须是 primary/validation")
    if primary["session"].get("scope_id") != validation["session"].get("scope_id"):
        raise ValueError("S1 primary/validation scope_id 不一致")
    if primary["capture_mode"] != validation["capture_mode"]:
        raise ValueError("S1 primary/validation capture_mode 不一致")
    bracketed_static = primary["capture_mode"] == "bracketed_kmbox"

    primary_rows, primary_summary = _session_rows(
        primary, left_roi, right_roi, block_count
    )
    validation_rows, validation_summary = _session_rows(
        validation, left_roi, right_roi, block_count
    )
    all_rows = primary_rows + validation_rows

    primary_frame_hashes = {
        str(frame["bgr_sha256"]) for frame, _ in primary["all_decoded"]
    }
    validation_frame_hashes = {
        str(frame["bgr_sha256"]) for frame, _ in validation["all_decoded"]
    }
    primary_times = [
        int(frame["source_time_at_steady_ns"])
        for frame, _ in primary["all_decoded"]
    ]
    validation_times = [
        int(frame["source_time_at_steady_ns"])
        for frame, _ in validation["all_decoded"]
    ]
    time_overlap = not (
        max(primary_times) < min(validation_times)
        or max(validation_times) < min(primary_times)
    )
    primary_clock_sessions = {
        str(frame["source_clock_session_id"])
        for frame, _ in primary["all_decoded"]
    }
    validation_clock_sessions = {
        str(frame["source_clock_session_id"])
        for frame, _ in validation["all_decoded"]
    }
    distinct_capture_process_session = (
        primary["session"].get("capture_process_session_id")
        != validation["session"].get("capture_process_session_id")
    )
    independence = {
        "distinct_run_uuid": primary["session"].get("run_uuid")
        != validation["session"].get("run_uuid"),
        "distinct_capture_process_session": distinct_capture_process_session,
        # source_clock_session_id 是时钟源的稳定 epoch，不是本次客户端
        # capture/mapping observation 的身份；独立捕获进程各自建立新 mapper。
        "distinct_timing_observation_session": distinct_capture_process_session,
        "distinct_source_clock_session": primary_clock_sessions.isdisjoint(
            validation_clock_sessions
        ),
        "source_time_ranges_overlap": time_overlap,
        "decoded_frame_hash_overlap_count": len(
            primary_frame_hashes & validation_frame_hashes
        ),
        "independence_unit": (
            "nonoverlapping_separate_capture_and_timing_observation"
        ),
        "identical_static_indications_disqualify_independence": not bracketed_static,
        "statistical_independence_proven": False,
    }

    invalid_reasons: list[str] = []
    independence_checks = [
        independence["distinct_run_uuid"],
        independence["distinct_capture_process_session"],
        independence["distinct_timing_observation_session"],
        not independence["source_time_ranges_overlap"],
    ]
    if not bracketed_static:
        independence_checks.append(
            independence["decoded_frame_hash_overlap_count"] == 0
        )
    if not all(independence_checks):
        invalid_reasons.append("OPERATIONAL_INDEPENDENCE_MISSING")

    liveness_bracket = None
    if bracketed_static:
        primary_pre = _challenge_image_change(
            primary, "PRE_LIVENESS_CHALLENGE", left_roi, right_roi
        )
        primary_post = _challenge_image_change(
            primary, "POST_LIVENESS_CHALLENGE", left_roi, right_roi
        )
        validation_pre = _challenge_image_change(
            validation, "PRE_LIVENESS_CHALLENGE", left_roi, right_roi
        )
        validation_post = _challenge_image_change(
            validation, "POST_LIVENESS_CHALLENGE", left_roi, right_roi
        )

        def liveness_run_summary(
            loaded: dict, pre: dict, post: dict
        ) -> dict:
            pre_events = loaded["liveness_bracket"]["phase_events"][
                "PRE_LIVENESS_CHALLENGE"
            ]
            post_events = loaded["liveness_bracket"]["phase_events"][
                "POST_LIVENESS_CHALLENGE"
            ]

            def ledger_summary(events: Sequence[dict]) -> dict:
                pulses = [
                    int(event["nominal_dx_counts"])
                    for event in events
                    if int(event["nominal_dx_counts"]) != 0
                ]
                return {
                    "event_count": len(events),
                    "pulse_count": len(pulses),
                    "net_x_counts": sum(pulses),
                    "positive_pulse_count": sum(value > 0 for value in pulses),
                    "negative_pulse_count": sum(value < 0 for value in pulses),
                    "backend_ack_conserved": True,
                    "pass": bool(pulses)
                    and sum(pulses) == 0
                    and any(value > 0 for value in pulses)
                    and any(value < 0 for value in pulses),
                }

            pre_ledger = ledger_summary(pre_events)
            post_ledger = ledger_summary(post_events)
            return {
                "run_uuid": loaded["session"]["run_uuid"],
                "bracket_file_sha256": _file_sha256(
                    loaded["liveness_bracket"]["path"]
                ),
                "sequence_file_sha256": _file_sha256(
                    loaded["liveness_bracket"]["sequence_path"]
                ),
                "command_report_file_sha256": _file_sha256(
                    loaded["liveness_bracket"]["report_path"]
                ),
                "manual_motion_required": False,
                "pre": pre,
                "post": post,
                "pre_command_ledger": pre_ledger,
                "post_command_ledger": post_ledger,
                "pre_command_ledger_pass": pre_ledger["pass"],
                "post_command_ledger_pass": post_ledger["pass"],
                "pre_image_change_pass": pre["image_change_pass"],
                "post_image_change_pass": post["image_change_pass"],
            }

        policy_frozen = (
            primary["liveness_bracket"]["policy"]
            == validation["liveness_bracket"]["policy"]
        )
        liveness_bracket = {
            "policy_frozen_across_runs": policy_frozen,
            "automated_kmbox_challenge": True,
            "fixed_source_frame_command_cadence": True,
            "fixed_pixel_speed_used_as_gate": False,
            "challenge_frames_excluded_from_estimands": True,
            "settle_frames_excluded_from_estimands": True,
            "decoded_change_proves_every_baseline_frame_fresh": False,
            "command_ledger_proves_game_consumed_input": False,
            "primary": liveness_run_summary(primary, primary_pre, primary_post),
            "validation": liveness_run_summary(
                validation, validation_pre, validation_post
            ),
        }
        if not policy_frozen:
            invalid_reasons.append("LIVENESS_POLICY_CHANGED_ACROSS_RUNS")
        for role, pre, post, summary in (
            ("PRIMARY", primary_pre, primary_post, primary_summary),
            ("VALIDATION", validation_pre, validation_post, validation_summary),
        ):
            if not pre["image_change_pass"]:
                invalid_reasons.append(f"{role}_PRE_LIVENESS_CHALLENGE_MISSING")
            if not post["image_change_pass"]:
                invalid_reasons.append(f"{role}_POST_LIVENESS_CHALLENGE_MISSING")
            if (
                summary["left_unique_witness_frame_count"] != 1
                or summary["right_unique_witness_frame_count"] != 1
            ):
                invalid_reasons.append(f"{role}_BASELINE_NOT_DIGITALLY_STATIC")

        noise_model = {
            "iid_claimed": False,
            "artificial_epsilon_added": False,
            "variability_distribution_claimed": False,
            "coverage_method": "actual_baseline_resolution_type_b_interval",
        }
        for witness, roi in (("left", left_roi), ("right", right_roi)):
            model = _bracketed_null_interval(primary, validation, roi, witness)
            noise_model[witness] = model
            if not model["validation_passed"]:
                invalid_reasons.append(
                    f"{witness.upper()}_RESOLUTION_VALIDATION_EXCEEDED"
                )
            null_interval = model["null_displacement_interval_px"]
            if not (float(null_interval[0]) < 0.0 < float(null_interval[1])):
                invalid_reasons.append(
                    f"{witness.upper()}_NULL_INTERVAL_EXCLUDES_ZERO"
                )
    else:
        noise_model = {
            "iid_claimed": False,
            "artificial_epsilon_added": False,
            "nuisance": ["constant_1", "linear_block_fraction_0_to_1"],
            "coverage_method": (
                "primary_residual_hull_with_separate_validation_exceedance_check"
            ),
        }
        for witness in ("left", "right"):
            primary_values = primary_summary[f"{witness}_residuals"]
            validation_values = validation_summary[f"{witness}_residuals"]
            primary_nondegenerate = _nondegenerate(
                primary_values,
                primary_summary[f"{witness}_unique_witness_frame_count"],
            )
            validation_nondegenerate = _nondegenerate(
                validation_values,
                validation_summary[f"{witness}_unique_witness_frame_count"],
            )
            lower = float(min(primary_values))
            upper = float(max(primary_values))
            exceedances = [
                value
                for value in validation_values
                if float(value) < lower or float(value) > upper
            ]
            nondegenerate = primary_nondegenerate and validation_nondegenerate
            noise_model[witness] = {
                "nondegenerate": nondegenerate,
                "primary_interval_px": [lower, upper],
                "primary": _distribution(primary_values),
                "validation": _distribution(validation_values),
                "validation_exceedance_count": len(exceedances),
                "validation_passed": len(exceedances) == 0,
            }
            if (
                not nondegenerate
                and "NONDEGENERATE_NOISE_MISSING" not in invalid_reasons
            ):
                invalid_reasons.append("NONDEGENERATE_NOISE_MISSING")
            if exceedances:
                invalid_reasons.append(
                    f"{witness.upper()}_NOISE_VALIDATION_EXCEEDED"
                )

    all_frames = primary["decoded"] + validation["decoded"]
    source_periods = []
    mapping_uncertainties_ms = []
    for loaded in (primary, validation):
        times = [
            int(frame["source_time_at_steady_ns"])
            for frame, _ in loaded["decoded"]
        ]
        source_periods.extend(
            float(times[index] - times[index - 1])
            for index in range(1, len(times))
        )
        mapping_uncertainties_ms.extend(
            float(frame.get("source_clock_uncertainty_ms", 0.0))
            for frame, _ in loaded["decoded"]
        )

    result = {
        "schema_version": 2 if bracketed_static else 1,
        "evidence_type": "mouse_effect_probe_a2_s1_zero_input_calibration",
        "status": "VALID" if not invalid_reasons else "INVALID",
        "measurement_state": (
            "VALID_BRACKETED_CENSORED_ZERO"
            if bracketed_static and not invalid_reasons
            else "INVALID_BRACKETED_CENSORED_ZERO"
            if bracketed_static
            else "VALID_OBSERVED_NONDEGENERATE"
            if not invalid_reasons
            else "INVALID_OBSERVED_VARIABILITY"
        ),
        "invalid_reasons": invalid_reasons,
        "physical_output_capability": False,
        "probe_started": bracketed_static,
        "mouse_opened": bracketed_static,
        "physical_challenge_executed": bracketed_static,
        "actual_command_zero": not bracketed_static,
        "baseline_actual_command_zero": True,
        "production_aim_changed": False,
        "scope_id": primary["session"]["scope_id"],
        "geometry": {
            "left_roi": list(left_roi),
            "right_roi": list(right_roi),
        },
        "operational_independence": independence,
        "liveness_bracket": liveness_bracket,
        "noise_model": noise_model,
        "source_cadence": {
            "period_ns": _distribution(source_periods),
            "mapping_uncertainty_ms": _distribution(mapping_uncertainties_ms),
            "time_uncertainty_converted_with_fixed_speed": False,
        },
        "sessions": {
            "primary": {
                "run_uuid": primary["session"]["run_uuid"],
                "session_file_sha256": _file_sha256(primary["session_path"]),
                "manifest_file_sha256": _file_sha256(primary["manifest_path"]),
                **{key: value for key, value in primary_summary.items()
                   if not key.endswith("residuals")},
            },
            "validation": {
                "run_uuid": validation["session"]["run_uuid"],
                "session_file_sha256": _file_sha256(validation["session_path"]),
                "manifest_file_sha256": _file_sha256(validation["manifest_path"]),
                **{key: value for key, value in validation_summary.items()
                   if not key.endswith("residuals")},
            },
        },
        "frame_count": len(all_frames),
    }
    return result, all_rows


def derive_dependency_calibration_plan(
    synthetic_calibration: dict,
    zero_input_calibration: dict,
    *,
    observed_lag_reference: int,
    candidate_horizons: Sequence[int],
) -> dict:
    if synthetic_calibration.get("status") != "VALID":
        raise ValueError("A2 plan 要求 S0 synthetic calibration 为 VALID")
    if zero_input_calibration.get("status") != "VALID":
        raise ValueError("A2 plan 要求 S1 zero-input calibration 为 VALID")
    horizons = sorted({int(value) for value in candidate_horizons})
    if (
        observed_lag_reference <= 0
        or not horizons
        or any(value < observed_lag_reference for value in horizons)
        or max(horizons) <= observed_lag_reference
    ):
        raise ValueError("candidate horizon 必须覆盖 observed lag 且至少一个更长")
    period = zero_input_calibration.get("source_cadence", {}).get(
        "period_ns", {}
    )
    uncertainty = zero_input_calibration.get("source_cadence", {}).get(
        "mapping_uncertainty_ms", {}
    )
    minimum_period_ns = float(period.get("min", 0.0))
    maximum_uncertainty_ms = float(uncertainty.get("max", -1.0))
    if (
        not math.isfinite(minimum_period_ns)
        or minimum_period_ns <= 0.0
        or not math.isfinite(maximum_uncertainty_ms)
        or maximum_uncertainty_ms < 0.0
    ):
        raise ValueError("S1 source cadence/mapping uncertainty 不完整")

    # 这里只把时间不确定度换算成候选 source-frame 个数；不乘固定速度，
    # 也不输出像素不确定度。双边 mapper interval 用 2*u 覆盖。
    source_ambiguity_frames = max(
        1,
        int(math.ceil(
            2.0 * maximum_uncertainty_ms * 1_000_000.0
            / minimum_period_ns
        )),
    )
    maximum_horizon = max(horizons)
    post_tail_validation_samples = maximum_horizon
    response_samples = maximum_horizon + source_ambiguity_frames
    guard_samples = (
        maximum_horizon
        + source_ambiguity_frames
        + post_tail_validation_samples
    )
    # 四个完整 block 是 direction(+/-) x placement(early/late) 的最小
    # block-level coverage；ABBA/BAAB 不把连续 frame 当重复样本。
    block_count = 4
    primary_block_size = int(
        zero_input_calibration.get("sessions", {})
        .get("primary", {})
        .get("block_size", 0)
    )
    if primary_block_size <= 0:
        raise ValueError("S1 primary block_size 缺失")
    baseline_samples = max(2 * guard_samples, primary_block_size)
    per_block_samples = 2 * guard_samples + 2 + 2 * response_samples
    sample_count = baseline_samples + block_count * per_block_samples
    if sample_count >= 2400:
        raise ValueError("派生 A2 sequence 超出 2400-frame 短证据容量")
    sidecar_frames = 2400
    maximum_period_ns = float(period.get("max", minimum_period_ns))
    max_seconds = min(
        60,
        max(1, int(math.ceil(sidecar_frames * maximum_period_ns / 1e9)) + 5),
    )
    return {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_dependency_calibration_plan",
        "status": "VALID_OFFLINE_PLAN",
        "physical_output_capability": False,
        "physical_prepare_authorized_by_artifact": False,
        "physical_launch_authorized": False,
        "production_aim_changed": False,
        "scope_id": zero_input_calibration.get("scope_id"),
        "sequence_request": {
            "baseline_sample_count": baseline_samples,
            "response_sample_count": response_samples,
            "guard_sample_count": guard_samples,
            "block_count": block_count,
            "sample_count": sample_count,
            "nonzero_transition_count": block_count * 2,
            "net_x_counts": 0,
            "max_abs_prefix_x_counts": 1,
            "dy_counts_required": 0,
        },
        "roles": {
            "p_cal": {
                "profile": "dependency_calibration_a2_p_cal",
                "direction_order": [1, -1, -1, 1],
                "uses_holdout_for_tuning": False,
            },
            "p_holdout": {
                "profile": "dependency_calibration_a2_p_holdout",
                "direction_order": [-1, 1, 1, -1],
                "uses_holdout_for_tuning": False,
            },
        },
        "sidecar": {
            "frame_count": sidecar_frames,
            "max_seconds": max_seconds,
            "short_evidence_capacity": 2400,
        },
        "derivation": {
            "observed_lag_reference": observed_lag_reference,
            "candidate_horizons": horizons,
            "maximum_candidate_horizon": maximum_horizon,
            "source_mapping_ambiguity_frames": source_ambiguity_frames,
            "source_mapping_ambiguity_semantic":
                "candidate_frame_count_not_pixel_uncertainty",
            "post_tail_validation_samples": post_tail_validation_samples,
            "block_coverage_target":
                "direction_positive_negative_x_early_late_ABBA",
            "reused_physical_a_guard_or_gain": False,
            "fixed_speed_used": False,
        },
        "dependency_state_after_plan": {
            "zero_command_disturbance_bound": (
                "GREEN_S1_BRACKETED_CENSORED_ZERO"
                if zero_input_calibration.get("measurement_state")
                == "VALID_BRACKETED_CENSORED_ZERO"
                else "GREEN_S1_OBSERVED_VARIABILITY"
            ),
            "independent_nondegenerate_noise": (
                "NOT_CLAIMED_STATIC_DIGITAL_SCOPE"
                if zero_input_calibration.get("measurement_state")
                == "VALID_BRACKETED_CENSORED_ZERO"
                else "GREEN_S1_SCOPE"
            ),
            "independent_tail_support": "PENDING_USER_PHYSICAL",
            "witness_occlusion_margin": "PENDING_USER_PHYSICAL",
            "mapping_uncertainty_px": "PENDING_USER_PHYSICAL",
            "single_count_gain_upper_scope": "PENDING_USER_PHYSICAL",
        },
    }


def _parse_roi(value: str) -> Roi:
    parts = value.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("ROI 必须为 x,y,width,height")
    try:
        roi = tuple(int(part) for part in parts)
    except ValueError as exception:
        raise argparse.ArgumentTypeError("ROI 必须为整数") from exception
    return roi  # type: ignore[return-value]


def _write_new_text(path: pathlib.Path, content: str) -> None:
    resolved = path.resolve()
    if resolved.exists():
        raise ValueError(f"输出已存在，拒绝覆盖：{resolved}")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    pending = resolved.with_name(f".{resolved.name}.pending-{os.getpid()}")
    if pending.exists():
        raise ValueError(f"临时输出已存在：{pending}")
    try:
        pending.write_text(content, encoding="utf-8", newline="\n")
        os.rename(pending, resolved)
    finally:
        if pending.exists():
            pending.unlink()


def _parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Physical A2 S0/S1 calibration；不打开 Capture/Probe/Mouse"
    )
    subparsers = parser.add_subparsers(dest="mode", required=True)
    synthetic = subparsers.add_parser("synthetic")
    synthetic.add_argument("--output", type=pathlib.Path, required=True)

    noise = subparsers.add_parser("noise")
    noise.add_argument("--primary-run", type=pathlib.Path, required=True)
    noise.add_argument("--validation-run", type=pathlib.Path, required=True)
    noise.add_argument("--left-roi", type=_parse_roi, required=True)
    noise.add_argument("--right-roi", type=_parse_roi, required=True)
    noise.add_argument("--blocks", type=int, required=True)
    noise.add_argument("--output", type=pathlib.Path, required=True)
    noise.add_argument("--rows-csv", type=pathlib.Path, required=True)

    plan = subparsers.add_parser("plan")
    plan.add_argument("--synthetic-calibration", type=pathlib.Path, required=True)
    plan.add_argument("--zero-input-calibration", type=pathlib.Path, required=True)
    plan.add_argument("--observed-lag-reference", type=int, required=True)
    plan.add_argument("--candidate-horizons", required=True)
    plan.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = _parse_arguments(arguments)
    try:
        if options.mode == "synthetic":
            result = build_synthetic_calibration()
            _write_new_text(
                options.output,
                json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False)
                + "\n",
            )
            print(f"A2 S0 {result['status']}: output={options.output.resolve()}")
            return 0 if result["status"] == "VALID" else 1

        if options.mode == "plan":
            synthetic = _read_json(
                options.synthetic_calibration, "S0 synthetic calibration"
            )
            zero_input = _read_json(
                options.zero_input_calibration, "S1 zero-input calibration"
            )
            try:
                horizons = tuple(
                    int(value) for value in options.candidate_horizons.split(",")
                )
            except ValueError as exception:
                raise ValueError("candidate horizons 必须为逗号分隔整数") from exception
            result = derive_dependency_calibration_plan(
                synthetic,
                zero_input,
                observed_lag_reference=options.observed_lag_reference,
                candidate_horizons=horizons,
            )
            _write_new_text(
                options.output,
                json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False)
                + "\n",
            )
            print(f"A2 plan VALID: output={options.output.resolve()}")
            return 0

        result, rows = analyze_zero_input_sessions(
            options.primary_run,
            options.validation_run,
            options.left_roi,
            options.right_roi,
            block_count=options.blocks,
        )
        if not rows:
            raise ValueError("S1 没有逐帧证据行")
        if options.rows_csv.resolve().exists():
            raise ValueError("rows CSV 已存在，拒绝覆盖")
        fields = list(rows[0].keys())
        pending_csv = options.rows_csv.resolve().with_name(
            f".{options.rows_csv.name}.pending-{os.getpid()}"
        )
        options.rows_csv.resolve().parent.mkdir(parents=True, exist_ok=True)
        try:
            with pending_csv.open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            os.rename(pending_csv, options.rows_csv.resolve())
        finally:
            if pending_csv.exists():
                pending_csv.unlink()
        result["rows_csv"] = str(options.rows_csv.resolve())
        _write_new_text(
            options.output,
            json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False)
            + "\n",
        )
        print(
            f"A2 S1 {result['status']}: frames={result['frame_count']}, "
            f"output={options.output.resolve()}"
        )
        return 0 if result["status"] == "VALID" else 1
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        print(f"A2 calibration failed: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
