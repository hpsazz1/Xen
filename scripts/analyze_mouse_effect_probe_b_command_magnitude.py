#!/usr/bin/env python3
"""离线分析 Physical B 多幅值 Primary；不具备 Mouse 或 Launch 能力。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys
from typing import Any

import cv2
import numpy as np


SAMPLE_COUNT = 1684
BLOCK_COUNT = 10
BASELINE_COUNT = 64
BLOCK_SAMPLE_COUNT = 162
GUARD_COUNT = 32
RESPONSE_COUNT = 48
PULSE_RESPONSE_ROWS = RESPONSE_COUNT + 1
AMPLITUDES = (1, 1, 4, 4, 13, 13, 2, 2, 8, 8)
ROLES = ("estimation",) * 6 + ("confirmation",) * 4
POLARITIES = ("normal", "inverted") * 5
ROIS = {
    "left": (16, 48, 96, 224),
    "right": (208, 48, 96, 224),
}
F1_DELAY_SAMPLES = 4


def _exact_int(value: Any, expected: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and \
        value == expected


def _validate_sequence_payload(sequence: dict[str, Any]) -> None:
    """独立复核 C++ 生成器的完整 sample 布局，拒绝只改摘要的漂移。"""
    request = sequence.get("request")
    summary = sequence.get("summary")
    samples = sequence.get("samples")
    blocks = sequence.get("blocks")
    sequence_sha256 = sequence.get("sequence_sha256")
    if not isinstance(request, dict) or not isinstance(summary, dict) or \
            not isinstance(samples, list) or not isinstance(blocks, list) or \
            not isinstance(sequence_sha256, str) or \
            len(sequence_sha256) != 64 or \
            any(character not in "0123456789abcdef"
                for character in sequence_sha256) or \
            not _exact_int(sequence.get("schema"), 6) or \
            sequence.get("profile") != \
                "physical_b_command_magnitude_primary" or \
            request.get("run_role") != "primary" or \
            not _exact_int(request.get("baseline_sample_count"),
                           BASELINE_COUNT) or \
            not _exact_int(request.get("response_sample_count"),
                           RESPONSE_COUNT) or \
            not _exact_int(request.get("guard_sample_count"), GUARD_COUNT) or \
            not _exact_int(summary.get("net_x_counts"), 0) or \
            not _exact_int(summary.get("max_abs_prefix_x_counts"), 13) or \
            len(samples) != SAMPLE_COUNT or len(blocks) != BLOCK_COUNT:
        raise ValueError("Primary sequence header/request/summary 无效")

    expected_samples: list[tuple[int, str, int]] = [
        (0, "baseline", 0) for _ in range(BASELINE_COUNT)
    ]
    for block_index in range(BLOCK_COUNT):
        block_id = block_index + 1
        amplitude = AMPLITUDES[block_index]
        sign = 1 if POLARITIES[block_index] == "normal" else -1
        expected_first = BASELINE_COUNT + block_index * BLOCK_SAMPLE_COUNT
        block = blocks[block_index]
        if not isinstance(block, dict) or \
                not _exact_int(block.get("block_id"), block_id) or \
                not _exact_int(block.get("pair_index"),
                               block_index // 2 + 1) or \
                block.get("role") != ROLES[block_index] or \
                block.get("polarity") != POLARITIES[block_index] or \
                not _exact_int(block.get("amplitude_counts"), amplitude) or \
                not _exact_int(block.get("first_sample_index"),
                               expected_first) or \
                not _exact_int(block.get("sample_count"),
                               BLOCK_SAMPLE_COUNT):
            raise ValueError("Primary sequence block 布局无效")
        expected_samples.extend(
            (block_id, "guard", 0) for _ in range(GUARD_COUNT))
        expected_samples.append((block_id, "pulse", sign * amplitude))
        expected_samples.extend(
            (block_id, "response", 0) for _ in range(RESPONSE_COUNT))
        expected_samples.append((block_id, "pulse", -sign * amplitude))
        expected_samples.extend(
            (block_id, "response", 0) for _ in range(RESPONSE_COUNT))
        expected_samples.extend(
            (block_id, "guard", 0) for _ in range(GUARD_COUNT))

    if len(expected_samples) != SAMPLE_COUNT:
        raise RuntimeError("内部 Primary sequence 期望容量错误")
    for index, (sample, expected) in enumerate(zip(samples, expected_samples)):
        expected_block, expected_phase, expected_dx = expected
        if not isinstance(sample, dict) or \
                not _exact_int(sample.get("sample_index"), index) or \
                not _exact_int(sample.get("block_id"), expected_block) or \
                sample.get("phase") != expected_phase or \
                not _exact_int(sample.get("dx_counts"), expected_dx) or \
                not _exact_int(sample.get("dy_counts"), 0):
            raise ValueError(f"Primary sequence sample[{index}] 布局无效")


def _canonical_sha256(value: dict[str, Any]) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def analysis_contract() -> dict[str, Any]:
    contract: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_command_magnitude_analysis_contract",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "measurement": {
            "decode": "opencv_imread_color_bgr",
            "gray": "opencv_bgr2gray_float32",
            "translation":
                "opencv_phase_correlate_hann_current_relative_to_anchor",
            "pulse_anchor": "exact_event_frame_immediately_before_pulse",
            "nuisance_slope":
                "linear_fit_on_each_block_exact_32_frame_pre_guard_only",
            "source_join": "exact_int64_ndi_submission_timestamp",
            "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
            "response_rows_including_pulse": PULSE_RESPONSE_ROWS,
            "left_roi": list(ROIS["left"]),
            "right_roi": list(ROIS["right"]),
        },
        "model": {
            "family": "causal_step_response_diagnostic",
            "input": "backend_completed_relative_command_dx_counts",
            "fit": "least_squares_through_origin_at_each_response_offset",
            "fit_amplitudes": [1, 4, 13],
            "within_run_confirmation_amplitudes": [2, 8],
            "fit_role": "estimation_whole_pulses_only",
            "confirmation_used_for_refit": False,
            "output_feedback_used": False,
            "f1_delay_samples": F1_DELAY_SAMPLES,
        },
        "deletion_tests": {
            "unit": "every_confirmation_pulse_and_both_witnesses",
            "primary_model_strictly_beats_f1_rmse": True,
            "primary_model_strictly_beats_f1_max": True,
            "background_direction_opposes_command": True,
            "aggregate_masking_allowed": False,
            "failure_status": "LINEAR_STEP_RESPONSE_DELETED",
            "survival_status": "READY_FOR_COMMAND_MAGNITUDE_HOLDOUT",
        },
        "holdout": {
            "different_run_uuid_and_activation_required": True,
            "different_pair_order": [8, 2, 13, 1, 4],
            "first_polarity": "inverted",
            "primary_model_refit_allowed": False,
            "production_candidate_allowed_from_primary_alone": False,
        },
        "failure_semantics": {
            "missing_or_duplicate_rows_allowed": False,
            "nonfinite_value_is_red": True,
            "physical_launch_or_dispatch_available": False,
            "new_production_gain_may_be_claimed": False,
        },
    }
    contract["contract_semantic_sha256"] = _canonical_sha256(contract)
    return contract


def _finite_curve(value: Any, context: str) -> np.ndarray:
    try:
        curve = np.asarray(value, dtype=np.float64)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{context} 不是数值曲线") from error
    if curve.shape != (PULSE_RESPONSE_ROWS,) or not np.all(np.isfinite(curve)):
        raise ValueError(f"{context} 长度或有限性无效")
    return curve


def _metric(observed: np.ndarray, predicted: np.ndarray) -> dict[str, float]:
    residual = observed - predicted
    return {
        "rmse_px": float(np.sqrt(np.mean(np.square(residual)))),
        "mae_px": float(np.mean(np.abs(residual))),
        "max_abs_error_px": float(np.max(np.abs(residual))),
    }


def _strictly_less(first: float, second: float) -> bool:
    return bool(math.isfinite(first) and math.isfinite(second) and
                first < math.nextafter(second, -math.inf))


def _validate_pulses(pulses: list[dict[str, Any]]) -> None:
    if not isinstance(pulses, list) or len(pulses) != 20:
        raise ValueError("Primary 必须包含 20 个完整 pulse")
    for block_index in range(BLOCK_COUNT):
        pair = pulses[block_index * 2:(block_index + 1) * 2]
        if len(pair) != 2:
            raise ValueError("Primary block 缺完整外出/回锚 pulse")
        polarity_sign = 1 if POLARITIES[block_index] == "normal" else -1
        expected_commands = (
            polarity_sign * AMPLITUDES[block_index],
            -polarity_sign * AMPLITUDES[block_index],
        )
        for pulse_ordinal, (pulse, expected_command) in enumerate(
                zip(pair, expected_commands), 1):
            if not isinstance(pulse, dict) or \
                    pulse.get("block_id") != block_index + 1 or \
                    pulse.get("pair_index") != block_index // 2 + 1 or \
                    pulse.get("role") != ROLES[block_index] or \
                    pulse.get("polarity") != POLARITIES[block_index] or \
                    pulse.get("amplitude_counts") != \
                        AMPLITUDES[block_index] or \
                    pulse.get("pulse_ordinal") != pulse_ordinal or \
                    pulse.get("command_dx_counts") != expected_command:
                raise ValueError("Primary pulse identity/split/order 合同无效")
            for witness in ROIS:
                _finite_curve(
                    pulse.get(f"{witness}_dx_px"),
                    f"block={block_index + 1} pulse={pulse_ordinal} {witness}")


def evaluate_primary(pulses: list[dict[str, Any]],
                     frozen_f1_gains: dict[str, float]) -> dict[str, Any]:
    _validate_pulses(pulses)
    f1_gains: dict[str, float] = {}
    for witness in ROIS:
        value = frozen_f1_gains.get(witness)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or \
                not math.isfinite(float(value)) or float(value) >= 0.0:
            raise ValueError(f"冻结 F1 {witness} gain 无效")
        f1_gains[witness] = float(value)

    estimation = [pulse for pulse in pulses if pulse["role"] == "estimation"]
    confirmation = [pulse for pulse in pulses if pulse["role"] == "confirmation"]
    if len(estimation) != 12 or len(confirmation) != 8:
        raise ValueError("Primary estimation/confirmation whole-pulse split 无效")

    commands = np.asarray(
        [pulse["command_dx_counts"] for pulse in estimation],
        dtype=np.float64)
    denominator = float(np.dot(commands, commands))
    if not math.isfinite(denominator) or denominator <= 0.0:
        raise ValueError("Primary estimation 设计矩阵退化")
    fitted: dict[str, list[float]] = {}
    for witness in ROIS:
        observed = np.vstack([
            _finite_curve(pulse[f"{witness}_dx_px"], witness)
            for pulse in estimation
        ])
        fitted[witness] = (
            np.sum(commands[:, None] * observed, axis=0) /
            denominator).tolist()

    confirmation_rows: list[dict[str, Any]] = []
    all_whole_pulses_pass = True
    for pulse in confirmation:
        command = int(pulse["command_dx_counts"])
        row: dict[str, Any] = {
            key: pulse[key] for key in (
                "block_id", "pair_index", "role", "polarity",
                "amplitude_counts", "pulse_ordinal", "command_dx_counts")
        }
        row["witnesses"] = {}
        row_passes: list[bool] = []
        row_directions: list[bool] = []
        for witness in ROIS:
            observed = _finite_curve(pulse[f"{witness}_dx_px"], witness)
            predicted = np.asarray(fitted[witness]) * command
            f1_prediction = np.zeros(PULSE_RESPONSE_ROWS, dtype=np.float64)
            f1_prediction[F1_DELAY_SAMPLES:] = f1_gains[witness] * command
            model_metric = _metric(observed, predicted)
            f1_metric = _metric(observed, f1_prediction)
            beats_rmse = _strictly_less(
                model_metric["rmse_px"], f1_metric["rmse_px"])
            beats_max = _strictly_less(
                model_metric["max_abs_error_px"],
                f1_metric["max_abs_error_px"])
            opposite = bool(
                command * float(np.mean(observed[F1_DELAY_SAMPLES:])) < 0.0)
            row["witnesses"][witness] = {
                "model": model_metric,
                "frozen_f1": f1_metric,
                "model_strictly_beats_f1_rmse": beats_rmse,
                "model_strictly_beats_f1_max": beats_max,
                "opposite_direction": opposite,
            }
            row_passes.append(beats_rmse and beats_max)
            row_directions.append(opposite)
        row["model_strictly_beats_f1_rmse"] = all(
            value["model_strictly_beats_f1_rmse"]
            for value in row["witnesses"].values())
        row["model_strictly_beats_f1_max"] = all(
            value["model_strictly_beats_f1_max"]
            for value in row["witnesses"].values())
        row["opposite_direction"] = all(row_directions)
        row["whole_pulse_pass"] = all(row_passes) and all(row_directions)
        all_whole_pulses_pass = all_whole_pulses_pass and row["whole_pulse_pass"]
        confirmation_rows.append(row)

    status = ("READY_FOR_COMMAND_MAGNITUDE_HOLDOUT"
              if all_whole_pulses_pass
              else "LINEAR_STEP_RESPONSE_DELETED")
    return {
        "status": status,
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "fit": {
            "model_family": "causal_step_response_diagnostic",
            "input": "backend_completed_relative_command_dx_counts",
            "fit_role": "estimation_whole_pulses_only",
            "fit_amplitudes": [1, 4, 13],
            "confirmation_used_for_refit": False,
            "output_feedback_used": False,
            "left_step_response_px_per_count": fitted["left"],
            "right_step_response_px_per_count": fitted["right"],
        },
        "frozen_f1": {
            "delay_samples": F1_DELAY_SAMPLES,
            "gains": f1_gains,
        },
        "confirmation": confirmation_rows,
        "all_confirmation_whole_pulses_pass": all_whole_pulses_pass,
        "cross_run_holdout_required_before_candidate": True,
    }


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_json(path: pathlib.Path, context: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{context} 无法读取: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{context} 必须是 JSON object")
    return value


def _load_bgr(pixel_root: pathlib.Path, frame: dict[str, Any],
              expected_shape: tuple[int, int, int],
              cache: dict[tuple[str, str, str], np.ndarray]) -> np.ndarray:
    relative = pathlib.PurePath(str(frame.get("file", "")))
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("PNG relative path 非法")
    key = (
        str(relative), str(frame.get("png_sha256", "")),
        str(frame.get("bgr_sha256", "")),
    )
    if key in cache:
        return cache[key]
    path = (pixel_root / relative).resolve()
    if not path.is_relative_to(pixel_root.resolve()) or not path.is_file() or \
            _file_sha256(path) != str(frame.get("png_sha256", "")):
        raise ValueError("PNG path/hash 合同无效")
    bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if bgr is None or bgr.shape != expected_shape:
        raise ValueError("PNG 解码或几何无效")
    if hashlib.sha256(np.ascontiguousarray(bgr).tobytes()).hexdigest() != \
            str(frame.get("bgr_sha256", "")):
        raise ValueError("decoded BGR SHA-256 不匹配")
    cache[key] = bgr
    return bgr


def _measure_primary(root: pathlib.Path) -> tuple[
        list[dict[str, Any]], dict[str, Any], dict[str, float]]:
    if not root.is_absolute() or not root.is_dir():
        raise ValueError("Primary Run 必须是已存在绝对目录")
    paths = {
        "task": root / "task.json",
        "launch": root / "launch-summary.json",
        "sequence": root / "sequence.json",
        "binding": root / "probe-binding.json",
        "report": root / "command-report.json",
        "safety": root / "safety-ledger.json",
        "lifecycle": root / "sidecar-lifecycle.json",
        "manifest": root / "pixel-evidence" / "manifest.json",
        "a2": root / "inputs" / "a2-magnitude-analysis.json",
        "b0": root / "inputs" / "b0-fidelity-evaluation.json",
    }
    task = _load_json(paths["task"], "task")
    launch = _load_json(paths["launch"], "launch summary")
    sequence = _load_json(paths["sequence"], "sequence")
    _validate_sequence_payload(sequence)
    binding = _load_json(paths["binding"], "probe binding")
    report = _load_json(paths["report"], "command report")
    manifest = _load_json(paths["manifest"], "manifest")
    a2 = _load_json(paths["a2"], "A2 magnitude analysis")
    b0 = _load_json(paths["b0"], "B0 fidelity evaluation")
    profile = "physical_b_command_magnitude_primary"
    if task.get("schema_version") != 8 or \
            task.get("evidence_type") != \
                "mouse_effect_probe_b_command_magnitude_task" or \
            task.get("status") != "PREPARED" or \
            task.get("run_role") != "primary" or \
            task.get("profile") != profile or \
            task.get("dispatch_mode") != "physical_b" or \
            task.get("physical_output_capability") is not True or \
            task.get("requires_user_frontend_launch") is not True or \
            task.get("physical_output_confirmation") != \
                "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT" or \
            task.get("sequence_sample_count") != SAMPLE_COUNT or \
            task.get("expected_nonzero_transition_count") != 20 or \
            task.get("max_abs_prefix_x_counts") != 13 or \
            launch.get("schema_version") != 6 or \
            launch.get("evidence_type") != \
                "mouse_effect_probe_b_command_magnitude_launch" or \
            launch.get("status") != "RECORDED_UNANALYZED" or \
            launch.get("profile") != profile or \
            launch.get("run_role") != "primary" or \
            launch.get("activation_epoch") != task.get("activation_epoch") or \
            launch.get("scope_id") != task.get("scope_id") or \
            sequence.get("profile") != profile or \
            report.get("profile") != profile or \
            report.get("dispatch_mode") != "physical_b" or \
            task.get("run_uuid") != report.get("run_uuid") or \
            launch.get("run_uuid") != task.get("run_uuid") or \
            task.get("sequence_sha256") != sequence.get("sequence_sha256") or \
            report.get("sequence_sha256") != sequence.get("sequence_sha256"):
        raise ValueError("Primary task/launch/profile/run/sequence binding 无效")
    dynamics = task.get("dynamics_policy", {})
    safety = task.get("safety", {})
    if dynamics.get("policy_id") != "b-command-magnitude-primary-v1" or \
            dynamics.get("input_definition") != \
                "backend_completed_relative_command_dx_counts" or \
            dynamics.get("primary_estimation_amplitudes") != [1, 4, 13] or \
            dynamics.get("within_run_confirmation_amplitudes") != [2, 8] or \
            dynamics.get("validation_used_for_refit") is not False or \
            dynamics.get("new_production_gain_claimed") is not False or \
            dynamics.get("cross_run_holdout_required_before_candidate") is \
                not True or \
            dynamics.get("fixed_pixel_speed_used_as_gate") is not False or \
            safety.get("zero_y_required") is not True or \
            safety.get("max_abs_pulse_counts") != 13 or \
            safety.get("max_abs_prefix_x_counts") != 13 or \
            safety.get("manual_mouse_motion_or_wasd_forbidden") is not True or \
            safety.get("no_runtime_amplitude_or_repetition_change") is not True:
        raise ValueError("Primary dynamics/safety policy 无效")
    if binding.get("schema_version") != 4 or \
            binding.get("evidence_type") != "mouse_effect_probe_binding" or \
            binding.get("experiment") != profile or \
            binding.get("profile") != profile or \
            binding.get("run_role") != "primary" or \
            binding.get("dispatch_mode") != "physical_b" or \
            binding.get("run_uuid") != task.get("run_uuid") or \
            binding.get("activation_epoch") != task.get("activation_epoch") or \
            binding.get("scope_id") != task.get("scope_id") or \
            binding.get("sequence_sha256") != sequence.get("sequence_sha256") or \
            binding.get("sidecar_physical_output_capability") is not False or \
            binding.get("normal_aim_output_required") is not False or \
            binding.get("dy_counts_required") != 0 or \
            binding.get("max_abs_pulse_counts") != 13 or \
            binding.get("max_abs_prefix_x_counts") != 13 or \
            binding.get("validation_used_for_refit") is not False or \
            binding.get("new_production_gain_claimed") is not False:
        raise ValueError("Primary probe binding 无效")
    file_bindings = task.get("files", {})
    for key, path in (("sequence", paths["sequence"]),
                      ("probe_binding", paths["binding"]),
                      ("a2_magnitude_analysis", paths["a2"]),
                      ("b0_fidelity_evaluation", paths["b0"])):
        identity = file_bindings.get(key, {})
        if int(identity.get("size", -1)) != path.stat().st_size or \
                identity.get("sha256") != _file_sha256(path):
            raise ValueError(f"Primary {key} file identity 无效")
    if a2.get("status") != "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" or \
            a2.get("physical_output_capability") is not False or \
            a2.get("physical_dispatch_count") != 0 or \
            a2.get("production_aim_changed") is not False or \
            a2.get("evaluation", {}).get(
                "new_production_gain_claimed") is not False or \
            b0.get("status") != "BASELINE_REPLAY_FIDELITY_INVALID" or \
            b0.get("physical_output_capability") is not False or \
            b0.get("physical_dispatch_count") != 0 or \
            b0.get("production_aim_changed") is not False:
        raise ValueError("Primary 上游 deletion evidence 合同无效")
    frozen_f1 = a2.get("analysis_contract", {}).get(
        "model", {}).get("frozen_f1_gains", {})

    samples = sequence.get("samples")
    blocks = sequence.get("blocks")
    result = report.get("result", {})
    events = result.get("events")
    frames = manifest.get("frames")
    expected_frames = int(task.get("sidecar", {}).get("frames", -1))
    if manifest.get("evidence_type") != "output_off_capture" or \
            manifest.get("physical_output_capability") is not False or \
            manifest.get("capture_source_name") != \
                task.get("capture", {}).get("source_name") or \
            manifest.get("source_binding", {}).get("sha256") != \
                task.get("files", {}).get("probe_binding", {}).get("sha256") or \
            not isinstance(events, list) or len(events) != SAMPLE_COUNT or \
            result.get("complete") is not True or \
            result.get("stop_reason") != "normal_completion" or \
            result.get("cumulative_requested_x_counts") != 0 or \
            result.get("cumulative_backend_completed_x_counts") != 0 or \
            not isinstance(frames, list) or len(frames) != expected_frames or \
            manifest.get("recorded_frame_count") != expected_frames or \
            launch.get("command_event_count") != SAMPLE_COUNT or \
            launch.get("source_timestamp_matched_event_count") != \
                SAMPLE_COUNT or \
            launch.get("source_timestamp_unmatched_baseline_event_count") != 0 or \
            launch.get("sidecar_frame_count") != expected_frames or \
            launch.get("backend_completed_pulse_count") != 20 or \
            launch.get("requested_net_x_counts") != 0 or \
            launch.get("backend_completed_net_x_counts") != 0 or \
            launch.get("command_report_sha256") != \
                report.get("report_sha256") or \
            launch.get("sidecar_manifest_sha256") != \
                _file_sha256(paths["manifest"]) or \
            launch.get("safety_ledger_sha256") != \
                _file_sha256(paths["safety"]) or \
            launch.get("sidecar_lifecycle_sha256") != \
                _file_sha256(paths["lifecycle"]) or \
            launch.get("png_hash_verified_count") != expected_frames or \
            launch.get("visible_effect_analyzed") is not False or \
            launch.get("human_observation_received") is not False or \
            launch.get("validation_used_for_refit") is not False or \
            launch.get("new_production_gain_claimed") is not False:
        raise ValueError("Primary sample/block/event/frame completion 合同无效")

    frame_by_timestamp: dict[int, dict[str, Any]] = {}
    frame_sessions: set[str] = set()
    for expected_index, frame in enumerate(frames):
        timestamp = int(frame.get("source_timestamp", 0))
        session = str(frame.get("source_clock_session_id", ""))
        if frame.get("index") != expected_index or timestamp <= 0 or \
                timestamp in frame_by_timestamp or \
                frame.get("source_timestamp_valid") is not True or \
                frame.get("source_time_timing_valid") is not True or \
                frame.get("source_clock_status") != "VALID" or \
                not session or \
                int(frame.get("source_dropped_frames", -1)) != 0 or \
                int(frame.get("transport_dropped_frames", -1)) != 0 or \
                int(frame.get("transport_invalid_packets", -1)) != 0:
            raise ValueError("Primary manifest index/source timing 无效")
        frame_by_timestamp[timestamp] = frame
        frame_sessions.add(session)
    if len(frame_sessions) != 1:
        raise ValueError("Primary manifest source clock session 不唯一")

    requested = 0
    completed = 0
    matched: list[dict[str, Any]] = []
    sessions: set[str] = set()
    last_source_timestamp = 0
    last_source_steady_ns = 0
    for index, (sample, event) in enumerate(zip(samples, events)):
        dx = int(sample.get("dx_counts", 999))
        if sample.get("sample_index") != index or \
                int(sample.get("dy_counts", 999)) != 0 or \
                event.get("sample_index") != index or \
                int(event.get("nominal_dx_counts", 999)) != dx or \
                int(event.get("nominal_dy_counts", 999)) != 0 or \
                int(event.get("requested_dy_counts", 999)) != 0 or \
                event.get("safety_allowed") is not True or \
                int(event.get("source_dropped_frames", -1)) != 0 or \
                int(event.get("transport_dropped_frames", -1)) != 0 or \
                int(event.get("transport_invalid_packets", -1)) != 0:
            raise ValueError("Primary event/sample X-only identity 无效")
        if dx:
            if int(event.get("requested_dx_counts", 999)) != dx or \
                    event.get("dispatch_attempted") is not True or \
                    event.get("backend_succeeded") is not True or \
                    event.get("protocol_ack_received") is not True:
                raise ValueError("Primary nonzero event 未完成/ACK")
            requested += dx
            completed += dx
        elif int(event.get("requested_dx_counts", 999)) != 0 or \
                event.get("dispatch_attempted") is not False or \
                event.get("backend_succeeded") is not False or \
                event.get("protocol_ack_received") is not False:
            raise ValueError("Primary zero event 不得 dispatch")
        if int(event.get("cumulative_requested_x_counts", 999)) != requested or \
                int(event.get(
                    "cumulative_backend_completed_x_counts", 999)) != completed:
            raise ValueError("Primary cumulative command ledger 无效")
        timestamp = int(event.get("source_timestamp", 0))
        steady_ns = int(event.get("source_time_at_steady_ns", 0))
        frame = frame_by_timestamp.get(timestamp)
        session = str(event.get("source_clock_session_id", ""))
        if event.get("source_timestamp_valid") is not True or \
                event.get("source_clock_status") != "VALID" or \
                timestamp <= last_source_timestamp or \
                steady_ns <= last_source_steady_ns or \
                frame is None or not session or \
                frame.get("source_clock_session_id") != session:
            raise ValueError("Primary event/frame exact join 或 session 无效")
        matched.append(frame)
        sessions.add(session)
        last_source_timestamp = timestamp
        last_source_steady_ns = steady_ns
    if requested != 0 or completed != 0 or len(sessions) != 1 or \
            sessions != frame_sessions:
        raise ValueError("Primary net-zero/source session 合同无效")

    height = int(task.get("capture", {}).get("roi_height", 0))
    width = int(task.get("capture", {}).get("roi_width", 0))
    if (width, height) != (320, 320) or \
            task.get("sidecar", {}).get("left_witness_roi") != \
                ",".join(map(str, ROIS["left"])) or \
            task.get("sidecar", {}).get("right_witness_roi") != \
                ",".join(map(str, ROIS["right"])):
        raise ValueError("Primary witness ROI scope 无效")
    pixel_root = paths["manifest"].parent
    cache: dict[tuple[str, str, str], np.ndarray] = {}
    windows = {
        witness: cv2.createHanningWindow((roi[2], roi[3]), cv2.CV_32F)
        for witness, roi in ROIS.items()
    }
    pulses: list[dict[str, Any]] = []
    for block_index, block in enumerate(blocks):
        first = BASELINE_COUNT + block_index * BLOCK_SAMPLE_COUNT
        amplitude = AMPLITUDES[block_index]
        polarity = POLARITIES[block_index]
        role = ROLES[block_index]
        if block.get("block_id") != block_index + 1 or \
                block.get("pair_index") != block_index // 2 + 1 or \
                block.get("role") != role or \
                block.get("polarity") != polarity or \
                block.get("amplitude_counts") != amplitude or \
                block.get("first_sample_index") != first or \
                block.get("sample_count") != BLOCK_SAMPLE_COUNT:
            raise ValueError("Primary block order/split/amplitude 无效")
        pre_frames = matched[first:first + GUARD_COUNT]
        pre_indices = [int(frame["index"]) for frame in pre_frames]
        if pre_indices != list(range(pre_indices[0],
                                     pre_indices[0] + GUARD_COUNT)):
            raise ValueError("Primary block pre-guard frames 不连续")
        first_sign = 1 if polarity == "normal" else -1
        for pulse_ordinal, (pulse_index, command) in enumerate((
                (first + GUARD_COUNT, first_sign * amplitude),
                (first + GUARD_COUNT + 1 + RESPONSE_COUNT,
                 -first_sign * amplitude)), 1):
            if int(samples[pulse_index].get("dx_counts", 0)) != command:
                raise ValueError("Primary pulse/return exact index 无效")
            response_frames = matched[
                pulse_index:pulse_index + PULSE_RESPONSE_ROWS]
            indices = [int(frame["index"]) for frame in response_frames]
            anchor_frame = matched[pulse_index - 1]
            if indices != list(range(indices[0],
                                     indices[0] + PULSE_RESPONSE_ROWS)) or \
                    int(anchor_frame["index"]) + 1 != indices[0]:
                raise ValueError("Primary pulse response frames 不连续")
            row: dict[str, Any] = {
                "block_id": block_index + 1,
                "pair_index": block_index // 2 + 1,
                "role": role,
                "polarity": polarity,
                "amplitude_counts": amplitude,
                "pulse_ordinal": pulse_ordinal,
                "command_dx_counts": command,
                "source_timestamp": int(
                    response_frames[0]["source_timestamp"]),
                "manifest_first_index": indices[0],
                "manifest_last_index": indices[-1],
            }
            anchor_bgr = _load_bgr(
                pixel_root, anchor_frame, (height, width, 3), cache)
            anchor_gray = cv2.cvtColor(
                anchor_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
            pre_anchor_bgr = _load_bgr(
                pixel_root, pre_frames[-1], (height, width, 3), cache)
            pre_anchor_gray = cv2.cvtColor(
                pre_anchor_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
            for witness, roi in ROIS.items():
                x, y, roi_width, roi_height = roi
                hann = windows[witness]
                pre_anchor = np.ascontiguousarray(
                    pre_anchor_gray[y:y + roi_height, x:x + roi_width])
                pre_dx: list[float] = []
                for frame in pre_frames:
                    bgr = _load_bgr(
                        pixel_root, frame, (height, width, 3), cache)
                    gray = cv2.cvtColor(
                        bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
                    current = np.ascontiguousarray(
                        gray[y:y + roi_height, x:x + roi_width])
                    shift, _ = cv2.phaseCorrelate(
                        pre_anchor.copy(), current.copy(), hann)
                    pre_dx.append(float(shift[0]))
                design = np.column_stack((
                    np.ones(GUARD_COUNT, dtype=np.float64),
                    np.arange(-GUARD_COUNT + 1, 1, dtype=np.float64),
                ))
                nuisance = np.linalg.lstsq(
                    design, np.asarray(pre_dx), rcond=None)[0]
                pulse_anchor = np.ascontiguousarray(
                    anchor_gray[y:y + roi_height, x:x + roi_width])
                raw_dx: list[float] = []
                phase_response: list[float] = []
                for frame in response_frames:
                    bgr = _load_bgr(
                        pixel_root, frame, (height, width, 3), cache)
                    gray = cv2.cvtColor(
                        bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
                    current = np.ascontiguousarray(
                        gray[y:y + roi_height, x:x + roi_width])
                    shift, response = cv2.phaseCorrelate(
                        pulse_anchor.copy(), current.copy(), hann)
                    values = (float(shift[0]), float(response))
                    if not all(math.isfinite(value) for value in values):
                        raise ValueError("Primary phaseCorrelate 非有限")
                    raw_dx.append(values[0])
                    phase_response.append(values[1])
                offsets = np.arange(
                    1, PULSE_RESPONSE_ROWS + 1, dtype=np.float64)
                cleaned = np.asarray(raw_dx) - nuisance[1] * offsets
                row[f"{witness}_dx_px"] = cleaned.tolist()
                row[f"{witness}_raw_dx_px"] = raw_dx
                row[f"{witness}_phase_response"] = phase_response
                row[f"{witness}_pre_guard_slope_px_per_frame"] = \
                    float(nuisance[1])
            pulses.append(row)

    identity = {
        "run_uuid": task["run_uuid"],
        "activation_epoch": task["activation_epoch"],
        "scope_id": task["scope_id"],
        "source_clock_session_id": next(iter(sessions)),
        "sequence_sha256": sequence["sequence_sha256"],
        "task_sha256": _file_sha256(paths["task"]),
        "launch_summary_sha256": _file_sha256(paths["launch"]),
        "sequence_file_sha256": _file_sha256(paths["sequence"]),
        "command_report_sha256": _file_sha256(paths["report"]),
        "safety_ledger_sha256": _file_sha256(paths["safety"]),
        "sidecar_lifecycle_sha256": _file_sha256(paths["lifecycle"]),
        "manifest_sha256": _file_sha256(paths["manifest"]),
        "a2_magnitude_analysis_sha256": _file_sha256(paths["a2"]),
        "b0_fidelity_evaluation_sha256": _file_sha256(paths["b0"]),
        "physical_output_was_historical_input": True,
        "physical_output_dispatched_by_this_analysis": False,
    }
    return pulses, identity, frozen_f1


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if not path.is_absolute() or path.exists() or pending.exists():
        raise ValueError("分析输出必须是尚不存在的绝对路径")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with pending.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2,
                      allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        pending.replace(path)
    finally:
        if pending.exists():
            pending.unlink()


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="分析 Physical B command-magnitude Primary")
    parser.add_argument("--run-directory", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args(arguments)
    try:
        pulses, identity, frozen_f1 = _measure_primary(
            options.run_directory.resolve())
        evaluation = evaluate_primary(pulses, frozen_f1)
        output: dict[str, Any] = {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_b_command_magnitude_primary_analysis",
            "status": evaluation["status"],
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "production_aim_changed": False,
            "new_production_gain_claimed": False,
            "analysis_contract": analysis_contract(),
            "primary": identity,
            "evaluation": evaluation,
            "pulse_measurements": pulses,
        }
        output["analysis_semantic_sha256"] = _canonical_sha256(output)
        _write_json_atomic(options.output, output)
    except (OSError, UnicodeError, ValueError, KeyError, TypeError,
            cv2.error) as error:
        print(f"command-magnitude analysis failed: {error}", file=sys.stderr)
        return 2
    print(
        "command-magnitude analysis: "
        f"status={evaluation['status']}, physical_output_capability=false, "
        "physical_dispatch_count=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
