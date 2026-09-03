#!/usr/bin/env python3
"""Physical B Primary v2 的 core/tail deletion 与 F1 证据生成。"""

from __future__ import annotations

import argparse
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
    "b69917ffdbf32061644c1531913371590e81719ee5b2440eb0609fba2c9c0b2d"
)
_CORE_DELAY_SAMPLES = 4
_TAIL_LENGTHS = (0, 1, 2, 4, 8)
_ALTERNATIVE_CORE_DELAYS = (1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)
_TIME_SHIFTS = (-16, -15, -14, -13, 13, 14, 15, 16)
_PHASE_ROTATIONS = tuple(range(13, 51))
_GUARD_SAMPLE_COUNT = 32
_PERIOD_SAMPLE_COUNT = 63
_BLOCK_SAMPLE_COUNT = 64
_BLOCK_OUTPUT_SAMPLE_COUNT = 96
_PRIMARY_SAMPLE_COUNT = 800


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
        "schema_version": 2,
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
            "family": (
                "delayed_static_gain_with_optional_relative_command_tail"
            ),
            "identification_input": "cumulative_position_counts",
            "tail_input": "completed_command_dx_counts",
            "actuator_audit_input": "completed_command_dx_counts",
            "outputs": ["left_witness_dx_px", "right_witness_dx_px"],
            "core_delay_samples": 4,
            "tail_lengths": [0, 1, 2, 4, 8],
            "tail_lag_origin": 4,
            "expected_background_gain_sign": "negative",
            "output_feedback_used": False,
            "dtype": "float64",
            "column_scaling": "none",
            "solver": "svd_minimum_norm_full_column_rank",
            "rank_tolerance": "max(rows,cols)*sigma_max*float64_eps",
            "nuisance_columns_per_block": [
                "constant_1", "sample_offset_from_last_pre_guard"
            ],
        },
        "alternative_core_delays": [
            1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
        ],
        "split": {
            "estimation_pair_index": 1,
            "selection_pair_index": 2,
            "confirmation_pair_index": 3,
            "pair_roles": ["estimation", "selection", "confirmation"],
            "whole_block_only": True,
            "random_frame_split_allowed": False,
            "rows_may_cross_block_boundary": False,
            "guard_rows_shared_between_blocks": False,
            "nuisance_fit_rows": "exact_dedicated_pre_guard_only",
            "plant_fit_rows": "complete_period_return_plus_exact_post_guard",
            "selection_used_for_single_refit": True,
            "confirmation_used_for_refit": False,
        },
        "validation": {
            "input_forced_required": True,
            "confirmation_nuisance_rule": (
                "fit_constant_and_linear_only_on_each_exact_pre_guard; "
                "extrapolate_without_consuming_response_or_post_guard"
            ),
            "metrics": [
                "rmse_px", "mae_px", "max_abs_error_px",
                "worst_witness_rmse_px", "worst_block_rmse_px",
            ],
            "residual_diagnostics": {
                "inputs": [
                    "completed_command_dx_counts",
                    "cumulative_position_counts",
                ],
                "lags": list(range(13)),
                "automatic_gate": False,
            },
            "numerical_zero_residual_rule": (
                "l2_norm<=sqrt(rows)*max(rows,cols,1)*float64_eps"
            ),
        },
        "negative_controls": {
            "time_shifts": [-16, -15, -14, -13, 13, 14, 15, 16],
            "time_shift_wrap_allowed": False,
            "phase_rotations": list(range(13, 51)),
            "phase_rotation_period_samples": 63,
            "phase_rotation_rebuilds_command_from_position": True,
            "rank_fraction": "1/39_engineering_control_not_p_value",
        },
        "deletion_tests": {
            "core_vs_nuisance": "DT-N",
            "delay_specificity": "DT-D",
            "conditional_gain": "DT-G",
            "tail": "DT-T",
            "time_alignment": "DT-TIME",
            "phase_alignment": "DT-PHASE",
            "tail_failure_falls_back_to_core": True,
        },
        "selection": {
            "metric_round_decimal_places": _METRIC_ROUND_DECIMAL_PLACES,
            "tie_break": [
                "lowest_rounded_worst_witness_rmse_px",
                "lowest_rounded_worst_block_rmse_px",
                "lowest_tail_length",
            ],
            "selection_pair_grants_acceptance": False,
            "core_must_strictly_beat_nuisance_on_confirmation": True,
            "delay_4_must_strictly_beat_all_alternatives": True,
            "strict_comparison_tolerance": (
                "max(1,abs(lhs),abs(rhs))*float64_eps*comparison_rows"
            ),
        },
        "f1": {
            "bind_primary_artifact_hashes": True,
            "freeze_delay_tail_gain_and_tail_coefficients": True,
            "confirmation_rows_used_for_refit": False,
            "cross_run_holdout_used_for_tuning": False,
            "cross_run_error_budget_rule": (
                "primary_confirmation_metric_plus_frozen_a2_mapping_uncertainty_px"
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
        measurements: dict[str, Sequence[float]]) -> tuple[
            np.ndarray, np.ndarray, dict[str, np.ndarray]]:
    samples = sequence.get("samples")
    blocks = sequence.get("blocks")
    if not isinstance(samples, list) or not isinstance(blocks, list) or \
            len(samples) != _PRIMARY_SAMPLE_COUNT or len(blocks) != 6:
        raise ValueError("Physical B Primary v2 sequence 容量必须为 800/6")
    if sequence.get("input_definition") != "cumulative_position_counts" or \
            sequence.get("sequence_semantic_sha256") != \
            _EXPECTED_PRIMARY_SEQUENCE_SHA256 or \
            sequence.get("pair_roles") != [
                "estimation", "selection", "confirmation"] or \
            int(sequence.get("guard_sample_count", -1)) != \
            _GUARD_SAMPLE_COUNT:
        raise ValueError("Physical B Primary v2 exact sequence 身份非法")

    position = 0
    commands: list[int] = []
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
        commands.append(command)
        positions.append(position)
    if position != 0 or max(abs(value) for value in positions) != 1:
        raise ValueError("Physical B sequence 未满足净零/最大前缀 1")

    expected_roles = [
        "estimation", "estimation", "selection", "selection",
        "confirmation", "confirmation",
    ]
    expected_polarities = ["normal", "inverted"] * 3
    pre_guard_rows: set[int] = set()
    output_rows: set[int] = set()
    for index, block in enumerate(blocks):
        first = int(block.get("first_sample_index", -1))
        expected_first = _GUARD_SAMPLE_COUNT + index * (
            _GUARD_SAMPLE_COUNT + _BLOCK_SAMPLE_COUNT +
            _GUARD_SAMPLE_COUNT)
        if int(block.get("block_id", -1)) != index + 1 or \
                int(block.get("pair_index", -1)) != index // 2 + 1 or \
                block.get("role") != expected_roles[index] or \
                block.get("polarity") != expected_polarities[index] or \
                first != expected_first or \
                int(block.get("period_sample_count", -1)) != \
                _PERIOD_SAMPLE_COUNT or \
                int(block.get("sample_count", -1)) != _BLOCK_SAMPLE_COUNT:
            raise ValueError("Physical B whole-block role/polarity 合同非法")
        pre = set(range(first - _GUARD_SAMPLE_COUNT, first))
        output = set(range(first, first + _BLOCK_OUTPUT_SAMPLE_COUNT))
        if pre & output or pre_guard_rows & output or output_rows & pre:
            raise ValueError("Physical B pre-guard 与 block 输出行发生共享")
        if any(samples[row].get("phase") != "guard" for row in pre) or \
                any(samples[row].get("phase") != "guard" for row in
                    range(first + _BLOCK_SAMPLE_COUNT,
                          first + _BLOCK_OUTPUT_SAMPLE_COUNT)):
            raise ValueError("Physical B dedicated pre/post guard 合同非法")
        pre_guard_rows.update(pre)
        output_rows.update(output)

    arrays: dict[str, np.ndarray] = {}
    if set(measurements) != {"left", "right"}:
        raise ValueError("Physical B 必须同时提供左右 witness 输出")
    for witness, values in measurements.items():
        array = np.asarray(values, dtype=np.float64)
        if array.shape != (len(samples),) or not np.all(np.isfinite(array)):
            raise ValueError(f"{witness} witness 输出容量或数值非法")
        arrays[witness] = array
    return (
        np.asarray(commands, dtype=np.float64),
        np.asarray(positions, dtype=np.float64),
        arrays,
    )


def _block_output_indices(sequence: dict, block: dict) -> np.ndarray:
    first = int(block["first_sample_index"])
    end = first + int(block["sample_count"]) + _GUARD_SAMPLE_COUNT
    indices = np.arange(first, end, dtype=np.int64)
    if indices.size != _BLOCK_OUTPUT_SAMPLE_COUNT or \
            first < _GUARD_SAMPLE_COUNT:
        raise ValueError("Physical B block 必须是 64 response + 32 exact post-guard")
    return indices


def _block_pre_guard_indices(block: dict) -> np.ndarray:
    first = int(block["first_sample_index"])
    indices = np.arange(
        first - _GUARD_SAMPLE_COUNT, first, dtype=np.int64)
    if indices.size != _GUARD_SAMPLE_COUNT or int(indices[0]) < 0:
        raise ValueError("Physical B block 缺少 exact dedicated pre-guard")
    return indices


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


def _strictly_lower(first: float, second: float, row_count: int) -> bool:
    tolerance = max(1.0, abs(first), abs(second)) * \
        np.finfo(np.float64).eps * max(row_count, 1)
    return bool(first < second - tolerance)


def _nuisance_basis(indices: np.ndarray, first: int) -> np.ndarray:
    offsets = indices.astype(np.float64) - float(first - 1)
    return np.column_stack((np.ones(indices.size, dtype=np.float64), offsets))


def _fit_nuisance_predictions(
        sequence: dict,
        outputs: dict[str, np.ndarray]) -> tuple[dict, dict]:
    public: dict[str, dict] = {}
    predictions: dict[str, dict[int, np.ndarray]] = {}
    for witness, output in outputs.items():
        public[witness] = {}
        predictions[witness] = {}
        for block in sequence["blocks"]:
            block_id = int(block["block_id"])
            first = int(block["first_sample_index"])
            pre = _block_pre_guard_indices(block)
            scored = _block_output_indices(sequence, block)
            coefficients, matrix = _svd_least_squares(
                _nuisance_basis(pre, first), output[pre])
            predicted = _nuisance_basis(scored, first) @ coefficients
            if not np.all(np.isfinite(predicted)):
                raise ValueError("Physical B nuisance 外推产生非有限值")
            predictions[witness][block_id] = predicted
            public[witness][str(block_id)] = {
                "block_id": block_id,
                "pair_index": int(block["pair_index"]),
                "role": str(block["role"]),
                "polarity": str(block["polarity"]),
                "fit_source": "exact_dedicated_pre_guard_only",
                "fit_sample_indices": [int(value) for value in pre],
                "coefficients": [float(value) for value in coefficients],
                "design_matrix": matrix,
            }
    return public, predictions


def _plant_matrix(
        sequence: dict,
        block: dict,
        indices: np.ndarray,
        positions: np.ndarray,
        commands: np.ndarray,
        delay: int,
        tail_length: int,
        include_core: bool,
        include_tail: bool,
        input_shift: int) -> np.ndarray:
    columns: list[np.ndarray] = []
    source_indices: list[np.ndarray] = []
    if include_core:
        source = indices - delay + input_shift
        columns.append(positions[source])
        source_indices.append(source)
    if include_tail:
        for lag in range(tail_length):
            source = indices - delay - lag + input_shift
            columns.append(commands[source])
            source_indices.append(source)
    if not columns:
        raise ValueError("Physical B plant 至少需要一个冻结 regressor")
    first = int(block["first_sample_index"])
    allowed_first = first - _GUARD_SAMPLE_COUNT
    allowed_end = first + _BLOCK_OUTPUT_SAMPLE_COUNT + _GUARD_SAMPLE_COUNT
    if any(int(np.min(source)) < allowed_first or
           int(np.max(source)) >= allowed_end for source in source_indices):
        raise ValueError("Physical B time control 需要跨 block、丢行或补零")
    return np.column_stack(columns).astype(np.float64, copy=False)


def _fit_plant(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        nuisance_predictions: dict[str, dict[int, np.ndarray]],
        positions: np.ndarray,
        commands: np.ndarray,
        roles: Sequence[str],
        *,
        delay: int,
        tail_length: int,
        include_core: bool = True,
        include_tail: bool = True,
        input_shift: int = 0) -> dict:
    role_set = set(roles)
    blocks = [
        block for block in sequence["blocks"]
        if block["role"] in role_set
    ]
    if len(blocks) != len(role_set) * 2:
        raise ValueError("Physical B plant fit 必须使用完整 normal/inverted pair")
    by_witness: dict[str, dict] = {}
    for witness, output in outputs.items():
        matrices: list[np.ndarray] = []
        observed: list[np.ndarray] = []
        for block in blocks:
            indices = _block_output_indices(sequence, block)
            matrices.append(_plant_matrix(
                sequence, block, indices, positions, commands, delay,
                tail_length, include_core, include_tail, input_shift))
            observed.append(
                output[indices] -
                nuisance_predictions[witness][int(block["block_id"])]
            )
        matrix = np.vstack(matrices)
        values = np.concatenate(observed)
        coefficients, metrics = _svd_least_squares(matrix, values)
        offset = 1 if include_core else 0
        by_witness[witness] = {
            "coefficients": [float(value) for value in coefficients],
            "gain": float(coefficients[0]) if include_core else None,
            "tail_coefficients": [
                float(value) for value in coefficients[offset:]
            ],
            "design_matrix": metrics,
        }
    return {
        "delay_samples": int(delay),
        "tail_length": int(tail_length if include_tail else 0),
        "include_core": bool(include_core),
        "include_tail": bool(include_tail and tail_length > 0),
        "input_shift_samples": int(input_shift),
        "fit_roles": list(roles),
        "confirmation_used_for_refit": False,
        "by_witness": by_witness,
    }


def _diagnostic_correlations(
        residual: np.ndarray,
        indices: np.ndarray,
        commands: np.ndarray,
        positions: np.ndarray) -> dict:
    result: dict[str, list[float] | bool] = {"automatic_gate": False}
    for name, signal in (
            ("completed_command_dx_counts", commands),
            ("cumulative_position_counts", positions)):
        matrix = np.column_stack([
            signal[indices - lag] for lag in range(13)
        ]).astype(np.float64, copy=False)
        correlations, _ = _residual_input_correlation(residual, matrix)
        result[name] = [float(value) for value in correlations]
    return result


def _evaluate_model(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        nuisance_predictions: dict[str, dict[int, np.ndarray]],
        positions: np.ndarray,
        commands: np.ndarray,
        role: str,
        model: dict | None,
        *,
        diagnostics: bool = False) -> dict:
    blocks = [
        block for block in sequence["blocks"] if block["role"] == role
    ]
    if len(blocks) != 2:
        raise ValueError("Physical B evaluation 必须使用完整 normal/inverted pair")
    witness_results: dict[str, dict] = {}
    for witness, output in outputs.items():
        block_results: list[dict] = []
        residuals: list[np.ndarray] = []
        for block in blocks:
            indices = _block_output_indices(sequence, block)
            nuisance = nuisance_predictions[witness][int(block["block_id"])]
            forced = np.zeros(indices.size, dtype=np.float64)
            if model is not None:
                matrix = _plant_matrix(
                    sequence, block, indices, positions, commands,
                    int(model["delay_samples"]), int(model["tail_length"]),
                    bool(model["include_core"]), bool(model["include_tail"]),
                    int(model["input_shift_samples"]),
                )
                coefficients = np.asarray(
                    model["by_witness"][witness]["coefficients"],
                    dtype=np.float64,
                )
                forced = matrix @ coefficients
            residual = output[indices] - nuisance - forced
            metrics = _error_metrics(
                residual,
                np.column_stack([positions[indices]]),
            )
            entry = {
                "block_id": int(block["block_id"]),
                "polarity": str(block["polarity"]),
                "nuisance_prediction_frozen_from_pre_guard": True,
                **metrics,
            }
            if diagnostics:
                entry["residual_input_diagnostics"] = \
                    _diagnostic_correlations(
                        residual, indices, commands, positions)
            block_results.append(entry)
            residuals.append(residual)
        aggregate = _error_metrics(
            np.concatenate(residuals),
            np.column_stack([
                positions[np.concatenate([
                    _block_output_indices(sequence, block)
                    for block in blocks
                ])]
            ]),
        )
        witness_results[witness] = {
            "blocks": block_results,
            "aggregate": aggregate,
        }
    summary = _validation_summary(witness_results)
    summary["role"] = role
    return summary


def _v2_selection_key(candidate: dict) -> tuple[float, float, int]:
    selection = candidate["selection"]
    return (
        round(float(selection["worst_witness_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
        round(float(selection["worst_block_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
        int(candidate["tail_length"]),
    )


def _score_key(evaluation: dict) -> tuple[float, float]:
    return (
        round(float(evaluation["worst_witness_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
        round(float(evaluation["worst_block_rmse_px"]),
              _METRIC_ROUND_DECIMAL_PLACES),
    )


def _blockwise_improvement(
        preferred: dict,
        deleted: dict,
        *,
        require_aggregate: bool = True) -> dict:
    comparisons: list[dict] = []
    passed = True
    for witness, preferred_witness in preferred["witnesses"].items():
        deleted_blocks = {
            int(block["block_id"]): block
            for block in deleted["witnesses"][witness]["blocks"]
        }
        for block in preferred_witness["blocks"]:
            deleted_block = deleted_blocks[int(block["block_id"])]
            preferred_rmse = float(block["rmse_px"])
            deleted_rmse = float(deleted_block["rmse_px"])
            block_passed = _strictly_lower(
                preferred_rmse, deleted_rmse, int(block["sample_count"]))
            passed = passed and block_passed
            comparisons.append({
                "witness": witness,
                "block_id": int(block["block_id"]),
                "polarity": str(block["polarity"]),
                "preferred_rmse_px": preferred_rmse,
                "deleted_rmse_px": deleted_rmse,
                "improvement_px": deleted_rmse - preferred_rmse,
                "passed": block_passed,
            })
    aggregate_passed = _strictly_lower(
        float(preferred["worst_witness_rmse_px"]),
        float(deleted["worst_witness_rmse_px"]),
        2 * _BLOCK_OUTPUT_SAMPLE_COUNT,
    )
    if require_aggregate:
        passed = passed and aggregate_passed
    return {
        "passed": passed,
        "aggregate_passed": aggregate_passed,
        "minimum_block_improvement_px": min(
            item["improvement_px"] for item in comparisons),
        "comparisons": comparisons,
    }


def _control_comparison(real: dict, control: dict) -> dict:
    blockwise = _blockwise_improvement(real, control)
    score_passed = _score_key(real) < _score_key(control)
    return {
        "passed": bool(blockwise["passed"] and score_passed),
        "real_score": list(_score_key(real)),
        "control_score": list(_score_key(control)),
        "blockwise": blockwise,
    }


def _select_tail_pipeline(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        nuisance_predictions: dict[str, dict[int, np.ndarray]],
        positions: np.ndarray,
        commands: np.ndarray,
        *,
        input_shift: int = 0) -> dict:
    candidates: dict[str, dict] = {}
    for tail_length in _TAIL_LENGTHS:
        estimation_model = _fit_plant(
            sequence, outputs, nuisance_predictions, positions, commands,
            ["estimation"], delay=_CORE_DELAY_SAMPLES,
            tail_length=tail_length, include_tail=tail_length > 0,
            input_shift=input_shift)
        selection = _evaluate_model(
            sequence, outputs, nuisance_predictions, positions, commands,
            "selection", estimation_model)
        candidate = {
            "tail_length": tail_length,
            "estimation_model": estimation_model,
            "selection": selection,
        }
        candidate["selection_key"] = list(_v2_selection_key(candidate))
        candidates[str(tail_length)] = candidate
    selected = min(candidates.values(), key=_v2_selection_key)
    selected_tail = int(selected["tail_length"])
    refit = _fit_plant(
        sequence, outputs, nuisance_predictions, positions, commands,
        ["estimation", "selection"], delay=_CORE_DELAY_SAMPLES,
        tail_length=selected_tail, include_tail=selected_tail > 0,
        input_shift=input_shift)
    confirmation = _evaluate_model(
        sequence, outputs, nuisance_predictions, positions, commands,
        "confirmation", refit)
    return {
        "selected_tail_length": selected_tail,
        "candidates": candidates,
        "refit_model": refit,
        "confirmation": confirmation,
    }


def _phase_rotated_signals(
        sequence: dict,
        rotation: int) -> tuple[np.ndarray, np.ndarray]:
    positions = np.zeros(len(sequence["samples"]), dtype=np.float64)
    commands = np.zeros(len(sequence["samples"]), dtype=np.float64)
    original = np.asarray([
        sample["identification_input_x_counts"]
        for sample in sequence["samples"]
    ], dtype=np.float64)
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        levels = np.roll(
            original[first:first + _PERIOD_SAMPLE_COUNT], -rotation)
        previous = 0.0
        for offset, level in enumerate(levels):
            positions[first + offset] = level
            commands[first + offset] = level - previous
            previous = level
        commands[first + _PERIOD_SAMPLE_COUNT] = -previous
    if np.any(np.abs(commands) > 1.0) or \
            not np.allclose(np.cumsum(commands), positions, atol=0.0):
        raise ValueError("Physical B phase control 未保持差分/积分/回零合同")
    return positions, commands


def audit_physical_b_sequence_design(sequence: dict) -> dict:
    """在任何 Physical 数据前审计 F0 v2 所需的全部冻结设计矩阵。"""
    zeros = [0.0] * len(sequence.get("samples", []))
    commands, positions, _ = _validate_sequence_and_measurements(
        sequence, {"left": zeros, "right": zeros})
    matrices: list[dict] = []

    def append_nuisance(block: dict) -> None:
        first = int(block["first_sample_index"])
        metrics = _matrix_metrics(_nuisance_basis(
            _block_pre_guard_indices(block), first))
        matrices.append({
            "control_family": "real",
            "model": "nuisance",
            "roles": [str(block["role"])],
            "block_id": int(block["block_id"]),
            **metrics,
        })

    def append_plant(
            control_family: str,
            control_value: int,
            signal_positions: np.ndarray,
            signal_commands: np.ndarray,
            roles: Sequence[str],
            delay: int,
            tail_length: int,
            include_core: bool,
            include_tail: bool,
            input_shift: int = 0) -> None:
        role_set = set(roles)
        blocks = [
            block for block in sequence["blocks"]
            if block["role"] in role_set
        ]
        matrix = np.vstack([
            _plant_matrix(
                sequence, block, _block_output_indices(sequence, block),
                signal_positions, signal_commands, delay, tail_length,
                include_core, include_tail, input_shift)
            for block in blocks
        ])
        metrics = _matrix_metrics(matrix)
        matrices.append({
            "control_family": control_family,
            "control_value": int(control_value),
            "model": "core_tail" if include_core and include_tail else
                "core" if include_core else "tail_without_gain",
            "roles": list(roles),
            "delay_samples": int(delay),
            "tail_length": int(tail_length if include_tail else 0),
            **metrics,
        })

    for block in sequence["blocks"]:
        append_nuisance(block)
    for tail_length in _TAIL_LENGTHS:
        for roles in (["estimation"], ["estimation", "selection"]):
            append_plant(
                "real", 0, positions, commands, roles,
                _CORE_DELAY_SAMPLES, tail_length, True, tail_length > 0)
        if tail_length > 0:
            append_plant(
                "real_gain_deletion", 0, positions, commands,
                ["estimation", "selection"], _CORE_DELAY_SAMPLES,
                tail_length, False, True)
    for delay in _ALTERNATIVE_CORE_DELAYS:
        append_plant(
            "alternative_delay", delay, positions, commands,
            ["estimation", "selection"], delay, 0, True, False)

    for shift in _TIME_SHIFTS:
        for tail_length in _TAIL_LENGTHS:
            for roles in (["estimation"], ["estimation", "selection"]):
                append_plant(
                    "time_shift", shift, positions, commands, roles,
                    _CORE_DELAY_SAMPLES, tail_length, True,
                    tail_length > 0, shift)

    for rotation in _PHASE_ROTATIONS:
        rotated_positions, rotated_commands = _phase_rotated_signals(
            sequence, rotation)
        for tail_length in _TAIL_LENGTHS:
            for roles in (["estimation"], ["estimation", "selection"]):
                append_plant(
                    "phase_rotation", rotation,
                    rotated_positions, rotated_commands, roles,
                    _CORE_DELAY_SAMPLES, tail_length, True,
                    tail_length > 0)
        for tail_length in _TAIL_LENGTHS[1:]:
            append_plant(
                "gain_phase_rotation", rotation,
                rotated_positions, commands,
                ["estimation", "selection"], _CORE_DELAY_SAMPLES,
                tail_length, True, True)
            append_plant(
                "gain_phase_rotation_deletion", rotation,
                rotated_positions, commands,
                ["estimation", "selection"], _CORE_DELAY_SAMPLES,
                tail_length, False, True)
            append_plant(
                "tail_phase_rotation", rotation,
                positions, rotated_commands,
                ["estimation", "selection"], _CORE_DELAY_SAMPLES,
                tail_length, True, True)

    failures = [
        f"{entry['control_family']}:{entry.get('control_value', 0)}:"
        f"{entry['model']}:T{entry.get('tail_length', 0)}"
        for entry in matrices if not entry["full_column_rank"]
    ]
    conditions = [
        float(entry["condition_number"])
        for entry in matrices if entry["condition_number"] is not None
    ]
    return {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_v2_model_rank_audit",
        "all_required_matrices_full_column_rank": not failures,
        "required_matrix_count": len(matrices),
        "failure_labels": failures,
        "worst_condition_number": max(conditions) if conditions else None,
        "matrices": matrices,
    }


def _fit_core_pipeline(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        nuisance_predictions: dict[str, dict[int, np.ndarray]],
        positions: np.ndarray,
        commands: np.ndarray,
        *,
        delay: int = _CORE_DELAY_SAMPLES,
        input_shift: int = 0,
        diagnostics: bool = False) -> tuple[dict, dict]:
    model = _fit_plant(
        sequence, outputs, nuisance_predictions, positions, commands,
        ["estimation", "selection"], delay=delay, tail_length=0,
        include_tail=False, input_shift=input_shift)
    confirmation = _evaluate_model(
        sequence, outputs, nuisance_predictions, positions, commands,
        "confirmation", model, diagnostics=diagnostics)
    return model, confirmation


def _differential_leakage_gate(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        positions: np.ndarray,
        commands: np.ndarray) -> dict:
    differential = {"differential": outputs["left"] - outputs["right"]}
    _, nuisance = _fit_nuisance_predictions(sequence, differential)
    nuisance_confirmation = _evaluate_model(
        sequence, differential, nuisance, positions, commands,
        "confirmation", None)
    _, real_confirmation = _fit_core_pipeline(
        sequence, differential, nuisance, positions, commands)
    real = _blockwise_improvement(
        real_confirmation, nuisance_confirmation)
    surrogate_improvements: list[dict] = []
    for rotation in _PHASE_ROTATIONS:
        rotated_positions, rotated_commands = _phase_rotated_signals(
            sequence, rotation)
        _, confirmation = _fit_core_pipeline(
            sequence, differential, nuisance,
            rotated_positions, rotated_commands)
        comparison = _blockwise_improvement(
            confirmation, nuisance_confirmation)
        surrogate_improvements.append({
            "rotation": rotation,
            "minimum_block_improvement_px":
                comparison["minimum_block_improvement_px"],
        })
    envelope = max(
        item["minimum_block_improvement_px"]
        for item in surrogate_improvements)
    real_improvement = float(real["minimum_block_improvement_px"])
    leaked = bool(real["passed"] and _strictly_lower(
        envelope, real_improvement, 2 * _BLOCK_OUTPUT_SAMPLE_COUNT))
    return {
        "passed": not leaked,
        "real_minimum_block_improvement_px": real_improvement,
        "phase_surrogate_envelope_px": envelope,
        "phase_surrogates": surrogate_improvements,
    }


def _deletion_surrogate_envelope(
        sequence: dict,
        outputs: dict[str, np.ndarray],
        nuisance_predictions: dict[str, dict[int, np.ndarray]],
        positions: np.ndarray,
        commands: np.ndarray,
        *,
        delete: str) -> dict:
    improvements: list[dict] = []
    for rotation in _PHASE_ROTATIONS:
        rotated_positions, rotated_commands = _phase_rotated_signals(
            sequence, rotation)
        if delete == "gain":
            pipeline_positions = rotated_positions
            pipeline_commands = commands
        elif delete == "tail":
            pipeline_positions = positions
            pipeline_commands = rotated_commands
        else:
            raise ValueError("未知 Physical B deletion surrogate")
        pipeline = _select_tail_pipeline(
            sequence, outputs, nuisance_predictions,
            pipeline_positions, pipeline_commands)
        tail_length = int(pipeline["selected_tail_length"])
        if tail_length == 0:
            improvement = 0.0
        else:
            if delete == "gain":
                deleted_model = _fit_plant(
                    sequence, outputs, nuisance_predictions,
                    pipeline_positions, pipeline_commands,
                    ["estimation", "selection"],
                    delay=_CORE_DELAY_SAMPLES, tail_length=tail_length,
                    include_core=False, include_tail=True)
            else:
                deleted_model, _ = _fit_core_pipeline(
                    sequence, outputs, nuisance_predictions,
                    pipeline_positions, pipeline_commands)
            deleted_confirmation = _evaluate_model(
                sequence, outputs, nuisance_predictions,
                pipeline_positions, pipeline_commands,
                "confirmation", deleted_model)
            comparison = _blockwise_improvement(
                pipeline["confirmation"], deleted_confirmation)
            improvement = float(
                comparison["minimum_block_improvement_px"])
        improvements.append({
            "rotation": rotation,
            "selected_tail_length": tail_length,
            "minimum_block_improvement_px": improvement,
        })
    return {
        "maximum_surrogate_minimum_block_improvement_px": max(
            item["minimum_block_improvement_px"] for item in improvements),
        "surrogates": improvements,
    }


def fit_primary_models(
        sequence: dict,
        measurements: dict[str, Sequence[float]]) -> dict:
    """按冻结的三-pair F0 v2 完成 core/tail 分层 deletion tests。"""
    commands, positions, outputs = _validate_sequence_and_measurements(
        sequence, measurements)
    nuisance_public, nuisance_predictions = _fit_nuisance_predictions(
        sequence, outputs)
    nuisance_confirmation = _evaluate_model(
        sequence, outputs, nuisance_predictions, positions, commands,
        "confirmation", None, diagnostics=True)
    core_model, core_confirmation = _fit_core_pipeline(
        sequence, outputs, nuisance_predictions, positions, commands,
        diagnostics=True)
    selection = _select_tail_pipeline(
        sequence, outputs, nuisance_predictions, positions, commands)

    core_invalid: list[str] = []
    dt_n = _blockwise_improvement(
        core_confirmation, nuisance_confirmation)
    if not dt_n["passed"]:
        core_invalid.append("CORE_DOES_NOT_BEAT_NUISANCE")

    delay_controls: list[dict] = []
    for delay in _ALTERNATIVE_CORE_DELAYS:
        _, confirmation = _fit_core_pipeline(
            sequence, outputs, nuisance_predictions,
            positions, commands, delay=delay)
        comparison = _control_comparison(core_confirmation, confirmation)
        delay_controls.append({
            "delay_samples": delay,
            "confirmation": confirmation,
            **comparison,
        })
    if not all(control["passed"] for control in delay_controls):
        core_invalid.append("DELAY_4_NOT_REPLICATED")

    time_controls: list[dict] = []
    for shift in _TIME_SHIFTS:
        _, confirmation = _fit_core_pipeline(
            sequence, outputs, nuisance_predictions,
            positions, commands, input_shift=shift)
        comparison = _control_comparison(core_confirmation, confirmation)
        time_controls.append({
            "shift_samples": shift,
            "confirmation": confirmation,
            **comparison,
        })
    if not all(control["passed"] for control in time_controls):
        core_invalid.append("REAL_ALIGNMENT_NOT_BETTER_THAN_TIME_CONTROLS")

    phase_controls: list[dict] = []
    phase_tail_pipelines: dict[int, dict] = {}
    for rotation in _PHASE_ROTATIONS:
        rotated_positions, rotated_commands = _phase_rotated_signals(
            sequence, rotation)
        _, confirmation = _fit_core_pipeline(
            sequence, outputs, nuisance_predictions,
            rotated_positions, rotated_commands)
        comparison = _control_comparison(core_confirmation, confirmation)
        phase_controls.append({
            "rotation": rotation,
            "confirmation": confirmation,
            **comparison,
        })
        phase_tail_pipelines[rotation] = _select_tail_pipeline(
            sequence, outputs, nuisance_predictions,
            rotated_positions, rotated_commands)
    if not all(control["passed"] for control in phase_controls):
        core_invalid.append("REAL_ALIGNMENT_NOT_BETTER_THAN_PHASE_CONTROLS")

    gains = [
        float(core_model["by_witness"][witness]["gain"])
        for witness in ("left", "right")
    ]
    direction_gate = {
        "expected_background_gain_sign": "negative",
        "gains": gains,
        "passed": all(value < 0.0 for value in gains),
    }
    if not direction_gate["passed"]:
        core_invalid.append("GAIN_DIRECTION_MISMATCH")

    differential_gate = _differential_leakage_gate(
        sequence, outputs, positions, commands)
    if not differential_gate["passed"]:
        core_invalid.append("WITNESS_DIFFERENTIAL_COMMAND_LEAKAGE")

    selected_tail = int(selection["selected_tail_length"])
    selected_model = selection["refit_model"]
    selected_confirmation = selection["confirmation"]
    tail_reasons: list[str] = []
    tail_tests: dict[str, dict] = {}
    if selected_tail > 0:
        no_gain_model = _fit_plant(
            sequence, outputs, nuisance_predictions, positions, commands,
            ["estimation", "selection"], delay=_CORE_DELAY_SAMPLES,
            tail_length=selected_tail, include_core=False, include_tail=True)
        no_gain_confirmation = _evaluate_model(
            sequence, outputs, nuisance_predictions, positions, commands,
            "confirmation", no_gain_model)
        dt_g = _blockwise_improvement(
            selected_confirmation, no_gain_confirmation)
        dt_t = _blockwise_improvement(
            selected_confirmation, core_confirmation)

        gain_surrogates = _deletion_surrogate_envelope(
            sequence, outputs, nuisance_predictions, positions, commands,
            delete="gain")
        tail_surrogates = _deletion_surrogate_envelope(
            sequence, outputs, nuisance_predictions, positions, commands,
            delete="tail")
        gain_phase_passed = _strictly_lower(
            float(gain_surrogates[
                "maximum_surrogate_minimum_block_improvement_px"]),
            float(dt_g["minimum_block_improvement_px"]),
            2 * _BLOCK_OUTPUT_SAMPLE_COUNT)
        tail_phase_passed = _strictly_lower(
            float(tail_surrogates[
                "maximum_surrogate_minimum_block_improvement_px"]),
            float(dt_t["minimum_block_improvement_px"]),
            2 * _BLOCK_OUTPUT_SAMPLE_COUNT)

        tail_time_controls: list[dict] = []
        for shift in _TIME_SHIFTS:
            pipeline = _select_tail_pipeline(
                sequence, outputs, nuisance_predictions,
                positions, commands, input_shift=shift)
            comparison = _control_comparison(
                selected_confirmation, pipeline["confirmation"])
            tail_time_controls.append({
                "shift_samples": shift,
                "selected_tail_length": pipeline["selected_tail_length"],
                **comparison,
            })
        tail_phase_controls: list[dict] = []
        for control in phase_controls:
            rotation = int(control["rotation"])
            pipeline = phase_tail_pipelines[rotation]
            comparison = _control_comparison(
                selected_confirmation, pipeline["confirmation"])
            tail_phase_controls.append({
                "rotation": rotation,
                "selected_tail_length": pipeline["selected_tail_length"],
                **comparison,
            })

        if not dt_g["passed"] or not gain_phase_passed:
            tail_reasons.append("CONDITIONAL_GAIN_DELETION_NOT_SUPPORTED")
        if not dt_t["passed"] or not tail_phase_passed:
            tail_reasons.append("TAIL_DELETION_NOT_SUPPORTED")
        if not all(item["passed"] for item in tail_time_controls):
            tail_reasons.append("TAIL_NOT_BETTER_THAN_TIME_CONTROLS")
        if not all(item["passed"] for item in tail_phase_controls):
            tail_reasons.append("TAIL_NOT_BETTER_THAN_PHASE_CONTROLS")
        tail_tests = {
            "DT-G": dt_g,
            "DT-T": dt_t,
            "NC-GAIN-PHASE": {
                "passed": gain_phase_passed,
                **gain_surrogates,
            },
            "NC-TAIL-PHASE": {
                "passed": tail_phase_passed,
                **tail_surrogates,
            },
            "DT-TIME": tail_time_controls,
            "DT-PHASE": tail_phase_controls,
        }
    else:
        tail_reasons.append("TAIL_NOT_SELECTED")

    core_passed = not core_invalid
    tail_retained = core_passed and selected_tail > 0 and not tail_reasons
    provisional_tail_selection = {
        "selected_tail_length": selected_tail,
        "refit_model": selected_model,
        "confirmation": selected_confirmation,
    }
    if tail_retained:
        frozen_tail_length = selected_tail
        frozen_model = selected_model
        frozen_confirmation = selected_confirmation
    else:
        frozen_tail_length = 0
        frozen_model = core_model
        frozen_confirmation = core_confirmation
    if not core_passed:
        status = "PRIMARY_RED"
        f1_kind = "NO_F1"
    elif tail_retained:
        status = "PRIMARY_CORE_PLUS_TAIL"
        f1_kind = "F1_CORE_TAIL"
    else:
        status = "PRIMARY_CORE_ONLY"
        f1_kind = "F1_CORE"
    result = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_primary_model_selection",
        "status": status,
        "f1_kind": f1_kind,
        "physical_output_capability": False,
        "production_aim_changed": False,
        "analysis_contract_semantic_sha256":
            physical_b_analysis_contract()["contract_semantic_sha256"],
        "selected_tail_length": frozen_tail_length,
        "provisional_tail_selection": provisional_tail_selection,
        "tail_candidates": selection["candidates"],
        "nuisance_by_witness": nuisance_public,
        "nuisance_only_confirmation": nuisance_confirmation,
        "core_model": core_model,
        "core_confirmation": core_confirmation,
        "selected_model": frozen_model,
        "selected_confirmation": frozen_confirmation,
        "core_gate": {
            "passed": core_passed,
            "invalid_reasons": core_invalid,
            "DT-N": dt_n,
            "DT-D": delay_controls,
            "DT-TIME": time_controls,
            "DT-PHASE": phase_controls,
            "gain_direction": direction_gate,
            "witness_differential": differential_gate,
        },
        "tail_gate": {
            "retained": tail_retained,
            "invalid_reasons": tail_reasons,
            "tests": tail_tests,
        },
        "primary_gate": {
            "ready_for_f1": core_passed,
            "invalid_reasons": core_invalid,
            "whole_block_only": True,
            "confirmation_used_for_refit": False,
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

    if int(task.get("schema_version", 0)) != 5 or \
            task.get("evidence_type") != "mouse_effect_probe_b_task" or \
            task.get("status") != "PREPARED" or \
            task.get("dispatch_mode") != "physical_b" or \
            task.get("profile") != "physical_b_prbs_primary" or \
            task.get("run_role") != "primary" or \
            task.get("cross_run_holdout_prepare_authorized") is not False:
        raise ValueError("Physical B Primary task 身份或 holdout 边界非法")
    if f0.get("schema_version") != 2 or \
            f0.get("evidence_type") != \
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
            int(result.get("consumed_sample_count", -1)) != \
            _PRIMARY_SAMPLE_COUNT or \
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
            int(launch_summary.get("command_event_count", -1)) != \
            _PRIMARY_SAMPLE_COUNT or \
            int(launch_summary.get("source_timestamp_matched_event_count", -1)) != \
            _PRIMARY_SAMPLE_COUNT or \
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
    if not isinstance(frames, list) or len(frames) < _PRIMARY_SAMPLE_COUNT:
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
            len(events) != _PRIMARY_SAMPLE_COUNT or \
            len(samples) != _PRIMARY_SAMPLE_COUNT:
        raise ValueError("Physical B event/sample 容量必须为 800")
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
        pre_guard_indices = _block_pre_guard_indices(block)
        output_indices = _block_output_indices(offline_sequence, block)
        indices = np.concatenate((pre_guard_indices, output_indices))
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
                "analysis_region": "nuisance_pre_guard"
                    if int(sample_index) < first else
                    "response_or_post_guard",
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
    selected = selection["selected_model"]
    confirmation = selection["selected_confirmation"]
    mapping_budget = float(
        loaded["f0"]["physical_b_primary_prepare_gate"]
        ["mapping_uncertainty_upper_px"])
    f1 = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_f1",
        "status": selection["f1_kind"],
        "physical_output_capability": False,
        "cross_run_holdout_prepare_authorized": False,
        "production_aim_changed": False,
        "scope_id": loaded["task"]["scope_id"],
        "analysis_contract_semantic_sha256":
            selection["analysis_contract_semantic_sha256"],
        "core_delay_samples": _CORE_DELAY_SAMPLES,
        "selected_tail_length": selection["selected_tail_length"],
        "selected_model": selected,
        "core_model": selection["core_model"],
        "primary_confirmation": confirmation,
        "primary_core_confirmation": selection["core_confirmation"],
        "primary_nuisance_only_confirmation":
            selection["nuisance_only_confirmation"],
        "confirmation_used_for_refit": False,
        "geometry": geometry,
        "holdout": {
            "used_for_tuning": False,
            "run_required": "different_run_activation_and_session",
            "lfsr": loaded["f0"]["cross_run_holdout"]["lfsr"],
            "sequence_semantic_sha256":
                loaded["f0"]["cross_run_holdout"]
                ["sequence_semantic_sha256"],
            "delay_and_tail_are_frozen": True,
            "confirmation_used_for_tuning": False,
            "mapping_uncertainty_upper_px": mapping_budget,
            "max_worst_witness_rmse_px":
                float(confirmation["worst_witness_rmse_px"]) +
                mapping_budget,
            "max_worst_block_rmse_px":
                float(confirmation["worst_block_rmse_px"]) +
                mapping_budget,
            "max_worst_witness_max_abs_error_px":
                float(confirmation[
                    "worst_witness_max_abs_error_px"]) + mapping_budget,
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
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_primary_analysis",
        "status": selection["status"],
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
        "samples_csv_row_count": len(rows),
        "nuisance_pre_guard_row_count":
            6 * _GUARD_SAMPLE_COUNT,
        "whole_block_output_row_count":
            6 * _BLOCK_OUTPUT_SAMPLE_COUNT,
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
        description="Physical B Primary v2 core/tail deletion 分析并生成 F1")
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
            f"{result['status']}: selected_tail="
            f"{result['model_selection']['selected_tail_length']}, "
            f"f1={None if result['f1'] is None else result['f1']['status']}, "
            f"matched={result['source_timestamp_matched_event_count']}, "
            f"output={options.output}"
        )
        return 0 if result["f1"] is not None else 1
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as exception:
        print(f"Physical B Primary analysis 失败: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
