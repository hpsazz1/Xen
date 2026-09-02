#!/usr/bin/env python3
"""Physical B Primary 的预注册整块 FIR 选择与 F1 证据生成。"""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import io
import json
import math
import os
import pathlib
import sys
from typing import Iterable, Sequence

import cv2
import numpy as np


_METRIC_ROUND_DECIMAL_PLACES = 12
_EXPECTED_PRIMARY_SEQUENCE_SHA256 = (
    "2132219c011c0aab75b30c246c37375496a46b4cd83b2455d9756c2f9c9c31e2"
)


def canonical_semantic_sha256(value: dict, field: str) -> str:
    payload = dict(value)
    payload.pop(field, None)
    return hashlib.sha256(json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")).hexdigest()


def physical_b_analysis_contract() -> dict:
    """返回数据到来前必须冻结的 Primary/F1 分析语义。"""
    contract = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_analysis_contract",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "preprocessing": {
            "decode": "opencv_imread_color_bgr",
            "witness_gray": "opencv_bgr2gray_float32",
            "translation": "opencv_phase_correlate_hann_current_relative_to_block_anchor",
            "anchor": "last_exact_pre_guard_source_frame",
            "output_rows": "complete_period_return_plus_exact_post_guard",
            "source_join": "exact_int64_ndi_submission_timestamp",
            "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
            "missing_or_duplicate_join_allowed": False,
        },
        "model": {
            "family": "strictly_causal_fir",
            "identification_input": "cumulative_position_counts",
            "actuator_audit_input": "completed_command_dx_counts",
            "outputs": ["left_witness_dx_px", "right_witness_dx_px"],
            "lag_origin": 1,
            "output_feedback_used": False,
            "dtype": "float64",
            "column_scaling": "none",
            "solver": "svd_minimum_norm_full_column_rank",
            "rank_tolerance": "max(rows,cols)*sigma_max*float64_eps",
            "nuisance_columns_per_block": [
                "constant_1", "linear_block_fraction_0_to_1"
            ],
        },
        "candidate_horizons": [4, 8, 16, 32],
        "deletion_control_horizons": [4],
        "acceptance_eligible_horizons": [8, 16, 32],
        "split": {
            "estimation_pair_index": 1,
            "within_run_validation_pair_index": 2,
            "whole_block_only": True,
            "random_frame_split_allowed": False,
            "rows_may_cross_block_boundary": False,
        },
        "validation": {
            "input_forced_required": True,
            "output_free_run_required": True,
            "output_free_run_equivalent_for_fir": True,
            "validation_nuisance_rule": (
                "fit_only_frozen_constant_and_linear_columns_per_whole_block; "
                "never_refit_fir_coefficients"
            ),
            "metrics": [
                "rmse_px", "mae_px", "max_abs_error_px",
                "max_abs_residual_past_input_correlation",
            ],
            "residual_input_lags": "1_through_maximum_preregistered_horizon_32",
            "numerical_zero_residual_rule": (
                "l2_norm<=sqrt(rows)*max(rows,cols,1)*float64_eps"
            ),
        },
        "selection": {
            "metric_round_decimal_places": _METRIC_ROUND_DECIMAL_PLACES,
            "tie_break": [
                "lowest_rounded_worst_witness_rmse_px",
                "lowest_rounded_worst_block_rmse_px",
                "lowest_rounded_max_abs_residual_past_input_correlation",
                "lowest_horizon",
            ],
            "selected_must_strictly_beat_h4": True,
            "selected_must_strictly_beat_nuisance_only": True,
            "selected_residual_input_correlation_must_not_exceed_h4": True,
            "strict_comparison_tolerance": (
                "max(1,abs(lhs),abs(rhs))*float64_eps*max_validation_rows"
            ),
        },
        "f1": {
            "bind_primary_artifact_hashes": True,
            "freeze_selected_h_and_all_witness_fir_coefficients": True,
            "freeze_h4_deletion_control_coefficients": True,
            "cross_run_holdout_used_for_tuning": False,
            "cross_run_selected_must_beat_fixed_h4": True,
            "cross_run_selected_must_beat_nuisance_only": True,
            "cross_run_error_budget_rule": (
                "primary_within_run_metric_plus_frozen_a2_mapping_uncertainty_px"
            ),
        },
        "failure_semantics": {
            "any_invalid_block_is_red": True,
            "missing_rows_may_be_dropped": False,
            "unknown_prehistory_zero_padding_allowed": False,
            "nonfinite_value_is_red": True,
            "rank_failure_is_red": True,
            "holdout_used_for_tuning": False,
            "automatic_green_is_human_acceptance": False,
        },
        "runtime": {
            "python_version": sys.version.split()[0],
            "numpy_version": np.__version__,
            "opencv_version": cv2.__version__,
        },
    }
    contract["contract_semantic_sha256"] = canonical_semantic_sha256(
        contract, "contract_semantic_sha256")
    return contract


def _matrix_metrics(matrix: np.ndarray) -> dict:
    if matrix.ndim != 2 or matrix.shape[0] == 0 or matrix.shape[1] == 0:
        raise ValueError("设计矩阵必须是非空二维矩阵")
    singular_values = np.linalg.svd(matrix, compute_uv=False)
    if singular_values.size == 0 or not np.all(np.isfinite(singular_values)):
        raise ValueError("设计矩阵 SVD 非有限")
    largest = float(singular_values[0])
    tolerance = max(matrix.shape) * largest * np.finfo(np.float64).eps
    rank = int(np.count_nonzero(singular_values > tolerance))
    condition = None
    if rank == matrix.shape[1] and float(singular_values[-1]) > 0.0:
        condition = largest / float(singular_values[-1])
    return {
        "shape": [int(matrix.shape[0]), int(matrix.shape[1])],
        "rank": rank,
        "rank_tolerance": tolerance,
        "full_column_rank": rank == matrix.shape[1],
        "singular_values": [float(value) for value in singular_values],
        "condition_number": condition,
    }


def _svd_least_squares(matrix: np.ndarray, output: np.ndarray) -> tuple[np.ndarray, dict]:
    metrics = _matrix_metrics(matrix)
    if not metrics["full_column_rank"]:
        raise ValueError("设计矩阵不是 full column rank")
    if output.ndim != 1 or output.shape[0] != matrix.shape[0] or \
            not np.all(np.isfinite(output)):
        raise ValueError("模型输出向量容量或数值非法")
    left, singular_values, right_transpose = np.linalg.svd(
        matrix, full_matrices=False)
    tolerance = float(metrics["rank_tolerance"])
    if np.any(singular_values <= tolerance):
        raise ValueError("设计矩阵奇异值未越过冻结 tolerance")
    coefficients = right_transpose.T @ (
        (left.T @ output) / singular_values)
    if not np.all(np.isfinite(coefficients)):
        raise ValueError("最小二乘系数非有限")
    return coefficients, metrics


def _validate_sequence_and_measurements(
        sequence: dict,
        measurements: dict[str, Sequence[float]],
        candidate_horizons: Sequence[int],
        acceptance_eligible_horizons: Sequence[int]) -> tuple[np.ndarray, dict[str, np.ndarray]]:
    samples = sequence.get("samples")
    blocks = sequence.get("blocks")
    if not isinstance(samples, list) or not isinstance(blocks, list) or \
            len(samples) != 416 or len(blocks) != 4:
        raise ValueError("Physical B Primary sequence 容量必须为 416/4")
    if sequence.get("input_definition") != "cumulative_position_counts" or \
            sequence.get("sequence_semantic_sha256") != \
            _EXPECTED_PRIMARY_SEQUENCE_SHA256 or \
            sequence.get("pair_roles") != [
                "estimation", "within_run_validation"]:
        raise ValueError("Physical B Primary exact sequence 身份非法")
    if list(candidate_horizons) != [4, 8, 16, 32] or \
            list(acceptance_eligible_horizons) != [8, 16, 32]:
        raise ValueError("Physical B H 候选或 A2 tail 裁决已漂移")

    position = 0
    positions: list[int] = []
    for index, sample in enumerate(samples):
        if int(sample.get("sample_index", -1)) != index or \
                int(sample.get("command_dy_counts", 1)) != 0:
            raise ValueError("Physical B sample index/Y 合同非法")
        command = int(sample.get("command_dx_counts", 2))
        if command not in (-1, 0, 1):
            raise ValueError("Physical B command 不是 -1/0/+1")
        position += command
        if position != int(sample.get("position_x_counts", 999)) or \
                position != int(sample.get("identification_input_x_counts", 999)):
            raise ValueError("Physical B cumulative-position input 无法由 command 重建")
        positions.append(position)
    if position != 0 or max(abs(value) for value in positions) != 1:
        raise ValueError("Physical B sequence 未满足净零/最大前缀 1")

    expected_roles = [
        "estimation", "estimation",
        "within_run_validation", "within_run_validation",
    ]
    expected_polarities = ["normal", "inverted", "normal", "inverted"]
    for index, block in enumerate(blocks):
        if int(block.get("block_id", -1)) != index + 1 or \
                int(block.get("pair_index", -1)) != index // 2 + 1 or \
                block.get("role") != expected_roles[index] or \
                block.get("polarity") != expected_polarities[index] or \
                int(block.get("period_sample_count", -1)) != 63 or \
                int(block.get("sample_count", -1)) != 64:
            raise ValueError("Physical B whole-block role/polarity 合同非法")

    arrays: dict[str, np.ndarray] = {}
    if set(measurements) != {"left", "right"}:
        raise ValueError("Physical B 必须同时提供左右 witness 输出")
    for witness, values in measurements.items():
        array = np.asarray(values, dtype=np.float64)
        if array.shape != (len(samples),) or not np.all(np.isfinite(array)):
            raise ValueError(f"{witness} witness 输出容量或数值非法")
        arrays[witness] = array
    return np.asarray(positions, dtype=np.float64), arrays


def _block_output_indices(sequence: dict, block: dict) -> np.ndarray:
    samples = sequence["samples"]
    first = int(block["first_sample_index"])
    end = first + int(block["sample_count"])
    while end < len(samples) and samples[end]["phase"] == "guard":
        end += 1
    indices = np.arange(first, end, dtype=np.int64)
    if indices.size != 96 or first < 32:
        raise ValueError("Physical B block 必须是 64 response + 32 exact post-guard")
    return indices


def _block_design(
        positions: np.ndarray,
        indices: np.ndarray,
        horizon: int) -> tuple[np.ndarray, np.ndarray]:
    if int(indices[0]) < horizon:
        raise ValueError("Physical B block prehistory 不足，禁止补零")
    row_count = int(indices.size)
    denominator = max(row_count - 1, 1)
    nuisance = np.asarray([
        [1.0, row / denominator] for row in range(row_count)
    ], dtype=np.float64)
    inputs = np.asarray([
        [positions[index - lag] for lag in range(1, horizon + 1)]
        for index in indices
    ], dtype=np.float64)
    return nuisance, inputs


def _fit_estimation_witness(
        sequence: dict,
        positions: np.ndarray,
        output: np.ndarray,
        horizon: int) -> dict:
    blocks = [
        block for block in sequence["blocks"]
        if block["role"] == "estimation"
    ]
    if len(blocks) != 2:
        raise ValueError("Primary estimation 必须是完整 normal/inverted pair")
    row_matrices: list[np.ndarray] = []
    outputs: list[np.ndarray] = []
    for block_index, block in enumerate(blocks):
        indices = _block_output_indices(sequence, block)
        nuisance, inputs = _block_design(positions, indices, horizon)
        block_nuisance = np.zeros((indices.size, 4), dtype=np.float64)
        block_nuisance[:, block_index * 2:block_index * 2 + 2] = nuisance
        row_matrices.append(np.hstack((block_nuisance, inputs)))
        outputs.append(output[indices])
    matrix = np.vstack(row_matrices)
    observed = np.concatenate(outputs)
    coefficients, metrics = _svd_least_squares(matrix, observed)
    predicted = matrix @ coefficients
    residual = observed - predicted
    return {
        "fir_coefficients": [float(value) for value in coefficients[-horizon:]],
        "nuisance_coefficients_by_estimation_block": [
            [float(value) for value in coefficients[0:2]],
            [float(value) for value in coefficients[2:4]],
        ],
        "design_matrix": metrics,
        "fit_rmse_px": float(np.sqrt(np.mean(residual * residual))),
        "fit_max_abs_error_px": float(np.max(np.abs(residual))),
    }


def _residual_input_correlation(
        residual: np.ndarray,
        inputs: np.ndarray) -> tuple[list[float], float]:
    residual_norm = float(np.linalg.norm(residual))
    numerical_zero = math.sqrt(max(int(residual.size), 1)) * \
        max(int(residual.size), int(inputs.shape[1]), 1) * \
        np.finfo(np.float64).eps
    correlations: list[float] = []
    for column in range(inputs.shape[1]):
        input_column = inputs[:, column]
        denominator = residual_norm * float(np.linalg.norm(input_column))
        value = 0.0 if residual_norm <= numerical_zero or denominator == 0.0 else \
            float(np.dot(residual, input_column) / denominator)
        if not math.isfinite(value):
            raise ValueError("residual-past-input correlation 非有限")
        correlations.append(value)
    return correlations, max((abs(value) for value in correlations), default=0.0)


def _error_metrics(
        residual: np.ndarray,
        inputs: np.ndarray) -> dict:
    correlations, maximum_correlation = _residual_input_correlation(
        residual, inputs)
    absolute = np.abs(residual)
    return {
        "sample_count": int(residual.size),
        "rmse_px": float(np.sqrt(np.mean(residual * residual))),
        "mae_px": float(np.mean(absolute)),
        "max_abs_error_px": float(np.max(absolute)),
        "residual_past_input_correlations": correlations,
        "max_abs_residual_past_input_correlation": maximum_correlation,
    }


def _validate_candidate(
        sequence: dict,
        positions: np.ndarray,
        outputs: dict[str, np.ndarray],
        horizon: int,
        estimation: dict[str, dict] | None,
        residual_audit_horizon: int) -> tuple[dict, dict]:
    blocks = [
        block for block in sequence["blocks"]
        if block["role"] == "within_run_validation"
    ]
    if len(blocks) != 2:
        raise ValueError("within-Run validation 必须是完整 normal/inverted pair")
    witness_results: dict[str, dict] = {}
    nuisance_results: dict[str, dict] = {}
    for witness, output in outputs.items():
        fir = np.zeros(horizon, dtype=np.float64) if estimation is None else \
            np.asarray(estimation[witness]["fir_coefficients"], dtype=np.float64)
        block_results: list[dict] = []
        nuisance_block_results: list[dict] = []
        all_residuals: list[np.ndarray] = []
        all_inputs: list[np.ndarray] = []
        all_nuisance_residuals: list[np.ndarray] = []
        for block in blocks:
            indices = _block_output_indices(sequence, block)
            nuisance, inputs = _block_design(positions, indices, horizon)
            _, residual_audit_inputs = _block_design(
                positions, indices, residual_audit_horizon)
            observed = output[indices]
            forced = inputs @ fir
            nuisance_coefficients, nuisance_matrix_metrics = \
                _svd_least_squares(nuisance, observed - forced)
            predicted = forced + nuisance @ nuisance_coefficients
            residual = observed - predicted
            metrics = _error_metrics(residual, residual_audit_inputs)
            block_results.append({
                "block_id": int(block["block_id"]),
                "polarity": str(block["polarity"]),
                "nuisance_coefficients": [
                    float(value) for value in nuisance_coefficients
                ],
                "nuisance_matrix": nuisance_matrix_metrics,
                **metrics,
            })
            all_residuals.append(residual)
            all_inputs.append(residual_audit_inputs)

            null_coefficients, _ = _svd_least_squares(nuisance, observed)
            null_residual = observed - nuisance @ null_coefficients
            nuisance_block_results.append({
                "block_id": int(block["block_id"]),
                "polarity": str(block["polarity"]),
                "nuisance_coefficients": [
                    float(value) for value in null_coefficients
                ],
                **_error_metrics(null_residual, residual_audit_inputs),
            })
            all_nuisance_residuals.append(null_residual)
        aggregate_residual = np.concatenate(all_residuals)
        aggregate_inputs = np.vstack(all_inputs)
        witness_results[witness] = {
            "blocks": block_results,
            "aggregate": _error_metrics(aggregate_residual, aggregate_inputs),
        }
        nuisance_residual = np.concatenate(all_nuisance_residuals)
        nuisance_results[witness] = {
            "blocks": nuisance_block_results,
            "aggregate": _error_metrics(nuisance_residual, aggregate_inputs),
        }
    return witness_results, nuisance_results


def _validation_summary(witness_results: dict[str, dict]) -> dict:
    witness_rmse = {
        witness: float(result["aggregate"]["rmse_px"])
        for witness, result in witness_results.items()
    }
    block_rmse = [
        float(block["rmse_px"])
        for result in witness_results.values()
        for block in result["blocks"]
    ]
    correlations = [
        float(result["aggregate"]
              ["max_abs_residual_past_input_correlation"])
        for result in witness_results.values()
    ]
    max_errors = [
        float(result["aggregate"]["max_abs_error_px"])
        for result in witness_results.values()
    ]
    return {
        "witnesses": witness_results,
        "worst_witness_rmse_px": max(witness_rmse.values()),
        "worst_block_rmse_px": max(block_rmse),
        "worst_witness_max_abs_error_px": max(max_errors),
        "max_abs_residual_past_input_correlation": max(correlations),
    }


def _selection_key(candidate: dict) -> tuple[float, float, float, int]:
    validation = candidate["validation"]
    return (
        round(float(validation["worst_witness_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
        round(float(validation["worst_block_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
        round(float(validation[
            "max_abs_residual_past_input_correlation"]),
            _METRIC_ROUND_DECIMAL_PLACES),
        int(candidate["horizon"]),
    )


def _strictly_lower(first: float, second: float, row_count: int) -> bool:
    tolerance = max(1.0, abs(first), abs(second)) * \
        np.finfo(np.float64).eps * max(row_count, 1)
    return first < second - tolerance


def fit_primary_models(
        sequence: dict,
        measurements: dict[str, Sequence[float]],
        candidate_horizons: Sequence[int],
        acceptance_eligible_horizons: Sequence[int]) -> dict:
    """用 pair 1 拟合并只用完整 pair 2 选择单一 F1 candidate。"""
    positions, outputs = _validate_sequence_and_measurements(
        sequence, measurements, candidate_horizons,
        acceptance_eligible_horizons)
    candidates: dict[str, dict] = {}
    nuisance_only: dict | None = None
    for horizon in candidate_horizons:
        estimation = {
            witness: _fit_estimation_witness(
                sequence, positions, output, int(horizon))
            for witness, output in outputs.items()
        }
        forced_by_witness, nuisance_by_witness = _validate_candidate(
            sequence, positions, outputs, int(horizon), estimation,
            max(int(value) for value in candidate_horizons))
        validation = _validation_summary(forced_by_witness)
        validation["input_forced"] = copy.deepcopy(forced_by_witness)
        validation["output_free_run"] = copy.deepcopy(forced_by_witness)
        candidate = {
            "horizon": int(horizon),
            "acceptance_eligible": int(horizon) in
                acceptance_eligible_horizons,
            "estimation": estimation,
            "validation": validation,
        }
        candidate["selection_key"] = list(_selection_key(candidate))
        candidates[str(horizon)] = candidate
        if nuisance_only is None:
            nuisance_only = _validation_summary(nuisance_by_witness)

    eligible = [
        candidates[str(horizon)] for horizon in acceptance_eligible_horizons
    ]
    selected = min(eligible, key=_selection_key)
    deletion = candidates["4"]
    assert nuisance_only is not None
    selected_metrics = selected["validation"]
    deletion_metrics = deletion["validation"]
    invalid_reasons: list[str] = []
    validation_rows = 2 * 96
    if not _strictly_lower(
            float(selected_metrics["worst_witness_rmse_px"]),
            float(deletion_metrics["worst_witness_rmse_px"]),
            validation_rows):
        invalid_reasons.append("SELECTED_MODEL_DOES_NOT_BEAT_H4")
    if not _strictly_lower(
            float(selected_metrics["worst_witness_rmse_px"]),
            float(nuisance_only["worst_witness_rmse_px"]),
            validation_rows):
        invalid_reasons.append("SELECTED_MODEL_DOES_NOT_BEAT_NUISANCE_ONLY")
    correlation_tolerance = np.finfo(np.float64).eps * validation_rows
    if float(selected_metrics[
            "max_abs_residual_past_input_correlation"]) > \
            float(deletion_metrics[
                "max_abs_residual_past_input_correlation"]) + \
            correlation_tolerance:
        invalid_reasons.append("SELECTED_RESIDUAL_INPUT_CORRELATION_EXCEEDS_H4")

    result = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_primary_model_selection",
        "status": "PRIMARY_MODEL_SELECTED" if not invalid_reasons else
            "PRIMARY_RED",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "analysis_contract_semantic_sha256":
            physical_b_analysis_contract()["contract_semantic_sha256"],
        "candidate_horizons": [int(value) for value in candidate_horizons],
        "acceptance_eligible_horizons": [
            int(value) for value in acceptance_eligible_horizons
        ],
        "selected_horizon": int(selected["horizon"]),
        "candidates": candidates,
        "nuisance_only_validation": nuisance_only,
        "primary_gate": {
            "ready_for_f1": not invalid_reasons,
            "invalid_reasons": invalid_reasons,
            "whole_block_only": True,
            "holdout_used_for_tuning": False,
        },
    }
    result["selection_semantic_sha256"] = canonical_semantic_sha256(
        result, "selection_semantic_sha256")
    return result


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_json(path: pathlib.Path, description: str) -> dict:
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"{description} 必须是绝对路径普通文件")
    size = path.stat().st_size
    if size <= 0 or size > 256 * 1024 * 1024:
        raise ValueError(f"{description} 为空或超过 256 MiB")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{description} 根必须是 JSON object")
    return value


def _parse_roi(value: str) -> tuple[int, int, int, int]:
    fields = value.split(",")
    if len(fields) != 4:
        raise ValueError("witness ROI 必须是 x,y,width,height")
    result = tuple(int(field, 10) for field in fields)
    if any(number < 0 for number in result[:2]) or \
            any(number <= 0 for number in result[2:]):
        raise ValueError("witness ROI 坐标/尺寸非法")
    return result


def _assert_file_identity(
        path: pathlib.Path, identity: dict, description: str) -> None:
    if not path.is_file() or not isinstance(identity, dict) or \
            int(identity.get("size", -1)) != path.stat().st_size or \
            str(identity.get("sha256", "")) != _file_sha256(path):
        raise ValueError(f"{description} 的 size/SHA 与 Prepare 不一致")


def _observation_fields(path: pathlib.Path) -> dict:
    if not path.is_absolute() or not path.is_file():
        raise ValueError("Physical B OBSERVATION.md 不存在")
    fields: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if not line.startswith("-") or "：" not in line:
            continue
        key, value = line[1:].split("：", 1)
        fields[key.strip()] = value.strip()
    required = {
        "用户原话", "可见视角变化", "物理鼠标/WASD",
        "正/负方向与左右 witness", "遮挡/scene cut", "异常/急停",
    }
    if not required.issubset(fields) or any(
            not fields[name] for name in required):
        raise ValueError("Physical B 人工观察字段尚未完整回收")
    normalized_manual = fields["物理鼠标/WASD"].lower()
    normalized_cut = fields["遮挡/scene cut"].lower()
    normalized_stop = fields["异常/急停"].lower()
    contaminated = not any(
        token in normalized_manual for token in ("未", "无", "没有"))
    scene_cut = not any(
        token in normalized_cut for token in ("未", "无", "没有"))
    abnormal = not any(
        token in normalized_stop for token in ("未", "无", "没有"))
    return {
        "fields": fields,
        "manual_mouse_or_wasd_used": contaminated,
        "occlusion_or_scene_cut_reported": scene_cut,
        "anomaly_or_emergency_stop_reported": abnormal,
        "file_sha256": _file_sha256(path),
    }


def _validate_primary_artifacts(
        run: pathlib.Path, f0_path: pathlib.Path) -> dict:
    paths = {
        "task": run / "task.json",
        "f0": f0_path,
        "offline_design": run / "evidence" / "offline-design.json",
        "a2_decision": run / "evidence" / "a2-decision.json",
        "sequence": run / "sequence.json",
        "binding": run / "probe-binding.json",
        "report": run / "command-report.json",
        "safety_ledger": run / "safety-ledger.json",
        "manifest": run / "pixel-evidence" / "manifest.json",
        "launch_summary": run / "launch-summary.json",
        "observation": run / "OBSERVATION.md",
    }
    if run == pathlib.Path(run.anchor) or not run.is_dir():
        raise ValueError("Physical B Primary Run 必须是既有非根目录")
    task = _load_json(paths["task"], "Physical B task")
    f0 = _load_json(paths["f0"], "Physical B F0")
    offline_design = _load_json(
        paths["offline_design"], "Physical B offline design")
    sequence = _load_json(paths["sequence"], "Physical B sequence")
    binding = _load_json(paths["binding"], "Physical B binding")
    report = _load_json(paths["report"], "Physical B command report")
    ledger = _load_json(paths["safety_ledger"], "Physical B safety ledger")
    manifest = _load_json(paths["manifest"], "Physical B sidecar manifest")
    launch_summary = _load_json(
        paths["launch_summary"], "Physical B launch summary")
    observation = _observation_fields(paths["observation"])

    if int(task.get("schema_version", 0)) != 4 or \
            task.get("evidence_type") != "mouse_effect_probe_b_task" or \
            task.get("status") != "PREPARED" or \
            task.get("dispatch_mode") != "physical_b" or \
            task.get("profile") != "physical_b_prbs_primary" or \
            task.get("run_role") != "primary" or \
            task.get("cross_run_holdout_prepare_authorized") is not False:
        raise ValueError("Physical B Primary task 身份或 holdout 边界非法")
    if f0.get("evidence_type") != \
            "mouse_effect_probe_physical_b_primary_f0" or \
            f0.get("status") != "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE" or \
            f0.get("physical_output_capability") is not False or \
            f0.get("physical_b_launch_authorized") is not False or \
            f0.get("cross_run_holdout", {}).get("prepare_allowed") is not False:
        raise ValueError("Physical B F0 身份或授权边界非法")
    contract = physical_b_analysis_contract()
    frozen_contract = f0.get("analysis_contract")
    if frozen_contract != contract or \
            f0.get("f0_semantic_sha256") != \
            canonical_semantic_sha256(f0, "f0_semantic_sha256"):
        raise ValueError("Physical B F0 分析合同或语义 SHA 已漂移")

    files = task.get("files", {})
    for key, path_key in (
            ("sequence", "sequence"),
            ("probe_binding", "binding"),
            ("primary_f0", "f0"),
            ("offline_design", "offline_design"),
            ("a2_dependency_decision", "a2_decision")):
        _assert_file_identity(paths[path_key], files.get(key), key)
    analyzer_identity = files.get("physical_b_analyzer")
    _assert_file_identity(
        pathlib.Path(__file__).resolve(), analyzer_identity,
        "Physical B analyzer")

    selected = offline_design.get("selected_candidate", {})
    offline_sequence = selected.get("sequence")
    if not isinstance(offline_sequence, dict) or \
            offline_sequence.get("sequence_semantic_sha256") != \
            _EXPECTED_PRIMARY_SEQUENCE_SHA256 or \
            task.get("offline_sequence_semantic_sha256") != \
            _EXPECTED_PRIMARY_SEQUENCE_SHA256:
        raise ValueError("Physical B offline exact sequence 身份非法")
    cpp_samples = sequence.get("samples")
    offline_samples = offline_sequence.get("samples")
    if not isinstance(cpp_samples, list) or \
            not isinstance(offline_samples, list) or \
            len(cpp_samples) != len(offline_samples):
        raise ValueError("Physical B C++/offline sample 容量不一致")
    for index, (actual, expected) in enumerate(
            zip(cpp_samples, offline_samples, strict=True)):
        if int(actual.get("sample_index", -1)) != index or \
                int(actual.get("block_id", -1)) != \
                int(expected.get("block_id", -2)) or \
                actual.get("phase") != expected.get("phase") or \
                int(actual.get("dx_counts", 2)) != \
                int(expected.get("command_dx_counts", 3)) or \
                int(actual.get("dy_counts", 2)) != 0:
            raise ValueError("Physical B C++ sequence 偏离 offline exact sequence")

    result = report.get("result", {})
    if report.get("dispatch_mode") != "physical_b" or \
            report.get("profile") != "physical_b_prbs_primary" or \
            report.get("run_uuid") != task.get("run_uuid") or \
            int(report.get("activation_epoch", 0)) != \
            int(task.get("activation_epoch", -1)) or \
            report.get("sequence_sha256") != task.get("sequence_sha256") or \
            result.get("state") != "completed" or \
            result.get("complete") is not True or \
            result.get("stop_reason") != "normal_completion" or \
            int(result.get("consumed_sample_count", -1)) != 416 or \
            int(result.get("cumulative_requested_x_counts", 1)) != 0 or \
            int(result.get("cumulative_backend_completed_x_counts", 1)) != 0:
        raise ValueError("Physical B command report 不是完整净零 Primary")
    if ledger.get("run_uuid") != task.get("run_uuid") or \
            ledger.get("probe_stop_reason") != "normal_completion" or \
            ledger.get("recording_failed") is not False or \
            int(ledger.get("dropped_observation_count", -1)) != 0 or \
            ledger.get("monitor_packet_recording_failed") is not False or \
            int(ledger.get("dropped_monitor_packet_count", -1)) != 0 or \
            not isinstance(ledger.get("observations"), list) or \
            not ledger.get("observations") or \
            not isinstance(ledger.get("monitor_packets"), list) or \
            not ledger.get("monitor_packets"):
        raise ValueError("Physical B safety ledger 身份非法")
    if manifest.get("evidence_type") != "output_off_capture" or \
            manifest.get("physical_output_capability") is not False or \
            manifest.get("capture_source_name") != \
            task.get("capture", {}).get("source_name") or \
            int(manifest.get("requested_frame_count", -1)) != \
            int(task.get("sidecar", {}).get("frames", -2)) or \
            int(manifest.get("recorded_frame_count", -1)) != \
            int(task.get("sidecar", {}).get("frames", -2)) or \
            manifest.get("source_binding", {}).get("sha256") != \
            files.get("probe_binding", {}).get("sha256"):
        raise ValueError("Physical B sidecar manifest 身份或容量非法")
    if launch_summary.get("evidence_type") != "mouse_effect_probe_b_launch" or \
            launch_summary.get("run_uuid") != task.get("run_uuid") or \
            launch_summary.get("run_role") != "primary" or \
            launch_summary.get("status") != "RECORDED_UNANALYZED" or \
            launch_summary.get("stop_reason") != "normal_completion" or \
            int(launch_summary.get("command_event_count", -1)) != 416 or \
            int(launch_summary.get("source_timestamp_matched_event_count", -1)) != 416 or \
            int(launch_summary.get("source_timestamp_unmatched_baseline_event_count", -1)) != 0:
        raise ValueError("Physical B launch summary 不是完整 Primary")
    if observation["manual_mouse_or_wasd_used"] or \
            observation["occlusion_or_scene_cut_reported"] or \
            observation["anomaly_or_emergency_stop_reported"]:
        raise ValueError("Physical B 人工观察报告输入污染、遮挡或异常")
    return {
        "paths": paths,
        "task": task,
        "f0": f0,
        "offline_design": offline_design,
        "offline_sequence": offline_sequence,
        "sequence": sequence,
        "binding": binding,
        "report": report,
        "ledger": ledger,
        "manifest": manifest,
        "launch_summary": launch_summary,
        "observation": observation,
    }


def _frame_index(manifest: dict) -> dict[int, tuple[int, dict]]:
    frames = manifest.get("frames")
    if not isinstance(frames, list) or len(frames) < 416:
        raise ValueError("Physical B sidecar frames 容量不足")
    result: dict[int, tuple[int, dict]] = {}
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict) or \
                int(frame.get("index", -1)) != index or \
                frame.get("source_timestamp_valid") is not True or \
                frame.get("source_time_timing_valid") is not True or \
                frame.get("source_clock_status") != "VALID" or \
                frame.get("source_time_basis") != "NDI_SDK_SUBMISSION" or \
                int(frame.get("source_dropped_frames", -1)) != 0 or \
                int(frame.get("transport_dropped_frames", -1)) != 0 or \
                int(frame.get("transport_invalid_packets", -1)) != 0:
            raise ValueError("Physical B sidecar frame timing/drop 合同非法")
        timestamp = int(frame.get("source_timestamp", 0))
        if timestamp <= 0 or timestamp in result:
            raise ValueError("Physical B sidecar source timestamp 非正或重复")
        result[timestamp] = (index, frame)
    return result


def _match_primary_events(
        loaded: dict,
        frame_by_timestamp: dict[int, tuple[int, dict]]) -> list[tuple[dict, dict, dict, int]]:
    events = loaded["report"].get("result", {}).get("events")
    samples = loaded["sequence"].get("samples")
    if not isinstance(events, list) or not isinstance(samples, list) or \
            len(events) != 416 or len(samples) != 416:
        raise ValueError("Physical B event/sample 容量必须为 416")
    matched: list[tuple[dict, dict, dict, int]] = []
    previous_manifest_index = -1
    requested_x = 0
    backend_x = 0
    for index, (event, sample) in enumerate(zip(events, samples, strict=True)):
        nominal_x = int(sample.get("dx_counts", 2))
        if int(event.get("sample_index", -1)) != index or \
                int(event.get("block_id", -1)) != int(sample.get("block_id", -2)) or \
                int(event.get("nominal_dx_counts", 2)) != nominal_x or \
                int(event.get("nominal_dy_counts", 2)) != 0 or \
                event.get("run_uuid") != loaded["task"].get("run_uuid") or \
                event.get("sequence_sha256") != loaded["task"].get("sequence_sha256") or \
                event.get("source_timestamp_valid") is not True or \
                event.get("source_clock_status") != "VALID" or \
                event.get("source_time_basis") != "NDI_SDK_SUBMISSION" or \
                int(event.get("source_dropped_frames", -1)) != 0 or \
                int(event.get("transport_dropped_frames", -1)) != 0 or \
                int(event.get("transport_invalid_packets", -1)) != 0 or \
                event.get("safety_allowed") is not True or \
                event.get("mouse_status") != "READY" or \
                event.get("stop_reason") != "none":
            raise ValueError("Physical B event 与 sequence/source/safety 不一致")
        if nominal_x == 0:
            if event.get("dispatch_attempted") is not False or \
                    int(event.get("requested_dx_counts", 1)) != 0 or \
                    event.get("backend_succeeded") is not False or \
                    event.get("protocol_ack_received") is not False:
                raise ValueError("Physical B 零 command sample 被伪造成 dispatch")
        else:
            if abs(nominal_x) != 1 or \
                    event.get("dispatch_attempted") is not True or \
                    int(event.get("requested_dx_counts", 0)) != nominal_x or \
                    event.get("backend_succeeded") is not True or \
                    event.get("protocol_ack_received") is not True:
                raise ValueError("Physical B transition 缺少 request/backend/ACK")
            requested_x += nominal_x
            backend_x += nominal_x
        if int(event.get("cumulative_requested_x_counts", 999)) != requested_x or \
                int(event.get("cumulative_backend_completed_x_counts", 999)) != backend_x:
            raise ValueError("Physical B cumulative requested/backend 不守恒")
        frame_match = frame_by_timestamp.get(int(event.get("source_timestamp", 0)))
        if frame_match is None:
            raise ValueError("Physical B event 缺少 exact timestamp sidecar frame")
        manifest_index, frame = frame_match
        if manifest_index <= previous_manifest_index or \
                str(frame.get("source_clock_session_id")) != \
                str(event.get("source_clock_session_id")):
            raise ValueError("Physical B event/frame 顺序或 source session 改变")
        previous_manifest_index = manifest_index
        matched.append((event, sample, frame, manifest_index))
    return matched


def _load_matched_bgr(
        pixel_root: pathlib.Path,
        frame: dict,
        expected_shape: tuple[int, int]) -> np.ndarray:
    relative = pathlib.PurePath(str(frame.get("file", "")))
    path = (pixel_root / relative).resolve()
    if not path.is_relative_to(pixel_root) or not path.is_file() or \
            _file_sha256(path) != str(frame.get("png_sha256", "")):
        raise ValueError("Physical B PNG 路径或 SHA-256 非法")
    bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if bgr is None or bgr.ndim != 3 or bgr.shape[:2] != expected_shape:
        raise ValueError("Physical B PNG 无法解码或几何漂移")
    bgr_sha = hashlib.sha256(
        np.ascontiguousarray(bgr).tobytes()).hexdigest()
    if bgr_sha != str(frame.get("bgr_sha256", "")):
        raise ValueError("Physical B decoded BGR SHA-256 不匹配")
    return bgr


def _measure_primary_witnesses(
        loaded: dict,
        matched: list[tuple[dict, dict, dict, int]]) -> tuple[dict, list[dict], dict]:
    task = loaded["task"]
    image_width = int(task["sidecar"]["roi_width"])
    image_height = int(task["sidecar"]["roi_height"])
    if image_width != 320 or image_height != 320:
        raise ValueError("Physical B F0 只接受 320x320 pixel scope")
    left_roi = _parse_roi(str(task["sidecar"]["left_witness_roi"]))
    right_roi = _parse_roi(str(task["sidecar"]["right_witness_roi"]))
    for roi in (left_roi, right_roi):
        x, y, width, height = roi
        if x + width > image_width or y + height > image_height:
            raise ValueError("Physical B witness ROI 超出图像")
    if not (left_roi[0] + left_roi[2] <= right_roi[0]):
        raise ValueError("Physical B 左右 witness ROI 重叠或顺序非法")
    left_window = cv2.createHanningWindow(
        (left_roi[2], left_roi[3]), cv2.CV_32F)
    right_window = cv2.createHanningWindow(
        (right_roi[2], right_roi[3]), cv2.CV_32F)
    pixel_root = loaded["paths"]["manifest"].parent.resolve()
    cache: dict[int, np.ndarray] = {}

    def frame_bgr(sample_index: int) -> np.ndarray:
        if sample_index not in cache:
            cache[sample_index] = _load_matched_bgr(
                pixel_root, matched[sample_index][2],
                (image_height, image_width))
        return cache[sample_index]

    measurements = {
        "left": [0.0] * len(matched),
        "right": [0.0] * len(matched),
    }
    rows: list[dict] = []
    texture: dict[str, list[float]] = {"left": [], "right": []}
    offline_sequence = loaded["offline_sequence"]
    for block in offline_sequence["blocks"]:
        first = int(block["first_sample_index"])
        anchor_index = first - 1
        indices = _block_output_indices(offline_sequence, block)
        anchor_bgr = frame_bgr(anchor_index)
        anchor_gray = cv2.cvtColor(
            anchor_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
        witness_specs = {
            "left": (left_roi, left_window),
            "right": (right_roi, right_window),
        }
        anchor_witness: dict[str, np.ndarray] = {}
        for witness, (roi, _) in witness_specs.items():
            x, y, width, height = roi
            image = np.ascontiguousarray(
                anchor_gray[y:y + height, x:x + width])
            anchor_witness[witness] = image
            texture[witness].append(float(np.std(image)))
        for sample_index in indices:
            current_bgr = frame_bgr(int(sample_index))
            current_gray = cv2.cvtColor(
                current_bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
            row = {
                "sample_index": int(sample_index),
                "block_id": int(block["block_id"]),
                "pair_index": int(block["pair_index"]),
                "role": str(block["role"]),
                "polarity": str(block["polarity"]),
                "phase": str(offline_sequence["samples"][sample_index]["phase"]),
                "identification_input_x_counts": int(
                    offline_sequence["samples"][sample_index]
                    ["identification_input_x_counts"]),
                "command_dx_counts": int(
                    offline_sequence["samples"][sample_index]
                    ["command_dx_counts"]),
                "manifest_index": int(matched[sample_index][3]),
                "source_timestamp": int(
                    matched[sample_index][0]["source_timestamp"]),
            }
            for witness, (roi, window) in witness_specs.items():
                x, y, width, height = roi
                current = np.ascontiguousarray(
                    current_gray[y:y + height, x:x + width])
                shift, response = cv2.phaseCorrelate(
                    anchor_witness[witness].copy(), current.copy(), window)
                dx, dy = float(shift[0]), float(shift[1])
                response_value = float(response)
                if not all(math.isfinite(value) for value in
                           (dx, dy, response_value)):
                    raise ValueError("Physical B phaseCorrelate 返回非有限值")
                measurements[witness][sample_index] = dx
                row[f"{witness}_dx_px"] = dx
                row[f"{witness}_dy_px"] = dy
                row[f"{witness}_phase_response"] = response_value
            rows.append(row)
    if any(min(values) <= 0.0 for values in texture.values()):
        raise ValueError("Physical B witness anchor 缺少非退化纹理")
    return measurements, rows, {
        "image_width": image_width,
        "image_height": image_height,
        "left_roi": list(left_roi),
        "right_roi": list(right_roi),
        "anchor_texture_gray_stddev": texture,
        "decoded_matched_frame_count": len(cache),
    }


def _write_new_text(path: pathlib.Path, content: str) -> None:
    if not path.is_absolute() or path.exists():
        raise ValueError("分析输出必须是尚不存在的绝对路径")
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + f".pending-{os.getpid()}")
    if pending.exists():
        raise ValueError("分析临时输出已存在")
    try:
        with pending.open("x", encoding="utf-8", newline="\n") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.rename(pending, path)
    finally:
        if pending.exists():
            pending.unlink()


def _build_f1(loaded: dict, selection: dict, geometry: dict) -> dict | None:
    if selection["primary_gate"]["ready_for_f1"] is not True:
        return None
    selected = selection["candidates"][str(selection["selected_horizon"])]
    deletion = selection["candidates"]["4"]
    mapping_budget = float(
        loaded["f0"]["physical_b_primary_prepare_gate"]
        ["mapping_uncertainty_upper_px"])
    f1 = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_f1",
        "status": "F1_FROZEN_READY_FOR_HOLDOUT_REVIEW",
        "physical_output_capability": False,
        "cross_run_holdout_prepare_authorized": False,
        "production_aim_changed": False,
        "scope_id": loaded["task"]["scope_id"],
        "analysis_contract_semantic_sha256":
            selection["analysis_contract_semantic_sha256"],
        "selected_horizon": selection["selected_horizon"],
        "selected_model_by_witness": selected["estimation"],
        "h4_deletion_control_by_witness": deletion["estimation"],
        "primary_within_run_validation": selected["validation"],
        "primary_h4_validation": deletion["validation"],
        "primary_nuisance_only_validation":
            selection["nuisance_only_validation"],
        "geometry": geometry,
        "holdout": {
            "used_for_tuning": False,
            "run_required": "different_run_activation_and_session",
            "lfsr": loaded["f0"]["cross_run_holdout"]["lfsr"],
            "sequence_semantic_sha256":
                loaded["f0"]["cross_run_holdout"]
                ["sequence_semantic_sha256"],
            "selected_must_beat_fixed_h4": True,
            "selected_must_beat_nuisance_only": True,
            "mapping_uncertainty_upper_px": mapping_budget,
            "max_worst_witness_rmse_px":
                float(selected["validation"]["worst_witness_rmse_px"]) +
                mapping_budget,
            "max_worst_block_rmse_px":
                float(selected["validation"]["worst_block_rmse_px"]) +
                mapping_budget,
            "max_worst_witness_max_abs_error_px":
                float(selected["validation"]
                      ["worst_witness_max_abs_error_px"]) + mapping_budget,
            "holdout_used_for_tuning": False,
        },
        "bindings": {
            "run_uuid": loaded["task"]["run_uuid"],
            "activation_epoch": loaded["task"]["activation_epoch"],
            "f0_semantic_sha256": loaded["f0"]["f0_semantic_sha256"],
            "f0_file_sha256": _file_sha256(loaded["paths"]["f0"]),
            "task_file_sha256": _file_sha256(loaded["paths"]["task"]),
            "sequence_file_sha256": _file_sha256(
                loaded["paths"]["sequence"]),
            "command_report_file_sha256": _file_sha256(
                loaded["paths"]["report"]),
            "sidecar_manifest_file_sha256": _file_sha256(
                loaded["paths"]["manifest"]),
            "safety_ledger_file_sha256": _file_sha256(
                loaded["paths"]["safety_ledger"]),
            "launch_summary_file_sha256": _file_sha256(
                loaded["paths"]["launch_summary"]),
            "observation_file_sha256":
                loaded["observation"]["file_sha256"],
            "analyzer_file_sha256": _file_sha256(
                pathlib.Path(__file__).resolve()),
        },
    }
    f1["f1_semantic_sha256"] = canonical_semantic_sha256(
        f1, "f1_semantic_sha256")
    return f1


def analyze_primary_run(
        run_directory: pathlib.Path,
        f0_path: pathlib.Path,
        samples_csv_path: pathlib.Path) -> tuple[dict, str]:
    run = run_directory.resolve()
    resolved_f0 = f0_path.resolve()
    loaded = _validate_primary_artifacts(run, resolved_f0)
    frames = _frame_index(loaded["manifest"])
    matched = _match_primary_events(loaded, frames)
    measurements, rows, geometry = _measure_primary_witnesses(
        loaded, matched)
    selection = fit_primary_models(
        loaded["offline_sequence"],
        measurements,
        candidate_horizons=loaded["f0"]["candidate_horizons"],
        acceptance_eligible_horizons=
            loaded["f0"]["acceptance_eligible_horizons"],
    )
    if not rows:
        raise ValueError("Physical B Primary 没有 whole-block 输出行")
    csv_buffer = io.StringIO(newline="")
    writer = csv.DictWriter(csv_buffer, fieldnames=list(rows[0].keys()))
    writer.writeheader()
    writer.writerows(rows)
    csv_content = csv_buffer.getvalue()
    csv_sha256 = hashlib.sha256(csv_content.encode("utf-8")).hexdigest()
    f1 = _build_f1(loaded, selection, geometry)
    result = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_primary_analysis",
        "status": "F1_FROZEN" if f1 is not None else "PRIMARY_RED",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "run_uuid": loaded["task"]["run_uuid"],
        "activation_epoch": loaded["task"]["activation_epoch"],
        "scope_id": loaded["task"]["scope_id"],
        "profile": "physical_b_prbs_primary",
        "analysis_contract": physical_b_analysis_contract(),
        "geometry": geometry,
        "source_timestamp_matched_event_count": len(matched),
        "whole_block_output_row_count": len(rows),
        "observation": loaded["observation"],
        "model_selection": selection,
        "f1": f1,
        "samples_csv": {
            "path": str(samples_csv_path.resolve()),
            "size": len(csv_content.encode("utf-8")),
            "sha256": csv_sha256,
        },
        "bindings": {
            "task_file_sha256": _file_sha256(loaded["paths"]["task"]),
            "f0_file_sha256": _file_sha256(loaded["paths"]["f0"]),
            "sequence_file_sha256": _file_sha256(
                loaded["paths"]["sequence"]),
            "command_report_file_sha256": _file_sha256(
                loaded["paths"]["report"]),
            "safety_ledger_file_sha256": _file_sha256(
                loaded["paths"]["safety_ledger"]),
            "sidecar_manifest_file_sha256": _file_sha256(
                loaded["paths"]["manifest"]),
            "launch_summary_file_sha256": _file_sha256(
                loaded["paths"]["launch_summary"]),
            "observation_file_sha256":
                loaded["observation"]["file_sha256"],
            "analyzer_file_sha256": _file_sha256(
                pathlib.Path(__file__).resolve()),
        },
        "cross_run_holdout_prepare_authorized": False,
        "holdout_used_for_tuning": False,
    }
    result["analysis_semantic_sha256"] = canonical_semantic_sha256(
        result, "analysis_semantic_sha256")
    return result, csv_content


def _parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Physical B Primary 整块像素 FIR 选择并生成 F1")
    parser.add_argument("--primary-run", type=pathlib.Path, required=True)
    parser.add_argument("--f0", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--samples-csv", type=pathlib.Path, required=True)
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    try:
        options = _parse_arguments(arguments)
        if not options.primary_run.is_absolute() or \
                not options.f0.is_absolute() or \
                not options.output.is_absolute() or \
                not options.samples_csv.is_absolute():
            raise ValueError("Physical B analyzer 所有路径必须是绝对路径")
        if options.output.exists() or options.samples_csv.exists():
            raise ValueError("Physical B analyzer 拒绝覆盖既有输出")
        result, csv_content = analyze_primary_run(
            options.primary_run, options.f0, options.samples_csv)
        _write_new_text(options.samples_csv, csv_content)
        result["samples_csv"]["size"] = options.samples_csv.stat().st_size
        result["samples_csv"]["sha256"] = _file_sha256(
            options.samples_csv)
        result["analysis_semantic_sha256"] = canonical_semantic_sha256(
            result, "analysis_semantic_sha256")
        _write_new_text(
            options.output,
            json.dumps(result, ensure_ascii=False, indent=2,
                       allow_nan=False) + "\n",
        )
        print(
            "Physical B Primary analysis "
            f"{result['status']}: selected_h="
            f"{result['model_selection']['selected_horizon']}, "
            f"matched={result['source_timestamp_matched_event_count']}, "
            f"output={options.output}"
        )
        return 0 if result["status"] == "F1_FROZEN" else 1
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as exception:
        print(f"Physical B Primary analysis 失败: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
