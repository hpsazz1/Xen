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


def _load_s1_session(run: pathlib.Path) -> dict:
    resolved = run.resolve()
    session_path = resolved / "s1-session.json"
    manifest_path = resolved / "pixel-evidence" / "manifest.json"
    session = _read_json(session_path, "S1 session")
    manifest = _read_json(manifest_path, "S1 manifest")
    frames = manifest.get("frames")
    if (
        session.get("evidence_type") != "mouse_effect_probe_a2_s1_session"
        or session.get("status") != "RECORDED_UNANALYZED"
        or session.get("physical_output_capability") is not False
        or session.get("probe_started") is not False
        or session.get("mouse_opened") is not False
        or session.get("actual_command_zero") is not True
        or session.get("aim_off") is not True
        or manifest.get("evidence_type") != "output_off_capture"
        or manifest.get("physical_output_capability") is not False
        or not isinstance(frames, list)
        or len(frames) < 8
        or int(manifest.get("requested_frame_count", -1)) != len(frames)
        or int(manifest.get("recorded_frame_count", -1)) != len(frames)
        or manifest.get("source_binding", {}).get("sha256")
        != session.get("obs_source_binding_sha256")
    ):
        raise ValueError("S1 session 未证明无 Probe/Mouse 的完整 output-off capture")
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
    return {
        "run": resolved,
        "session_path": session_path,
        "manifest_path": manifest_path,
        "session": session,
        "manifest": manifest,
        "decoded": decoded,
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
                "first_frame_index": block_index * block_size,
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

    primary_rows, primary_summary = _session_rows(
        primary, left_roi, right_roi, block_count
    )
    validation_rows, validation_summary = _session_rows(
        validation, left_roi, right_roi, block_count
    )
    all_rows = primary_rows + validation_rows

    primary_frame_hashes = {
        str(frame["bgr_sha256"]) for frame, _ in primary["decoded"]
    }
    validation_frame_hashes = {
        str(frame["bgr_sha256"]) for frame, _ in validation["decoded"]
    }
    primary_times = [
        int(frame["source_time_at_steady_ns"]) for frame, _ in primary["decoded"]
    ]
    validation_times = [
        int(frame["source_time_at_steady_ns"])
        for frame, _ in validation["decoded"]
    ]
    time_overlap = not (
        max(primary_times) < min(validation_times)
        or max(validation_times) < min(primary_times)
    )
    primary_clock_sessions = {
        str(frame["source_clock_session_id"]) for frame, _ in primary["decoded"]
    }
    validation_clock_sessions = {
        str(frame["source_clock_session_id"])
        for frame, _ in validation["decoded"]
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
            "nonoverlapping_block_separate_capture_and_timing_observation"
        ),
        "statistical_independence_proven": False,
    }

    invalid_reasons: list[str] = []
    if not all(
        (
            independence["distinct_run_uuid"],
            independence["distinct_capture_process_session"],
            independence["distinct_timing_observation_session"],
            not independence["source_time_ranges_overlap"],
            independence["decoded_frame_hash_overlap_count"] == 0,
        )
    ):
        invalid_reasons.append("OPERATIONAL_INDEPENDENCE_MISSING")

    noise_model: dict = {
        "iid_claimed": False,
        "artificial_epsilon_added": False,
        "nuisance": ["constant_1", "linear_block_fraction_0_to_1"],
        "coverage_method": "primary_residual_hull_with_separate_validation_exceedance_check",
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
        if not nondegenerate and "NONDEGENERATE_NOISE_MISSING" not in invalid_reasons:
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
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_s1_zero_input_calibration",
        "status": "VALID" if not invalid_reasons else "INVALID",
        "invalid_reasons": invalid_reasons,
        "physical_output_capability": False,
        "probe_started": False,
        "mouse_opened": False,
        "actual_command_zero": True,
        "production_aim_changed": False,
        "scope_id": primary["session"]["scope_id"],
        "geometry": {
            "left_roi": list(left_roi),
            "right_roi": list(right_roi),
        },
        "operational_independence": independence,
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
            "independent_nondegenerate_noise": "GREEN_S1_SCOPE",
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
