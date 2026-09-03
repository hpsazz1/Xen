#!/usr/bin/env python3
"""Physical B cross-Run holdout 的冻结 F1 绑定与一次性评估。"""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import importlib.util
import io
import json
import math
import pathlib
import sys
from typing import Sequence

import cv2
import numpy as np


_HOLDOUT_SEQUENCE_SEMANTIC_SHA256 = (
    "e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4"
)
_HOLDOUT_PROFILE = "physical_b_prbs_holdout"
_HOLDOUT_SAMPLE_COUNT = 288
_HOLDOUT_BLOCK_COUNT = 2
_GUARD_SAMPLE_COUNT = 32
_PERIOD_SAMPLE_COUNT = 63
_BLOCK_SAMPLE_COUNT = 64
_BLOCK_OUTPUT_SAMPLE_COUNT = 96
_EXPECTED_TRANSITION_COUNT = 68


def _load_primary_analyzer():
    path = pathlib.Path(__file__).resolve().with_name(
        "analyze_mouse_effect_probe_b.py")
    spec = importlib.util.spec_from_file_location(
        "analyze_mouse_effect_probe_b_for_holdout", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载冻结 Primary analyzer: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


PRIMARY = _load_primary_analyzer()


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


def holdout_evaluation_contract() -> dict:
    contract = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_holdout_contract",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "holdout_used_for_tuning": False,
        "frozen_inputs": [
            "primary_analysis_file_sha256",
            "f1_semantic_sha256",
            "holdout_sequence_semantic_sha256",
            "holdout_analyzer_file_sha256",
        ],
        "predictions": ["input_forced", "output_free_run"],
        "output_feedback_used": False,
        "nuisance_fit": "per_block_exact_dedicated_pre_guard_only",
        "scored_rows": "period_return_plus_exact_post_guard",
        "threshold_source": "frozen_f1_holdout_budget",
        "threshold_comparison": "less_than_or_equal",
        "missing_or_duplicate_rows_allowed": False,
        "different_run_uuid_required": True,
        "different_activation_epoch_required": True,
        "different_timing_observation_required": True,
        "different_source_clock_session_required": False,
        "same_source_clock_session_allowed": True,
        "nonoverlapping_source_time_ranges_required": True,
        "event_frame_source_clock_session_match_required": True,
        "automatic_green_is_human_acceptance": False,
    }
    contract["contract_semantic_sha256"] = canonical_semantic_sha256(
        contract, "contract_semantic_sha256")
    return contract


def _file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load_json(path: pathlib.Path, description: str) -> dict:
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"{description} 必须是既有绝对文件路径")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{description} 根对象必须是 JSON object")
    return value


def _valid_sha256(value: object) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value.lower())


def _exact_hash(path: pathlib.Path, expected: str, description: str) -> str:
    if not _valid_sha256(expected):
        raise ValueError(f"{description} expected SHA-256 格式非法")
    actual = _file_sha256(path)
    if actual != expected.lower():
        raise ValueError(f"{description} 文件 SHA-256 不匹配")
    return actual


def _source_timing_observation(
        events: Sequence[dict],
        description: str) -> dict:
    if not events:
        raise ValueError(f"{description} event 为空")
    sessions: set[str] = set()
    steady_times: list[int] = []
    source_timestamps: list[int] = []
    for event in events:
        session = str(event.get("source_clock_session_id", ""))
        steady_time = int(event.get("source_time_at_steady_ns", 0))
        source_timestamp = int(event.get("source_timestamp", 0))
        if not session or steady_time <= 0 or source_timestamp <= 0:
            raise ValueError(f"{description} source timing 身份或时间非法")
        sessions.add(session)
        steady_times.append(steady_time)
        source_timestamps.append(source_timestamp)
    if len(sessions) != 1:
        raise ValueError(f"{description} source clock session 不唯一")
    if any(current <= previous for previous, current in zip(
            steady_times, steady_times[1:])) or \
            any(current <= previous for previous, current in zip(
                source_timestamps, source_timestamps[1:])):
        raise ValueError(f"{description} source timing 顺序不严格递增")
    return {
        "identity_basis": "run_uuid_activation_epoch_source_time_range",
        "source_clock_session_id": next(iter(sessions)),
        "source_time_at_steady_ns": {
            "first": steady_times[0],
            "last": steady_times[-1],
        },
        "source_timestamp": {
            "first": source_timestamps[0],
            "last": source_timestamps[-1],
        },
    }


def validate_cross_run_timing_observation(
        primary: dict,
        holdout_run_uuid: str,
        holdout_activation_epoch: int,
        holdout_events: Sequence[dict]) -> dict:
    primary_run_uuid = str(primary.get("run_uuid", ""))
    primary_activation_epoch = int(primary.get("activation_epoch", 0))
    holdout_run_uuid = str(holdout_run_uuid)
    holdout_activation_epoch = int(holdout_activation_epoch)
    primary_observation = primary.get("timing_observation")
    if not primary_run_uuid or not holdout_run_uuid or \
            primary_activation_epoch <= 0 or holdout_activation_epoch <= 0 or \
            not isinstance(primary_observation, dict):
        raise ValueError("cross-Run timing observation 身份不完整")
    if primary_run_uuid == holdout_run_uuid or \
            primary_activation_epoch == holdout_activation_epoch:
        raise ValueError("holdout 未使用独立 Run UUID/activation")
    if primary_observation.get("identity_basis") != \
            "run_uuid_activation_epoch_source_time_range" or \
            str(primary_observation.get("source_clock_session_id", "")) != \
            str(primary.get("source_clock_session_id", "")):
        raise ValueError("Primary timing observation 与 plan 身份不一致")

    def read_range(name: str) -> tuple[int, int]:
        value = primary_observation.get(name)
        if not isinstance(value, dict):
            raise ValueError(f"Primary timing observation 缺少 {name}")
        first = int(value.get("first", 0))
        last = int(value.get("last", 0))
        if first <= 0 or last < first:
            raise ValueError(f"Primary timing observation {name} 非法")
        return first, last

    primary_steady = read_range("source_time_at_steady_ns")
    primary_timestamp = read_range("source_timestamp")
    holdout_observation = _source_timing_observation(
        holdout_events, "Holdout command report")
    holdout_steady_value = holdout_observation["source_time_at_steady_ns"]
    holdout_timestamp_value = holdout_observation["source_timestamp"]
    holdout_steady = (
        int(holdout_steady_value["first"]),
        int(holdout_steady_value["last"]),
    )
    holdout_timestamp = (
        int(holdout_timestamp_value["first"]),
        int(holdout_timestamp_value["last"]),
    )

    def ranges_overlap(
            primary_range: tuple[int, int],
            holdout_range: tuple[int, int]) -> bool:
        return not (
            primary_range[1] < holdout_range[0] or
            holdout_range[1] < primary_range[0]
        )

    steady_overlap = ranges_overlap(primary_steady, holdout_steady)
    timestamp_overlap = ranges_overlap(primary_timestamp, holdout_timestamp)
    if steady_overlap or timestamp_overlap:
        raise ValueError("Primary/Holdout source timing observation 时间窗重叠")
    different_session = (
        str(primary_observation["source_clock_session_id"]) !=
        str(holdout_observation["source_clock_session_id"])
    )
    return {
        "identity_basis": "run_uuid_activation_epoch_source_time_range",
        "different_run_uuid": True,
        "different_activation_epoch": True,
        "different_timing_observation": True,
        "different_source_clock_session": different_session,
        "same_source_clock_session_allowed": True,
        "source_time_ranges_overlap": False,
        "source_timestamp_ranges_overlap": False,
        "primary": copy.deepcopy(primary_observation),
        "holdout": holdout_observation,
    }


def _validate_f1(f1: dict) -> tuple[dict, dict]:
    if f1.get("schema_version") != 2 or \
            f1.get("evidence_type") != "mouse_effect_probe_physical_b_f1" or \
            f1.get("status") not in ("F1_CORE", "F1_CORE_TAIL") or \
            f1.get("physical_output_capability") is not False or \
            f1.get("cross_run_holdout_prepare_authorized") is not False or \
            f1.get("production_aim_changed") is not False or \
            f1.get("confirmation_used_for_refit") is not False or \
            f1.get("f1_semantic_sha256") != canonical_semantic_sha256(
                f1, "f1_semantic_sha256"):
        raise ValueError("Primary F1 身份、冻结边界或语义 SHA 非法")
    model = f1.get("selected_model")
    holdout = f1.get("holdout")
    if not isinstance(model, dict) or not isinstance(holdout, dict):
        raise ValueError("Primary F1 缺少冻结模型或 holdout 合同")
    tail_length = int(f1.get("selected_tail_length", -1))
    if int(f1.get("core_delay_samples", -1)) != 4 or \
            int(model.get("delay_samples", -1)) != 4 or \
            int(model.get("tail_length", -1)) != tail_length or \
            model.get("include_core") is not True or \
            model.get("include_tail") is not (tail_length > 0) or \
            int(model.get("input_shift_samples", 1)) != 0 or \
            model.get("fit_roles") != ["estimation", "selection"] or \
            model.get("confirmation_used_for_refit") is not False:
        raise ValueError("Primary F1 delay/tail/fit split 已漂移")
    by_witness = model.get("by_witness")
    if not isinstance(by_witness, dict) or set(by_witness) != {"left", "right"}:
        raise ValueError("Primary F1 必须冻结左右 witness 系数")
    expected_coefficients = 1 + tail_length
    for witness in ("left", "right"):
        coefficients = by_witness[witness].get("coefficients")
        if not isinstance(coefficients, list) or \
                len(coefficients) != expected_coefficients or \
                not all(math.isfinite(float(value)) for value in coefficients):
            raise ValueError("Primary F1 witness 系数容量或数值非法")
    lfsr = holdout.get("lfsr")
    if holdout.get("used_for_tuning") is not False or \
            holdout.get("holdout_used_for_tuning") is not False or \
            holdout.get("confirmation_used_for_tuning") is not False or \
            holdout.get("delay_and_tail_are_frozen") is not True or \
            holdout.get("run_required") != \
            "different_run_activation_and_session" or \
            holdout.get("sequence_semantic_sha256") != \
            _HOLDOUT_SEQUENCE_SEMANTIC_SHA256 or \
            not isinstance(lfsr, dict) or \
            int(lfsr.get("order", -1)) != 6 or \
            int(lfsr.get("feedback_mask", -1)) != 0x33 or \
            int(lfsr.get("seed", -1)) != 1 or \
            int(lfsr.get("phase", -1)) != 21 or \
            int(lfsr.get("period_sample_count", -1)) != 63 or \
            lfsr.get("maximum_length_proven_by_state_cycle") is not True:
        raise ValueError("Primary F1 cross-Run holdout recurrence 或 no-tuning 边界非法")
    for name in (
            "max_worst_witness_rmse_px",
            "max_worst_block_rmse_px",
            "max_worst_witness_max_abs_error_px"):
        value = float(holdout.get(name, float("nan")))
        if not math.isfinite(value) or value < 0.0:
            raise ValueError(f"Primary F1 holdout budget 非法: {name}")
    return model, holdout


def _validate_holdout_sequence(
        sequence: dict,
        measurements: dict[str, Sequence[float]] | None = None) -> tuple[
            np.ndarray, np.ndarray, dict[str, np.ndarray] | None]:
    samples = sequence.get("samples")
    blocks = sequence.get("blocks")
    if not isinstance(samples, list) or len(samples) != _HOLDOUT_SAMPLE_COUNT or \
            not isinstance(blocks, list) or len(blocks) != _HOLDOUT_BLOCK_COUNT or \
            sequence.get("input_definition") != "cumulative_position_counts" or \
            sequence.get("sequence_semantic_sha256") != \
            _HOLDOUT_SEQUENCE_SEMANTIC_SHA256 or \
            sequence.get("sequence_semantic_sha256") != \
            canonical_semantic_sha256(sequence, "sequence_semantic_sha256") or \
            sequence.get("pair_roles") != ["cross_run_holdout"] or \
            int(sequence.get("guard_sample_count", -1)) != _GUARD_SAMPLE_COUNT:
        raise ValueError("Physical B holdout exact sequence 身份非法")
    summary = sequence.get("summary", {})
    if int(summary.get("sample_count", -1)) != _HOLDOUT_SAMPLE_COUNT or \
            int(summary.get("net_command_dx_counts", 1)) != 0 or \
            int(summary.get("max_abs_position_x_counts", 2)) != 1 or \
            summary.get("all_commands_x_only_single_count") is not True:
        raise ValueError("Physical B holdout sequence summary 非法")

    position = 0
    commands: list[int] = []
    positions: list[int] = []
    for index, sample in enumerate(samples):
        command = int(sample.get("command_dx_counts", 2))
        if int(sample.get("sample_index", -1)) != index or \
                int(sample.get("command_dy_counts", 1)) != 0 or \
                command not in (-1, 0, 1) or \
                sample.get("role") != "cross_run_holdout":
            raise ValueError("Physical B holdout sample index/source/Y 合同非法")
        position += command
        if int(sample.get("position_x_counts", 999)) != position or \
                int(sample.get("identification_input_x_counts", 999)) != position:
            raise ValueError("Physical B holdout cumulative-position 无法重建")
        commands.append(command)
        positions.append(position)
    if position != 0 or max(abs(value) for value in positions) != 1 or \
            sum(value != 0 for value in commands) != _EXPECTED_TRANSITION_COUNT:
        raise ValueError("Physical B holdout command 净零、前缀或 transition 非法")

    expected_first = (32, 160)
    expected_polarity = ("normal", "inverted")
    pre_rows: set[int] = set()
    output_rows: set[int] = set()
    for index, block in enumerate(blocks):
        first = int(block.get("first_sample_index", -1))
        if int(block.get("block_id", -1)) != index + 1 or \
                int(block.get("pair_index", -1)) != 1 or \
                block.get("role") != "cross_run_holdout" or \
                block.get("polarity") != expected_polarity[index] or \
                first != expected_first[index] or \
                int(block.get("period_sample_count", -1)) != \
                _PERIOD_SAMPLE_COUNT or \
                int(block.get("sample_count", -1)) != _BLOCK_SAMPLE_COUNT:
            raise ValueError("Physical B holdout block 身份或边界非法")
        pre = set(range(first - _GUARD_SAMPLE_COUNT, first))
        output = set(range(first, first + _BLOCK_OUTPUT_SAMPLE_COUNT))
        if pre & output or pre_rows & output or output_rows & pre or \
                any(samples[row].get("phase") != "guard" for row in pre) or \
                any(samples[row].get("phase") != "guard" for row in range(
                    first + _BLOCK_SAMPLE_COUNT,
                    first + _BLOCK_OUTPUT_SAMPLE_COUNT)):
            raise ValueError("Physical B holdout dedicated guard 被共享或污染")
        pre_rows.update(pre)
        output_rows.update(output)

    arrays = None
    if measurements is not None:
        if set(measurements) != {"left", "right"}:
            raise ValueError("Physical B holdout 必须同时提供左右 witness")
        arrays = {}
        for witness, values in measurements.items():
            array = np.asarray(values, dtype=np.float64)
            if array.shape != (_HOLDOUT_SAMPLE_COUNT,) or \
                    not np.all(np.isfinite(array)):
                raise ValueError("Physical B holdout witness 输出容量或数值非法")
            arrays[witness] = array
    return (
        np.asarray(commands, dtype=np.float64),
        np.asarray(positions, dtype=np.float64),
        arrays,
    )


def evaluate_holdout_model(
        sequence: dict,
        measurements: dict[str, Sequence[float]],
        f1: dict) -> dict:
    model, budget = _validate_f1(f1)
    commands, positions, outputs = _validate_holdout_sequence(
        sequence, measurements)
    assert outputs is not None
    nuisance_public, nuisance_predictions = \
        PRIMARY._fit_nuisance_predictions(sequence, outputs)
    evaluation = PRIMARY._evaluate_model(
        sequence,
        outputs,
        nuisance_predictions,
        positions,
        commands,
        "cross_run_holdout",
        copy.deepcopy(model),
        diagnostics=True,
    )
    input_forced = copy.deepcopy(evaluation)
    output_free_run = copy.deepcopy(evaluation)
    checks = {
        "worst_witness_rmse_px":
            float(evaluation["worst_witness_rmse_px"]) <=
            float(budget["max_worst_witness_rmse_px"]),
        "worst_block_rmse_px":
            float(evaluation["worst_block_rmse_px"]) <=
            float(budget["max_worst_block_rmse_px"]),
        "worst_witness_max_abs_error_px":
            float(evaluation["worst_witness_max_abs_error_px"]) <=
            float(budget["max_worst_witness_max_abs_error_px"]),
    }
    return {
        "status": "HOLDOUT_GREEN" if all(checks.values()) else "HOLDOUT_RED",
        "holdout_used_for_tuning": False,
        "confirmation_used_for_tuning": False,
        "frozen_model_semantic_sha256": hashlib.sha256(json.dumps(
            model,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")).hexdigest(),
        "f1_semantic_sha256": f1["f1_semantic_sha256"],
        "prediction_semantics": {
            "input_forced": "frozen exogenous command/position sequence",
            "output_free_run": "same frozen input-only model; no output feedback",
            "numerically_identical_required": True,
        },
        "input_forced": input_forced,
        "output_free_run": output_free_run,
        "frozen_budget": {
            "max_worst_witness_rmse_px":
                float(budget["max_worst_witness_rmse_px"]),
            "max_worst_block_rmse_px":
                float(budget["max_worst_block_rmse_px"]),
            "max_worst_witness_max_abs_error_px":
                float(budget["max_worst_witness_max_abs_error_px"]),
        },
        "budget_checks": checks,
        "nuisance_by_witness": nuisance_public,
    }


def build_holdout_plan(
        primary_analysis_path: pathlib.Path,
        expected_primary_analysis_sha256: str,
        offline_design_path: pathlib.Path,
        expected_offline_design_sha256: str) -> dict:
    primary_path = primary_analysis_path.resolve()
    offline_path = offline_design_path.resolve()
    primary_sha = _exact_hash(
        primary_path, expected_primary_analysis_sha256, "Primary analysis")
    offline_sha = _exact_hash(
        offline_path, expected_offline_design_sha256, "offline design")
    analysis = _load_json(primary_path, "Primary analysis")
    offline = _load_json(offline_path, "offline design")
    if analysis.get("schema_version") != 2 or \
            analysis.get("evidence_type") != \
            "mouse_effect_probe_physical_b_primary_analysis" or \
            analysis.get("status") not in (
                "PRIMARY_CORE_ONLY", "PRIMARY_CORE_PLUS_TAIL") or \
            analysis.get("physical_output_capability") is not False or \
            analysis.get("production_aim_changed") is not False or \
            analysis.get("profile") != "physical_b_prbs_primary" or \
            analysis.get("cross_run_holdout_prepare_authorized") is not False or \
            analysis.get("holdout_used_for_tuning") is not False or \
            analysis.get("analysis_semantic_sha256") != \
            canonical_semantic_sha256(analysis, "analysis_semantic_sha256"):
        raise ValueError("Primary analysis 身份、状态或语义 SHA 非法")
    f1 = analysis.get("f1")
    if not isinstance(f1, dict):
        raise ValueError("Primary analysis 未产生可冻结 F1")
    _, holdout_budget = _validate_f1(f1)
    bindings = analysis.get("bindings")
    if not isinstance(bindings, dict):
        raise ValueError("Primary analysis 缺少输入文件 bindings")
    report_path = primary_path.with_name("command-report.json")
    report_sha = _exact_hash(
        report_path,
        str(bindings.get("command_report_file_sha256", "")),
        "Primary command report",
    )
    report = _load_json(report_path, "Primary command report")
    events = report.get("result", {}).get("events")
    if report.get("run_uuid") != analysis.get("run_uuid") or \
            int(report.get("activation_epoch", -1)) != \
            int(analysis.get("activation_epoch", -2)) or \
            not isinstance(events, list) or not events:
        raise ValueError("Primary command report 与 analysis 身份不一致")
    timing_observation = _source_timing_observation(
        events, "Primary command report")
    primary_session = timing_observation["source_clock_session_id"]

    if offline.get("schema_version") != 2 or \
            offline.get("evidence_type") != \
            "mouse_effect_probe_physical_b_offline_design" or \
            offline.get("status") != "VALID_OFFLINE_DESIGN" or \
            offline.get("physical_output_capability") is not False or \
            offline.get("physical_b_launch_authorized") is not False or \
            offline.get("production_aim_changed") is not False or \
            offline.get("design_semantic_sha256") != \
            canonical_semantic_sha256(offline, "design_semantic_sha256"):
        raise ValueError("Physical B offline design 身份或语义 SHA 非法")
    candidate = offline.get("cross_run_holdout_candidate")
    if not isinstance(candidate, dict) or \
            candidate.get("role") != "cross_run_holdout" or \
            candidate.get("input_definition") != "cumulative_position_counts" or \
            candidate.get("lfsr") != holdout_budget.get("lfsr"):
        raise ValueError("offline holdout candidate 与冻结 F1 不一致")
    sequence = candidate.get("sequence")
    if not isinstance(sequence, dict):
        raise ValueError("offline design 缺少 holdout exact sequence")
    _validate_holdout_sequence(sequence)

    plan = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_holdout_plan",
        "status": "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE",
        "physical_output_capability": False,
        "physical_b_launch_authorized": False,
        "production_aim_changed": False,
        "human_physical_acceptance": "NOT_INFERRED_BY_PLAN",
        "holdout_used_for_tuning": False,
        "contract": holdout_evaluation_contract(),
        "primary": {
            "run_uuid": str(analysis["run_uuid"]),
            "activation_epoch": int(analysis["activation_epoch"]),
            "scope_id": str(analysis["scope_id"]),
            "source_clock_session_id": primary_session,
            "timing_observation": timing_observation,
            "analysis_status": str(analysis["status"]),
            "analysis_semantic_sha256":
                str(analysis["analysis_semantic_sha256"]),
            "f1_semantic_sha256": str(f1["f1_semantic_sha256"]),
        },
        "frozen_f1": copy.deepcopy(f1),
        "sequence": {
            "schema": 5,
            "profile": _HOLDOUT_PROFILE,
            "sample_count": _HOLDOUT_SAMPLE_COUNT,
            "block_count": _HOLDOUT_BLOCK_COUNT,
            "expected_nonzero_transition_count": _EXPECTED_TRANSITION_COUNT,
            "max_abs_prefix_x_counts": 1,
            "lfsr": copy.deepcopy(candidate["lfsr"]),
            "sequence_semantic_sha256":
                _HOLDOUT_SEQUENCE_SEMANTIC_SHA256,
        },
        "offline_sequence": copy.deepcopy(sequence),
        "frozen_budget": {
            name: float(holdout_budget[name]) for name in (
                "max_worst_witness_rmse_px",
                "max_worst_block_rmse_px",
                "max_worst_witness_max_abs_error_px",
            )
        },
        "bindings": {
            "primary_analysis_file_sha256": primary_sha,
            "primary_command_report_file_sha256": report_sha,
            "primary_analyzer_file_sha256":
                str(bindings.get("analyzer_file_sha256", "")),
            "offline_design_file_sha256": offline_sha,
            "holdout_analyzer_file_sha256": _file_sha256(
                pathlib.Path(__file__).resolve()),
        },
    }
    plan["holdout_plan_semantic_sha256"] = canonical_semantic_sha256(
        plan, "holdout_plan_semantic_sha256")
    return plan


def _validate_plan(plan: dict) -> None:
    if plan.get("schema_version") != 2 or \
            plan.get("evidence_type") != \
            "mouse_effect_probe_physical_b_holdout_plan" or \
            plan.get("status") != "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE" or \
            plan.get("physical_output_capability") is not False or \
            plan.get("physical_b_launch_authorized") is not False or \
            plan.get("production_aim_changed") is not False or \
            plan.get("holdout_used_for_tuning") is not False or \
            plan.get("contract") != holdout_evaluation_contract() or \
            plan.get("holdout_plan_semantic_sha256") != \
            canonical_semantic_sha256(plan, "holdout_plan_semantic_sha256"):
        raise ValueError("Physical B holdout plan 身份或语义 SHA 非法")
    bindings = plan.get("bindings", {})
    if bindings.get("holdout_analyzer_file_sha256") != _file_sha256(
            pathlib.Path(__file__).resolve()):
        raise ValueError("Physical B holdout analyzer 已偏离 plan 绑定")
    _validate_f1(plan.get("frozen_f1", {}))
    _validate_holdout_sequence(plan.get("offline_sequence", {}))


def _observation_fields(path: pathlib.Path) -> dict:
    return PRIMARY._observation_fields(path)


def _validate_cpp_sequence(sequence: dict, plan: dict) -> None:
    offline = plan["offline_sequence"]
    request = sequence.get("request", {})
    if sequence.get("schema") != 5 or \
            sequence.get("profile") != _HOLDOUT_PROFILE or \
            request.get("guard_sample_count") != 32 or \
            request.get("lfsr_order") != 6 or \
            request.get("feedback_mask") != 51 or \
            request.get("seed") != 1 or request.get("phase") != 21 or \
            request.get("offline_sequence_semantic_sha256") != \
            _HOLDOUT_SEQUENCE_SEMANTIC_SHA256 or \
            len(sequence.get("samples", [])) != _HOLDOUT_SAMPLE_COUNT or \
            len(sequence.get("blocks", [])) != _HOLDOUT_BLOCK_COUNT or \
            int(sequence.get("summary", {}).get("net_x_counts", 1)) != 0 or \
            int(sequence.get("summary", {}).get(
                "max_abs_prefix_x_counts", 2)) != 1:
        raise ValueError("C++ Physical B holdout sequence 身份非法")
    for index, (actual, expected) in enumerate(zip(
            sequence["samples"], offline["samples"], strict=True)):
        if int(actual.get("sample_index", -1)) != index or \
                int(actual.get("block_id", -1)) != \
                int(expected.get("block_id", -2)) or \
                actual.get("phase") != expected.get("phase") or \
                int(actual.get("dx_counts", 2)) != \
                int(expected.get("command_dx_counts", 3)) or \
                int(actual.get("dy_counts", 2)) != 0:
            raise ValueError("C++ holdout sequence 偏离冻结 offline exact sequence")


def _validate_holdout_artifacts(
        run_directory: pathlib.Path,
        plan_path: pathlib.Path) -> dict:
    run = run_directory.resolve()
    if not run.is_dir() or run.parent == run:
        raise ValueError("Physical B holdout Run 必须是既有非根目录")
    paths = {
        "task": run / "task.json",
        "plan": plan_path.resolve(),
        "sequence": run / "sequence.json",
        "binding": run / "probe-binding.json",
        "report": run / "command-report.json",
        "safety_ledger": run / "safety-ledger.json",
        "manifest": run / "pixel-evidence" / "manifest.json",
        "launch_summary": run / "launch-summary.json",
        "observation": run / "OBSERVATION.md",
    }
    task = _load_json(paths["task"], "holdout task")
    plan = _load_json(paths["plan"], "holdout plan")
    sequence = _load_json(paths["sequence"], "holdout sequence")
    binding = _load_json(paths["binding"], "holdout binding")
    report = _load_json(paths["report"], "holdout command report")
    ledger = _load_json(paths["safety_ledger"], "holdout safety ledger")
    manifest = _load_json(paths["manifest"], "holdout sidecar manifest")
    launch = _load_json(paths["launch_summary"], "holdout launch summary")
    observation = _observation_fields(paths["observation"])
    _validate_plan(plan)
    if task.get("schema_version") != 7 or \
            task.get("evidence_type") != "mouse_effect_probe_b_task" or \
            task.get("status") != "PREPARED" or \
            task.get("experiment") != "physical_b_cross_run_holdout" or \
            task.get("run_role") != "cross_run_holdout" or \
            task.get("dispatch_mode") != "physical_b" or \
            task.get("profile") != _HOLDOUT_PROFILE or \
            task.get("cross_run_holdout_prepare_authorized") is not True or \
            task.get("holdout_used_for_tuning") is not False or \
            int(task.get("sequence_sample_count", -1)) != \
            _HOLDOUT_SAMPLE_COUNT or \
            int(task.get("expected_nonzero_transition_count", -1)) != \
            _EXPECTED_TRANSITION_COUNT:
        raise ValueError("Physical B holdout task 身份或授权边界非法")
    primary = plan["primary"]
    if task.get("scope_id") != primary["scope_id"] or \
            task.get("run_uuid") == primary["run_uuid"] or \
            int(task.get("activation_epoch", -1)) == \
            int(primary["activation_epoch"]):
        raise ValueError("holdout 未使用独立 Run UUID/activation 或 scope 漂移")
    task_primary = task.get("primary", {})
    task_holdout = task.get("holdout", {})
    if task_primary.get("timing_observation") != \
            primary.get("timing_observation") or \
            task_primary.get("source_clock_session_id") != \
            primary.get("source_clock_session_id") or \
            task_holdout.get("independence_contract_semantic_sha256") != \
            plan.get("contract", {}).get("contract_semantic_sha256"):
        raise ValueError("holdout task/plan timing observation 合同漂移")
    files = task.get("files", {})
    for key, path_key in (
            ("sequence", "sequence"),
            ("probe_binding", "binding"),
            ("holdout_plan", "plan"),
            ("holdout_analyzer", "analyzer")):
        if path_key == "analyzer":
            path = pathlib.Path(__file__).resolve()
        else:
            path = paths[path_key]
        PRIMARY._assert_file_identity(path, files.get(key), key)
    _validate_cpp_sequence(sequence, plan)
    if task.get("sequence_sha256") != sequence.get("sequence_sha256") or \
            task.get("offline_sequence_semantic_sha256") != \
            _HOLDOUT_SEQUENCE_SEMANTIC_SHA256:
        raise ValueError("holdout task/sequence 语义绑定不一致")
    result = report.get("result", {})
    events = result.get("events")
    if report.get("dispatch_mode") != "physical_b" or \
            report.get("profile") != _HOLDOUT_PROFILE or \
            report.get("run_uuid") != task.get("run_uuid") or \
            int(report.get("activation_epoch", 0)) != \
            int(task.get("activation_epoch", -1)) or \
            report.get("sequence_sha256") != task.get("sequence_sha256") or \
            result.get("state") != "completed" or \
            result.get("complete") is not True or \
            result.get("stop_reason") != "normal_completion" or \
            int(result.get("consumed_sample_count", -1)) != \
            _HOLDOUT_SAMPLE_COUNT or \
            int(result.get("cumulative_requested_x_counts", 1)) != 0 or \
            int(result.get("cumulative_backend_completed_x_counts", 1)) != 0 or \
            not isinstance(events, list) or len(events) != _HOLDOUT_SAMPLE_COUNT:
        raise ValueError("Physical B holdout command report 不完整或非净零")
    independence = validate_cross_run_timing_observation(
        primary,
        str(task["run_uuid"]),
        int(task["activation_epoch"]),
        events,
    )
    holdout_session = str(
        independence["holdout"]["source_clock_session_id"])
    if ledger.get("run_uuid") != task.get("run_uuid") or \
            ledger.get("probe_stop_reason") != "normal_completion" or \
            ledger.get("recording_failed") is not False or \
            int(ledger.get("dropped_observation_count", -1)) != 0 or \
            ledger.get("monitor_packet_recording_failed") is not False or \
            int(ledger.get("dropped_monitor_packet_count", -1)) != 0:
        raise ValueError("Physical B holdout safety ledger 非法")
    if manifest.get("evidence_type") != "output_off_capture" or \
            manifest.get("physical_output_capability") is not False or \
            int(manifest.get("requested_frame_count", -1)) != \
            int(task.get("sidecar", {}).get("frames", -2)) or \
            int(manifest.get("recorded_frame_count", -1)) != \
            int(task.get("sidecar", {}).get("frames", -2)) or \
            manifest.get("source_binding", {}).get("sha256") != \
            files.get("probe_binding", {}).get("sha256"):
        raise ValueError("Physical B holdout sidecar manifest 非法")
    if launch.get("schema_version") != 5 or \
            launch.get("evidence_type") != "mouse_effect_probe_b_launch" or \
            launch.get("run_uuid") != task.get("run_uuid") or \
            launch.get("run_role") != "cross_run_holdout" or \
            launch.get("status") != "RECORDED_UNANALYZED" or \
            launch.get("stop_reason") != "normal_completion" or \
            int(launch.get("command_event_count", -1)) != \
            _HOLDOUT_SAMPLE_COUNT or \
            int(launch.get("source_timestamp_matched_event_count", -1)) != \
            _HOLDOUT_SAMPLE_COUNT or \
            launch.get("cross_run_independence") != independence:
        raise ValueError("Physical B holdout launch summary 不完整")
    if observation["manual_mouse_or_wasd_used"] or \
            observation["occlusion_or_scene_cut_reported"] or \
            observation["anomaly_or_emergency_stop_reported"]:
        raise ValueError("Physical B holdout 人工观察存在输入污染、遮挡或异常")
    return {
        "paths": paths,
        "task": task,
        "plan": plan,
        "sequence": sequence,
        "offline_sequence": plan["offline_sequence"],
        "binding": binding,
        "report": report,
        "ledger": ledger,
        "manifest": manifest,
        "launch_summary": launch,
        "observation": observation,
        "source_clock_session_id": holdout_session,
        "cross_run_independence": independence,
    }


def _frame_index(manifest: dict) -> dict[int, tuple[int, dict]]:
    frames = manifest.get("frames")
    if not isinstance(frames, list) or len(frames) < _HOLDOUT_SAMPLE_COUNT:
        raise ValueError("Physical B holdout sidecar frames 容量不足")
    result: dict[int, tuple[int, dict]] = {}
    sessions: set[str] = set()
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
            raise ValueError("Physical B holdout sidecar timing/drop 合同非法")
        timestamp = int(frame.get("source_timestamp", 0))
        session = str(frame.get("source_clock_session_id", ""))
        if timestamp <= 0 or timestamp in result or not session:
            raise ValueError("Physical B holdout sidecar timestamp/session 非法或重复")
        sessions.add(session)
        result[timestamp] = (index, frame)
    if len(sessions) != 1:
        raise ValueError("Physical B holdout sidecar source session 不唯一")
    return result


def _match_events(
        loaded: dict,
        frame_by_timestamp: dict[int, tuple[int, dict]]) -> list[
            tuple[dict, dict, dict, int]]:
    events = loaded["report"]["result"]["events"]
    samples = loaded["sequence"]["samples"]
    matched: list[tuple[dict, dict, dict, int]] = []
    previous_manifest_index = -1
    requested_x = 0
    backend_x = 0
    for index, (event, sample) in enumerate(zip(events, samples, strict=True)):
        nominal_x = int(sample.get("dx_counts", 2))
        if int(event.get("sample_index", -1)) != index or \
                int(event.get("block_id", -1)) != \
                int(sample.get("block_id", -2)) or \
                int(event.get("nominal_dx_counts", 2)) != nominal_x or \
                int(event.get("nominal_dy_counts", 2)) != 0 or \
                event.get("run_uuid") != loaded["task"]["run_uuid"] or \
                event.get("sequence_sha256") != \
                loaded["task"]["sequence_sha256"] or \
                event.get("source_timestamp_valid") is not True or \
                event.get("source_clock_status") != "VALID" or \
                event.get("source_time_basis") != "NDI_SDK_SUBMISSION" or \
                int(event.get("source_dropped_frames", -1)) != 0 or \
                int(event.get("transport_dropped_frames", -1)) != 0 or \
                int(event.get("transport_invalid_packets", -1)) != 0 or \
                event.get("safety_allowed") is not True or \
                event.get("mouse_status") != "READY" or \
                event.get("stop_reason") != "none":
            raise ValueError("Physical B holdout event/source/safety 不一致")
        if nominal_x == 0:
            if event.get("dispatch_attempted") is not False or \
                    int(event.get("requested_dx_counts", 1)) != 0 or \
                    event.get("backend_succeeded") is not False or \
                    event.get("protocol_ack_received") is not False:
                raise ValueError("holdout 零 command 被伪造成 dispatch")
        else:
            if abs(nominal_x) != 1 or \
                    event.get("dispatch_attempted") is not True or \
                    int(event.get("requested_dx_counts", 0)) != nominal_x or \
                    event.get("backend_succeeded") is not True or \
                    event.get("protocol_ack_received") is not True:
                raise ValueError("holdout transition 缺少 request/backend/ACK")
            requested_x += nominal_x
            backend_x += nominal_x
        if int(event.get("cumulative_requested_x_counts", 999)) != requested_x or \
                int(event.get("cumulative_backend_completed_x_counts", 999)) != \
                backend_x:
            raise ValueError("holdout cumulative requested/backend 不守恒")
        frame_match = frame_by_timestamp.get(int(event.get("source_timestamp", 0)))
        if frame_match is None:
            raise ValueError("holdout event 缺少 exact timestamp sidecar frame")
        manifest_index, frame = frame_match
        if manifest_index <= previous_manifest_index or \
                str(frame.get("source_clock_session_id")) != \
                str(event.get("source_clock_session_id")):
            raise ValueError("holdout event/frame 顺序或 source session 改变")
        previous_manifest_index = manifest_index
        matched.append((event, sample, frame, manifest_index))
    return matched


def analyze_holdout_run(
        run_directory: pathlib.Path,
        plan_path: pathlib.Path,
        samples_csv_path: pathlib.Path) -> tuple[dict, str]:
    loaded = _validate_holdout_artifacts(run_directory, plan_path)
    frames = _frame_index(loaded["manifest"])
    matched = _match_events(loaded, frames)
    measurements, rows, geometry = PRIMARY._measure_primary_witnesses(
        loaded, matched)
    evaluation = evaluate_holdout_model(
        loaded["offline_sequence"], measurements,
        loaded["plan"]["frozen_f1"])
    if not rows:
        raise ValueError("Physical B holdout 没有 whole-block 输出行")
    csv_buffer = io.StringIO(newline="")
    writer = csv.DictWriter(csv_buffer, fieldnames=list(rows[0].keys()))
    writer.writeheader()
    writer.writerows(rows)
    csv_content = csv_buffer.getvalue()
    result = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_holdout_analysis",
        "status": evaluation["status"],
        "physical_output_capability": False,
        "production_aim_changed": False,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "holdout_used_for_tuning": False,
        "run_uuid": loaded["task"]["run_uuid"],
        "activation_epoch": loaded["task"]["activation_epoch"],
        "scope_id": loaded["task"]["scope_id"],
        "profile": _HOLDOUT_PROFILE,
        "run_role": "cross_run_holdout",
        "source_clock_session_id": loaded["source_clock_session_id"],
        "cross_run_independence": loaded["cross_run_independence"],
        "primary": loaded["plan"]["primary"],
        "contract": holdout_evaluation_contract(),
        "geometry": geometry,
        "source_timestamp_matched_event_count": len(matched),
        "samples_csv_row_count": len(rows),
        "nuisance_pre_guard_row_count":
            _HOLDOUT_BLOCK_COUNT * _GUARD_SAMPLE_COUNT,
        "whole_block_output_row_count":
            _HOLDOUT_BLOCK_COUNT * _BLOCK_OUTPUT_SAMPLE_COUNT,
        "observation": loaded["observation"],
        "evaluation": evaluation,
        "samples_csv": {
            "path": str(samples_csv_path.resolve()),
            "size": len(csv_content.encode("utf-8")),
            "sha256": hashlib.sha256(csv_content.encode("utf-8")).hexdigest(),
        },
        "bindings": {
            "task_file_sha256": _file_sha256(loaded["paths"]["task"]),
            "holdout_plan_file_sha256": _file_sha256(loaded["paths"]["plan"]),
            "sequence_file_sha256": _file_sha256(loaded["paths"]["sequence"]),
            "command_report_file_sha256": _file_sha256(loaded["paths"]["report"]),
            "safety_ledger_file_sha256":
                _file_sha256(loaded["paths"]["safety_ledger"]),
            "sidecar_manifest_file_sha256":
                _file_sha256(loaded["paths"]["manifest"]),
            "launch_summary_file_sha256":
                _file_sha256(loaded["paths"]["launch_summary"]),
            "observation_file_sha256": loaded["observation"]["file_sha256"],
            "analyzer_file_sha256": _file_sha256(
                pathlib.Path(__file__).resolve()),
        },
    }
    result["analysis_semantic_sha256"] = canonical_semantic_sha256(
        result, "analysis_semantic_sha256")
    return result, csv_content


def _parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Physical B cross-Run holdout 冻结绑定与一次性评估")
    subparsers = parser.add_subparsers(dest="command", required=True)
    bind = subparsers.add_parser("bind", help="在 holdout 数据到来前冻结计划")
    bind.add_argument("--primary-analysis", type=pathlib.Path, required=True)
    bind.add_argument("--primary-analysis-sha256", required=True)
    bind.add_argument("--offline-design", type=pathlib.Path, required=True)
    bind.add_argument("--offline-design-sha256", required=True)
    bind.add_argument("--output", type=pathlib.Path, required=True)
    evaluate = subparsers.add_parser("evaluate", help="按冻结计划裁决 holdout")
    evaluate.add_argument("--holdout-run", type=pathlib.Path, required=True)
    evaluate.add_argument("--plan", type=pathlib.Path, required=True)
    evaluate.add_argument("--output", type=pathlib.Path, required=True)
    evaluate.add_argument("--samples-csv", type=pathlib.Path, required=True)
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    try:
        options = _parse_arguments(arguments)
        path_values = [
            value for name, value in vars(options).items()
            if name not in (
                "command", "primary_analysis_sha256",
                "offline_design_sha256") and isinstance(value, pathlib.Path)
        ]
        if any(not value.is_absolute() for value in path_values):
            raise ValueError("Physical B holdout analyzer 所有路径必须是绝对路径")
        if options.output.exists():
            raise ValueError("Physical B holdout analyzer 拒绝覆盖既有输出")
        if options.command == "bind":
            result = build_holdout_plan(
                options.primary_analysis,
                options.primary_analysis_sha256,
                options.offline_design,
                options.offline_design_sha256,
            )
            PRIMARY._write_new_text(
                options.output,
                json.dumps(result, ensure_ascii=False, indent=2,
                           allow_nan=False) + "\n",
            )
            print(
                "Physical B holdout plan frozen: "
                f"primary={result['primary']['run_uuid']}, "
                f"f1={result['primary']['f1_semantic_sha256']}, "
                f"output={options.output}"
            )
            return 0
        if options.samples_csv.exists():
            raise ValueError("Physical B holdout analyzer 拒绝覆盖既有 CSV")
        result, csv_content = analyze_holdout_run(
            options.holdout_run, options.plan, options.samples_csv)
        PRIMARY._write_new_text(options.samples_csv, csv_content)
        result["samples_csv"]["size"] = options.samples_csv.stat().st_size
        result["samples_csv"]["sha256"] = _file_sha256(options.samples_csv)
        result["analysis_semantic_sha256"] = canonical_semantic_sha256(
            result, "analysis_semantic_sha256")
        PRIMARY._write_new_text(
            options.output,
            json.dumps(result, ensure_ascii=False, indent=2,
                       allow_nan=False) + "\n",
        )
        print(
            f"Physical B holdout {result['status']}: "
            f"matched={result['source_timestamp_matched_event_count']}, "
            f"output={options.output}"
        )
        return 0 if result["status"] == "HOLDOUT_GREEN" else 1
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as exception:
        print(f"Physical B holdout analyzer 失败: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
