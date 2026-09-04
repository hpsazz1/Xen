#!/usr/bin/env python3
"""用已录制 A2 ±16 活性 Run 检查 Physical B F1 的幅度域外推。"""

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


SAMPLE_COUNT = 1024
CHALLENGE_STARTS = (0, 832)
CHALLENGE_SAMPLE_COUNT = 192
PRE_GUARD_FRAME_COUNT = 32
DELAY_SAMPLES = 4
DELAY_CONTROLS = tuple(range(1, 9))
ROIS = {
    "left": (16, 48, 96, 224),
    "right": (208, 48, 96, 224),
}
FROZEN_F1_GAINS = {
    "left": -0.38771113363699733,
    "right": -0.40635479906015076,
}
EXPECTED_PROFILES = {
    "primary": "dependency_calibration_a2_s1_primary",
    "validation": "dependency_calibration_a2_s1_validation",
}


def canonical_semantic_sha256(
        document: dict[str, Any], excluded_field: str) -> str:
    payload = {
        key: value for key, value in document.items()
        if key != excluded_field
    }
    encoded = json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def analysis_contract() -> dict[str, Any]:
    contract: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_a2_magnitude_domain_contract",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "measurement": {
            "decode": "opencv_imread_color_bgr",
            "gray": "opencv_bgr2gray_float32",
            "translation":
                "opencv_phase_correlate_hann_current_relative_to_anchor",
            "anchor": "last_exact_frame_before_challenge",
            "nuisance":
                "constant_plus_linear_fit_on_32_exact_pre_frames_only",
            "left_roi": list(ROIS["left"]),
            "right_roi": list(ROIS["right"]),
            "source_join": "exact_int64_ndi_submission_timestamp",
            "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
        },
        "model": {
            "family": "delayed_static_gain_diagnostic",
            "input": "backend_completed_cumulative_position_counts",
            "delay_samples": DELAY_SAMPLES,
            "delay_controls": list(DELAY_CONTROLS),
            "output_feedback_used": False,
            "frozen_f1_gains": dict(FROZEN_F1_GAINS),
            "dtype": "float64",
            "solver": "least_squares_through_origin_after_frozen_nuisance",
        },
        "split": {
            "fit": "primary_whole_blocks_only",
            "evaluation": "validation_whole_blocks_only",
            "random_frame_split_allowed": False,
        },
        "deletion_tests": {
            "delay_4_best_on_both_primary_witnesses": True,
            "primary_model_strictly_beats_f1_rmse_on_every_validation_block":
                True,
            "primary_model_strictly_beats_f1_max_on_every_validation_block":
                True,
            "aggregate_masking_allowed": False,
            "new_production_gain_may_be_claimed": False,
        },
        "failure_semantics": {
            "missing_or_duplicate_rows_allowed": False,
            "unknown_prehistory_zero_padding_allowed": False,
            "nonfinite_value_is_red": True,
            "physical_launch_or_dispatch_available": False,
        },
    }
    contract["contract_semantic_sha256"] = canonical_semantic_sha256(
        contract, "contract_semantic_sha256")
    return contract


def _finite_array(values: Any, context: str) -> np.ndarray:
    array = np.asarray(values, dtype=np.float64)
    if array.ndim != 1 or not np.all(np.isfinite(array)):
        raise ValueError(f"{context} 必须是一维有限数值")
    return array


def _delayed_position(position: np.ndarray, delay: int) -> np.ndarray:
    if delay <= 0 or delay >= position.size:
        raise ValueError("delay 超出 block 范围")
    return np.concatenate((
        np.zeros(delay, dtype=np.float64),
        position[:-delay],
    ))


def _metric(observed: np.ndarray, predicted: np.ndarray) -> dict[str, float]:
    residual = observed - predicted
    return {
        "rmse_px": float(np.sqrt(np.mean(residual * residual))),
        "mae_px": float(np.mean(np.abs(residual))),
        "max_abs_error_px": float(np.max(np.abs(residual))),
    }


def _validate_blocks(blocks: list[dict[str, Any]], role: str) -> None:
    if len(blocks) != 2:
        raise ValueError(f"{role} 必须恰有两个正负 whole blocks")
    identities: set[str] = set()
    signs: set[int] = set()
    for block in blocks:
        block_id = str(block.get("block_id", ""))
        if not block_id or block_id in identities:
            raise ValueError(f"{role} block identity 缺失或重复")
        identities.add(block_id)
        position = _finite_array(
            block.get("position_x_counts"), f"{block_id} position")
        if position.size != CHALLENGE_SAMPLE_COUNT or \
                not np.all(position == np.rint(position)) or \
                np.max(np.abs(position)) != 16.0 or position[-1] != 0.0 or \
                set(np.abs(position).astype(int)) != set(range(17)):
            raise ValueError(f"{block_id} 未完整覆盖 0..16 count 并回零")
        nonzero = position[position != 0.0]
        sign_values = set(np.sign(nonzero).astype(int))
        if len(sign_values) != 1:
            raise ValueError(f"{block_id} 必须是单极性 challenge")
        signs.update(sign_values)
        witnesses = block.get("witness_dx_px")
        if not isinstance(witnesses, dict) or set(witnesses) != set(ROIS):
            raise ValueError(f"{block_id} witness 集合无效")
        for witness in ROIS:
            output = _finite_array(
                witnesses[witness], f"{block_id} {witness} output")
            if output.size != CHALLENGE_SAMPLE_COUNT:
                raise ValueError(f"{block_id} {witness} 缺 sample")
    if signs != {-1, 1}:
        raise ValueError(f"{role} 必须同时覆盖正负方向")


def _fit_gain(blocks: list[dict[str, Any]], witness: str,
              delay: int) -> tuple[float, dict[str, float]]:
    inputs = []
    outputs = []
    for block in blocks:
        position = _finite_array(
            block["position_x_counts"], "position")
        inputs.append(_delayed_position(position, delay))
        outputs.append(_finite_array(
            block["witness_dx_px"][witness], "output"))
    input_vector = np.concatenate(inputs)
    output_vector = np.concatenate(outputs)
    denominator = float(input_vector @ input_vector)
    if denominator <= 0.0:
        raise ValueError("幅度域输入退化")
    gain = float(input_vector @ output_vector / denominator)
    return gain, _metric(output_vector, gain * input_vector)


def _strictly_less(first: float, second: float, rows: int) -> bool:
    tolerance = (
        max(1.0, abs(first), abs(second)) *
        np.finfo(np.float64).eps * max(1, rows)
    )
    return bool(first < second - tolerance)


def evaluate_domain(
        primary_blocks: list[dict[str, Any]],
        validation_blocks: list[dict[str, Any]]) -> dict[str, Any]:
    _validate_blocks(primary_blocks, "primary")
    _validate_blocks(validation_blocks, "validation")

    delay_controls: dict[str, dict[str, dict[str, float] | float]] = {}
    for delay in DELAY_CONTROLS:
        entry: dict[str, dict[str, float] | float] = {}
        for witness in ROIS:
            gain, metrics = _fit_gain(primary_blocks, witness, delay)
            entry[witness] = {"gain": gain, **metrics}
        delay_controls[str(delay)] = entry

    invalid_reasons: list[str] = []
    for witness in ROIS:
        selected_rmse = float(
            delay_controls[str(DELAY_SAMPLES)][witness]["rmse_px"])
        if any(not _strictly_less(
                selected_rmse,
                float(delay_controls[str(delay)][witness]["rmse_px"]),
                len(primary_blocks) * CHALLENGE_SAMPLE_COUNT)
               for delay in DELAY_CONTROLS if delay != DELAY_SAMPLES):
            invalid_reasons.append("DELAY_4_NOT_BEST_ON_PRIMARY")
            break

    primary_model: dict[str, Any] = {
        "delay_samples": DELAY_SAMPLES,
        "fit_role": "primary_whole_blocks_only",
        "validation_used_for_refit": False,
    }
    validation: dict[str, Any] = {}
    all_blocks_delete_f1 = True
    for witness in ROIS:
        gain = float(
            delay_controls[str(DELAY_SAMPLES)][witness]["gain"])
        primary_model[f"{witness}_gain"] = gain
        primary_model[f"{witness}_fit_metrics"] = {
            key: value for key, value in
            delay_controls[str(DELAY_SAMPLES)][witness].items()
            if key != "gain"
        }
        if gain >= 0.0:
            invalid_reasons.append("PRIMARY_GAIN_DIRECTION_INVALID")
        blocks = []
        all_observed = []
        all_model = []
        all_f1 = []
        for block in validation_blocks:
            position = _finite_array(
                block["position_x_counts"], "validation position")
            delayed = _delayed_position(position, DELAY_SAMPLES)
            observed = _finite_array(
                block["witness_dx_px"][witness], "validation output")
            model_prediction = gain * delayed
            f1_prediction = FROZEN_F1_GAINS[witness] * delayed
            model_metrics = _metric(observed, model_prediction)
            f1_metrics = _metric(observed, f1_prediction)
            rmse_better = _strictly_less(
                model_metrics["rmse_px"], f1_metrics["rmse_px"],
                CHALLENGE_SAMPLE_COUNT)
            max_better = _strictly_less(
                model_metrics["max_abs_error_px"],
                f1_metrics["max_abs_error_px"],
                CHALLENGE_SAMPLE_COUNT)
            all_blocks_delete_f1 = (
                all_blocks_delete_f1 and rmse_better and max_better)
            blocks.append({
                "block_id": block["block_id"],
                "sample_count": CHALLENGE_SAMPLE_COUNT,
                "primary_model_rmse_px": model_metrics["rmse_px"],
                "primary_model_mae_px": model_metrics["mae_px"],
                "primary_model_max_abs_error_px":
                    model_metrics["max_abs_error_px"],
                "frozen_f1_rmse_px": f1_metrics["rmse_px"],
                "frozen_f1_mae_px": f1_metrics["mae_px"],
                "frozen_f1_max_abs_error_px":
                    f1_metrics["max_abs_error_px"],
                "primary_model_strictly_better_rmse": rmse_better,
                "primary_model_strictly_better_max": max_better,
            })
            all_observed.append(observed)
            all_model.append(model_prediction)
            all_f1.append(f1_prediction)
        validation[witness] = {
            "blocks": blocks,
            "aggregate_diagnostic": {
                "primary_model": _metric(
                    np.concatenate(all_observed),
                    np.concatenate(all_model)),
                "frozen_f1": _metric(
                    np.concatenate(all_observed),
                    np.concatenate(all_f1)),
                "may_override_whole_block_result": False,
            },
        }
    if not all_blocks_delete_f1:
        invalid_reasons.append("F1_NOT_DELETED_ON_EVERY_VALIDATION_BLOCK")

    invalid_reasons = list(dict.fromkeys(invalid_reasons))
    deleted = not invalid_reasons
    return {
        "status": "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN"
            if deleted else "A2_MAGNITUDE_DOMAIN_UNRESOLVED",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "f1_deleted_for_magnitude_domain": deleted,
        "new_production_gain_claimed": False,
        "invalid_reasons": invalid_reasons,
        "primary_model": primary_model,
        "primary_delay_controls": delay_controls,
        "validation": validation,
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
              cache: dict[int, np.ndarray]) -> np.ndarray:
    index = int(frame.get("index", -1))
    if index in cache:
        return cache[index]
    relative = pathlib.PurePath(str(frame.get("file", "")))
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("PNG relative path 非法")
    path = (pixel_root / relative).resolve()
    if not path.is_relative_to(pixel_root.resolve()) or not path.is_file() or \
            _file_sha256(path) != str(frame.get("png_sha256", "")):
        raise ValueError("PNG path/hash 合同无效")
    bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if bgr is None or bgr.shape != (320, 320, 3):
        raise ValueError("PNG 解码或几何无效")
    bgr_hash = hashlib.sha256(
        np.ascontiguousarray(bgr).tobytes()).hexdigest()
    if bgr_hash != str(frame.get("bgr_sha256", "")):
        raise ValueError("decoded BGR SHA-256 不匹配")
    cache[index] = bgr
    return bgr


def _expected_positions(events: list[dict[str, Any]], start: int) -> list[int]:
    previous = int(events[start - 1]["cumulative_backend_completed_x_counts"]) \
        if start else 0
    return [
        int(event["cumulative_backend_completed_x_counts"]) - previous
        for event in events[start:start + CHALLENGE_SAMPLE_COUNT]
    ]


def _measure_run(root: pathlib.Path, role: str) -> tuple[
        list[dict[str, Any]], dict[str, Any], list[dict[str, Any]]]:
    if not root.is_absolute() or not root.is_dir():
        raise ValueError(f"{role} Run 必须是已存在绝对目录")
    paths = {
        "task": root / "task.json",
        "sequence": root / "sequence.json",
        "command_report": root / "command-report.json",
        "manifest": root / "pixel-evidence" / "manifest.json",
    }
    task = _load_json(paths["task"], f"{role} task")
    sequence = _load_json(paths["sequence"], f"{role} sequence")
    report = _load_json(paths["command_report"], f"{role} command report")
    manifest = _load_json(paths["manifest"], f"{role} manifest")
    expected_profile = EXPECTED_PROFILES[role]
    if task.get("run_role") != role or task.get("profile") != expected_profile or \
            sequence.get("profile") != expected_profile or \
            report.get("profile") != expected_profile or \
            task.get("run_uuid") != report.get("run_uuid") or \
            task.get("sequence_sha256") != sequence.get("sequence_sha256") or \
            report.get("sequence_sha256") != sequence.get("sequence_sha256"):
        raise ValueError(f"{role} task/profile/run/sequence binding 无效")
    if str(task.get("sidecar", {}).get("left_witness_roi")) != \
            ",".join(map(str, ROIS["left"])) or \
            str(task.get("sidecar", {}).get("right_witness_roi")) != \
            ",".join(map(str, ROIS["right"])):
        raise ValueError(f"{role} witness ROI 与 F1 不一致")
    sequence_identity = task.get("files", {}).get("sequence", {})
    if int(sequence_identity.get("size", -1)) != paths["sequence"].stat().st_size or \
            str(sequence_identity.get("sha256", "")) != \
            _file_sha256(paths["sequence"]):
        raise ValueError(f"{role} sequence file identity 无效")
    result = report.get("result", {})
    samples = sequence.get("samples")
    events = result.get("events")
    frames = manifest.get("frames")
    if result.get("complete") is not True or \
            result.get("stop_reason") != "normal_completion" or \
            not isinstance(samples, list) or len(samples) != SAMPLE_COUNT or \
            not isinstance(events, list) or len(events) != SAMPLE_COUNT or \
            not isinstance(frames, list) or len(frames) != 2400 or \
            manifest.get("recorded_frame_count") != 2400:
        raise ValueError(f"{role} sample/event/frame completion 合同无效")

    frame_by_timestamp: dict[int, dict[str, Any]] = {}
    for expected_index, frame in enumerate(frames):
        timestamp = int(frame.get("source_timestamp", 0))
        if int(frame.get("index", -1)) != expected_index or timestamp <= 0 or \
                timestamp in frame_by_timestamp:
            raise ValueError(f"{role} manifest index/timestamp 无效")
        frame_by_timestamp[timestamp] = frame
    requested = 0
    completed = 0
    matched: list[dict[str, Any]] = []
    sessions: set[str] = set()
    for index, (sample, event) in enumerate(zip(samples, events)):
        dx = int(sample.get("dx_counts", 999))
        dy = int(sample.get("dy_counts", 999))
        if int(sample.get("sample_index", -1)) != index or dy != 0 or \
                int(event.get("sample_index", -1)) != index or \
                int(event.get("nominal_dx_counts", 999)) != dx or \
                int(event.get("nominal_dy_counts", 999)) != 0 or \
                int(event.get("requested_dy_counts", 999)) != 0:
            raise ValueError(f"{role} event/sample X-only identity 无效")
        if dx:
            if int(event.get("requested_dx_counts", 999)) != dx or \
                    event.get("backend_succeeded") is not True or \
                    event.get("protocol_ack_received") is not True:
                raise ValueError(f"{role} nonzero event 未完成/ACK")
            requested += dx
            completed += dx
        elif int(event.get("requested_dx_counts", 999)) != 0 or \
                event.get("dispatch_attempted") is not False:
            raise ValueError(f"{role} zero event 不得 dispatch")
        if int(event.get("cumulative_requested_x_counts", 999)) != requested or \
                int(event.get(
                    "cumulative_backend_completed_x_counts", 999)) != completed:
            raise ValueError(f"{role} cumulative command ledger 无效")
        timestamp = int(event.get("source_timestamp", 0))
        frame = frame_by_timestamp.get(timestamp)
        session = str(event.get("source_clock_session_id", ""))
        if frame is None or not session or \
                str(frame.get("source_clock_session_id", "")) != session:
            raise ValueError(f"{role} event/frame exact join 或 session 无效")
        sessions.add(session)
        matched.append(frame)
    if requested != 0 or completed != 0 or len(sessions) != 1:
        raise ValueError(f"{role} net-zero 或 source session 无效")

    blocks: list[dict[str, Any]] = []
    sample_rows: list[dict[str, Any]] = []
    cache: dict[int, np.ndarray] = {}
    pixel_root = paths["manifest"].parent
    windows = {
        witness: cv2.createHanningWindow(
            (roi[2], roi[3]), cv2.CV_32F)
        for witness, roi in ROIS.items()
    }
    for challenge_number, start in enumerate(CHALLENGE_STARTS, 1):
        event_frames = matched[start:start + CHALLENGE_SAMPLE_COUNT]
        indices = [int(frame["index"]) for frame in event_frames]
        if indices != list(range(indices[0], indices[0] +
                                 CHALLENGE_SAMPLE_COUNT)):
            raise ValueError(f"{role} challenge event frames 不连续")
        anchor_index = indices[0] - 1
        pre_indices = list(range(
            anchor_index - PRE_GUARD_FRAME_COUNT + 1,
            anchor_index + 1))
        if pre_indices[0] < 0:
            raise ValueError(f"{role} challenge 缺 exact pre frames")
        positions = _expected_positions(events, start)
        block_id = f"{role.upper()}-CHALLENGE-{challenge_number}"
        outputs: dict[str, list[float]] = {}
        anchor_frame = frames[anchor_index]
        anchor_bgr = _load_bgr(pixel_root, anchor_frame, cache)
        anchor_gray = cv2.cvtColor(
            anchor_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
        witness_measurements: dict[str, tuple[np.ndarray, np.ndarray]] = {}
        for witness, roi in ROIS.items():
            x, y, width, height = roi
            anchor = np.ascontiguousarray(
                anchor_gray[y:y + height, x:x + width])
            pre_dx = []
            for manifest_index in pre_indices:
                bgr = _load_bgr(pixel_root, frames[manifest_index], cache)
                gray = cv2.cvtColor(
                    bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
                current = np.ascontiguousarray(
                    gray[y:y + height, x:x + width])
                shift, _ = cv2.phaseCorrelate(
                    anchor.copy(), current.copy(), windows[witness])
                pre_dx.append(float(shift[0]))
            design = np.column_stack((
                np.ones(PRE_GUARD_FRAME_COUNT, dtype=np.float64),
                np.arange(
                    -PRE_GUARD_FRAME_COUNT + 1, 1,
                    dtype=np.float64),
            ))
            nuisance = np.linalg.lstsq(
                design, np.asarray(pre_dx), rcond=None)[0]
            raw_dx = []
            raw_dy = []
            responses = []
            for frame in event_frames:
                bgr = _load_bgr(pixel_root, frame, cache)
                gray = cv2.cvtColor(
                    bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
                current = np.ascontiguousarray(
                    gray[y:y + height, x:x + width])
                shift, response = cv2.phaseCorrelate(
                    anchor.copy(), current.copy(), windows[witness])
                values = (float(shift[0]), float(shift[1]), float(response))
                if not all(math.isfinite(value) for value in values):
                    raise ValueError(f"{role} phaseCorrelate 非有限")
                raw_dx.append(values[0])
                raw_dy.append(values[1])
                responses.append(values[2])
            offsets = np.arange(
                1, CHALLENGE_SAMPLE_COUNT + 1, dtype=np.float64)
            cleaned = np.asarray(raw_dx) - (
                nuisance[0] + nuisance[1] * offsets)
            outputs[witness] = cleaned.tolist()
            witness_measurements[witness] = (
                np.column_stack((raw_dx, raw_dy, responses)), nuisance)
        delayed = _delayed_position(
            np.asarray(positions, dtype=np.float64), DELAY_SAMPLES)
        for offset, frame in enumerate(event_frames):
            row: dict[str, Any] = {
                "run_role": role,
                "block_id": block_id,
                "sample_index_in_challenge": offset,
                "source_timestamp": int(frame["source_timestamp"]),
                "manifest_index": int(frame["index"]),
                "position_x_counts": positions[offset],
                "delayed_position_x_counts": int(delayed[offset]),
            }
            for witness in ROIS:
                values, nuisance = witness_measurements[witness]
                row[f"{witness}_dx_px"] = float(values[offset, 0])
                row[f"{witness}_dy_px"] = float(values[offset, 1])
                row[f"{witness}_phase_response"] = float(values[offset, 2])
                row[f"{witness}_nuisance_intercept_px"] = float(nuisance[0])
                row[f"{witness}_nuisance_slope_px_per_frame"] = \
                    float(nuisance[1])
                row[f"{witness}_clean_dx_px"] = outputs[witness][offset]
            sample_rows.append(row)
        blocks.append({
            "block_id": block_id,
            "position_x_counts": positions,
            "witness_dx_px": outputs,
        })
    identity = {
        "role": role,
        "run_uuid": task["run_uuid"],
        "activation_epoch": task["activation_epoch"],
        "scope_id": task["scope_id"],
        "source_clock_session_id": next(iter(sessions)),
        "sequence_sha256": sequence["sequence_sha256"],
        "task_sha256": _file_sha256(paths["task"]),
        "sequence_file_sha256": _file_sha256(paths["sequence"]),
        "command_report_sha256": _file_sha256(paths["command_report"]),
        "manifest_sha256": _file_sha256(paths["manifest"]),
        "physical_output_was_historical_input": True,
        "physical_output_dispatched_by_this_analysis": False,
    }
    return blocks, identity, sample_rows


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if not path.is_absolute() or path.exists() or pending.exists():
        raise ValueError("分析输出必须是尚不存在的绝对路径")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with pending.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(
                value, stream, ensure_ascii=False, indent=2,
                allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(pending, path)
    finally:
        if pending.exists():
            pending.unlink()


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "只读重算既有 A2 ±16 Run 的 F1 witness 幅度域；"
            "不打开 Mouse/KMBOX，不修改生产 Aim。"))
    parser.add_argument("--primary-run", required=True, type=pathlib.Path)
    parser.add_argument("--validation-run", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args(arguments)
    try:
        primary, primary_identity, primary_rows = _measure_run(
            options.primary_run.resolve(), "primary")
        validation, validation_identity, validation_rows = _measure_run(
            options.validation_run.resolve(), "validation")
        if primary_identity["run_uuid"] == validation_identity["run_uuid"] or \
                primary_identity["activation_epoch"] == \
                validation_identity["activation_epoch"] or \
                primary_identity["scope_id"] != validation_identity["scope_id"]:
            raise ValueError("Primary/Validation independence 或 scope 无效")
        evaluation = evaluate_domain(primary, validation)
        output = {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_a2_magnitude_domain_analysis",
            "status": evaluation["status"],
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "production_aim_changed": False,
            "analysis_contract": analysis_contract(),
            "primary": primary_identity,
            "validation": validation_identity,
            "evaluation": evaluation,
            "samples": primary_rows + validation_rows,
        }
        output["analysis_semantic_sha256"] = canonical_semantic_sha256(
            output, "analysis_semantic_sha256")
        _write_json_atomic(options.output, output)
    except (OSError, UnicodeError, ValueError, cv2.error) as error:
        print(f"A2 magnitude domain analysis failed: {error}", file=sys.stderr)
        return 2
    print(
        "A2 magnitude domain analysis: "
        f"status={output['status']}, "
        "physical_output_capability=false, physical_dispatch_count=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
