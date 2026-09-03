#!/usr/bin/env python3
"""Physical B cross-Run holdout 冻结评估合同专项。"""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile

import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载测试模块: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


HOLDOUT = load_module(
    ROOT / "scripts" / "analyze_mouse_effect_probe_b_holdout.py",
    "analyze_mouse_effect_probe_b_holdout",
)
DESIGNER = load_module(
    ROOT / "scripts" / "design_mouse_effect_probe_prbs.py",
    "design_mouse_effect_probe_prbs_for_holdout_tests",
)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical(value: dict, field: str) -> str:
    payload = dict(value)
    payload.pop(field, None)
    return hashlib.sha256(json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")).hexdigest()


def frozen_holdout_sequence() -> dict:
    bits = DESIGNER.generate_maximum_length_period(
        order=6, feedback_mask=0x33, seed=1, phase=21)
    sequence = DESIGNER.build_candidate_sequence(
        period_bits=bits,
        input_definition="cumulative_position_counts",
        guard_sample_count=32,
        pair_repetitions=1,
        role="cross_run_holdout",
    )
    DESIGNER._assign_pair_roles(sequence, ["cross_run_holdout"])
    DESIGNER._bind_sequence_semantics(sequence)
    return sequence


def frozen_f1() -> dict:
    model = {
        "delay_samples": 4,
        "tail_length": 0,
        "include_core": True,
        "include_tail": False,
        "input_shift_samples": 0,
        "fit_roles": ["estimation", "selection"],
        "confirmation_used_for_refit": False,
        "by_witness": {
            "left": {
                "coefficients": [-0.4],
                "gain": -0.4,
                "tail_coefficients": [],
                "design_matrix": {"full_column_rank": True},
            },
            "right": {
                "coefficients": [-0.45],
                "gain": -0.45,
                "tail_coefficients": [],
                "design_matrix": {"full_column_rank": True},
            },
        },
    }
    value = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_f1",
        "status": "F1_CORE",
        "physical_output_capability": False,
        "cross_run_holdout_prepare_authorized": False,
        "production_aim_changed": False,
        "scope_id": "scope-fixture",
        "analysis_contract_semantic_sha256": "a" * 64,
        "core_delay_samples": 4,
        "selected_tail_length": 0,
        "selected_model": model,
        "confirmation_used_for_refit": False,
        "holdout": {
            "used_for_tuning": False,
            "run_required": "different_run_activation_and_session",
            "lfsr": {
                "order": 6,
                "feedback_mask": 51,
                "feedback_mask_hex": "0x33",
                "seed": 1,
                "phase": 21,
                "period_sample_count": 63,
                "recurrence": (
                    "output=state_lsb; feedback=parity(state & feedback_mask); "
                    "state=(state>>1)|(feedback<<(order-1))"
                ),
                "maximum_length_proven_by_state_cycle": True,
            },
            "sequence_semantic_sha256":
                "e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4",
            "delay_and_tail_are_frozen": True,
            "confirmation_used_for_tuning": False,
            "mapping_uncertainty_upper_px": 0.02,
            "max_worst_witness_rmse_px": 0.05,
            "max_worst_block_rmse_px": 0.05,
            "max_worst_witness_max_abs_error_px": 0.08,
            "holdout_used_for_tuning": False,
        },
        "bindings": {
            "run_uuid": "11111111-1111-4111-8111-111111111111",
            "activation_epoch": 100,
            "analyzer_file_sha256": "b" * 64,
        },
    }
    value["f1_semantic_sha256"] = canonical(value, "f1_semantic_sha256")
    return value


def synthetic_measurements(sequence: dict, f1: dict) -> dict[str, list[float]]:
    positions = np.asarray([
        sample["identification_input_x_counts"]
        for sample in sequence["samples"]
    ], dtype=np.float64)
    outputs = {
        "left": np.zeros(len(positions), dtype=np.float64),
        "right": np.zeros(len(positions), dtype=np.float64),
    }
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        for index in range(first - 32, first + 64 + 32):
            offset = index - (first - 1)
            nuisance = 0.015 * int(block["block_id"]) + 0.0002 * offset
            source = index - 4
            outputs["left"][index] = nuisance - 0.4 * positions[source]
            outputs["right"][index] = -0.3 * nuisance - 0.45 * positions[source]
    return {name: values.tolist() for name, values in outputs.items()}


def test_contract_and_frozen_model_green_red() -> None:
    contract = HOLDOUT.holdout_evaluation_contract()
    expect(contract["holdout_used_for_tuning"] is False and
           contract["predictions"] == ["input_forced", "output_free_run"] and
           contract["output_feedback_used"] is False and
           contract["threshold_source"] == "frozen_f1_holdout_budget" and
           contract["different_source_clock_session_required"] is False and
           contract["same_source_clock_session_allowed"] is True and
           contract["different_timing_observation_required"] is True and
           contract["nonoverlapping_source_time_ranges_required"] is True and
           contract["event_frame_source_clock_session_match_required"] is True,
           "holdout 评估必须同时冻结 no-tuning、两种预测与 F1 预算")
    sequence = frozen_holdout_sequence()
    f1 = frozen_f1()
    measurements = synthetic_measurements(sequence, f1)
    result = HOLDOUT.evaluate_holdout_model(sequence, measurements, f1)
    expect(result["status"] == "HOLDOUT_GREEN" and
           result["holdout_used_for_tuning"] is False and
           result["input_forced"] == result["output_free_run"] and
           all(result["budget_checks"].values()),
           "严格由冻结 F1 生成的独立序列必须一次性判绿")

    polluted = copy.deepcopy(measurements)
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        for index in range(first, first + 96):
            polluted["left"][index] += 0.2
    red = HOLDOUT.evaluate_holdout_model(sequence, polluted, f1)
    expect(red["status"] == "HOLDOUT_RED" and
           not all(red["budget_checks"].values()) and
           red["frozen_model_semantic_sha256"] ==
           result["frozen_model_semantic_sha256"],
           "超预算 holdout 必须判红且不得改变冻结模型")


def test_plan_binds_primary_f1_recurrence_and_timing_observation() -> None:
    sequence = frozen_holdout_sequence()
    f1 = frozen_f1()
    with tempfile.TemporaryDirectory(prefix="xen-probe-b-holdout-plan-") as text:
        root = pathlib.Path(text)
        report = {
            "run_uuid": f1["bindings"]["run_uuid"],
            "activation_epoch": f1["bindings"]["activation_epoch"],
            "result": {"events": [
                {
                    "source_clock_session_id": "primary-source-session",
                    "source_time_at_steady_ns": 1000,
                    "source_timestamp": 10000,
                },
                {
                    "source_clock_session_id": "primary-source-session",
                    "source_time_at_steady_ns": 2000,
                    "source_timestamp": 20000,
                },
            ]},
        }
        report_path = root / "command-report.json"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        analysis = {
            "schema_version": 2,
            "evidence_type": "mouse_effect_probe_physical_b_primary_analysis",
            "status": "PRIMARY_CORE_ONLY",
            "physical_output_capability": False,
            "production_aim_changed": False,
            "run_uuid": f1["bindings"]["run_uuid"],
            "activation_epoch": f1["bindings"]["activation_epoch"],
            "scope_id": "scope-fixture",
            "profile": "physical_b_prbs_primary",
            "f1": f1,
            "bindings": {
                "command_report_file_sha256":
                    hashlib.sha256(report_path.read_bytes()).hexdigest(),
                "analyzer_file_sha256": "b" * 64,
            },
            "cross_run_holdout_prepare_authorized": False,
            "holdout_used_for_tuning": False,
        }
        analysis["analysis_semantic_sha256"] = canonical(
            analysis, "analysis_semantic_sha256")
        analysis_path = root / "physical-b-primary-analysis.json"
        analysis_path.write_text(json.dumps(analysis), encoding="utf-8")

        offline = {
            "schema_version": 2,
            "evidence_type": "mouse_effect_probe_physical_b_offline_design",
            "status": "VALID_OFFLINE_DESIGN",
            "physical_output_capability": False,
            "physical_b_launch_authorized": False,
            "production_aim_changed": False,
            "cross_run_holdout_candidate": {
                "role": "cross_run_holdout",
                "input_definition": "cumulative_position_counts",
                "lfsr": f1["holdout"]["lfsr"],
                "sequence": sequence,
            },
        }
        offline["design_semantic_sha256"] = canonical(
            offline, "design_semantic_sha256")
        offline_path = root / "offline-design.json"
        offline_path.write_text(json.dumps(offline), encoding="utf-8")

        analysis_sha = hashlib.sha256(analysis_path.read_bytes()).hexdigest()
        offline_sha = hashlib.sha256(offline_path.read_bytes()).hexdigest()
        plan = HOLDOUT.build_holdout_plan(
            analysis_path, analysis_sha, offline_path, offline_sha)
        expect(plan["status"] == "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE" and
               plan["primary"]["source_clock_session_id"] ==
               "primary-source-session" and
               plan["primary"]["timing_observation"] == {
                   "identity_basis":
                       "run_uuid_activation_epoch_source_time_range",
                   "source_clock_session_id": "primary-source-session",
                   "source_time_at_steady_ns": {"first": 1000, "last": 2000},
                   "source_timestamp": {"first": 10000, "last": 20000},
               } and
               plan["primary"]["run_uuid"] == f1["bindings"]["run_uuid"] and
               plan["sequence"]["sequence_semantic_sha256"] ==
               sequence["sequence_semantic_sha256"] and
               plan["holdout_used_for_tuning"] is False,
               "holdout plan 必须绑定 Primary/F1、独立 recurrence 与 source session")

        rejected = False
        try:
            HOLDOUT.build_holdout_plan(
                analysis_path, "0" * 64, offline_path, offline_sha)
        except ValueError:
            rejected = True
        expect(rejected, "Primary analysis 文件身份漂移必须 fail closed")


def test_cross_run_timing_observation_allows_same_clock_epoch() -> None:
    primary = {
        "run_uuid": "11111111-1111-4111-8111-111111111111",
        "activation_epoch": 100,
        "source_clock_session_id": "stable-source-clock-epoch",
        "timing_observation": {
            "identity_basis":
                "run_uuid_activation_epoch_source_time_range",
            "source_clock_session_id": "stable-source-clock-epoch",
            "source_time_at_steady_ns": {"first": 1000, "last": 2000},
            "source_timestamp": {"first": 10000, "last": 20000},
        },
    }
    holdout_events = [
        {
            "source_clock_session_id": "stable-source-clock-epoch",
            "source_time_at_steady_ns": 3000,
            "source_timestamp": 30000,
        },
        {
            "source_clock_session_id": "stable-source-clock-epoch",
            "source_time_at_steady_ns": 4000,
            "source_timestamp": 40000,
        },
    ]
    audit = HOLDOUT.validate_cross_run_timing_observation(
        primary,
        "22222222-2222-4222-8222-222222222222",
        200,
        holdout_events,
    )
    expect(audit["different_run_uuid"] is True and
           audit["different_activation_epoch"] is True and
           audit["different_timing_observation"] is True and
           audit["different_source_clock_session"] is False and
           audit["same_source_clock_session_allowed"] is True and
           audit["source_time_ranges_overlap"] is False and
           audit["source_timestamp_ranges_overlap"] is False,
           "稳定 clock epoch 不得被误作复用的 capture/timing observation")

    deletion_cases = []
    deletion_cases.append((
        "复用 Run UUID",
        primary["run_uuid"],
        200,
        holdout_events,
    ))
    deletion_cases.append((
        "复用 activation",
        "22222222-2222-4222-8222-222222222222",
        primary["activation_epoch"],
        holdout_events,
    ))
    steady_overlap = copy.deepcopy(holdout_events)
    steady_overlap[0]["source_time_at_steady_ns"] = 1500
    deletion_cases.append((
        "mapped steady 时间窗重叠",
        "22222222-2222-4222-8222-222222222222",
        200,
        steady_overlap,
    ))
    source_overlap = copy.deepcopy(holdout_events)
    source_overlap[0]["source_timestamp"] = 15000
    deletion_cases.append((
        "source timestamp 时间窗重叠",
        "22222222-2222-4222-8222-222222222222",
        200,
        source_overlap,
    ))
    mixed_session = copy.deepcopy(holdout_events)
    mixed_session[1]["source_clock_session_id"] = "other-source-clock-epoch"
    deletion_cases.append((
        "Run 内混杂 clock epoch",
        "22222222-2222-4222-8222-222222222222",
        200,
        mixed_session,
    ))
    empty_session = copy.deepcopy(holdout_events)
    empty_session[0]["source_clock_session_id"] = ""
    deletion_cases.append((
        "空 clock epoch",
        "22222222-2222-4222-8222-222222222222",
        200,
        empty_session,
    ))
    for description, run_uuid, activation_epoch, events in deletion_cases:
        rejected = False
        try:
            HOLDOUT.validate_cross_run_timing_observation(
                primary, run_uuid, activation_epoch, events)
        except ValueError:
            rejected = True
        expect(rejected, f"{description} 必须 fail closed")


def test_sidecar_frame_sessions_must_be_unique_and_nonempty() -> None:
    frames = [
        {
            "index": index,
            "source_timestamp_valid": True,
            "source_time_timing_valid": True,
            "source_clock_status": "VALID",
            "source_time_basis": "NDI_SDK_SUBMISSION",
            "source_dropped_frames": 0,
            "transport_dropped_frames": 0,
            "transport_invalid_packets": 0,
            "source_timestamp": 10000 + index,
            "source_clock_session_id": "stable-source-clock-epoch",
        }
        for index in range(288)
    ]
    expect(len(HOLDOUT._frame_index({"frames": frames})) == 288,
           "单一非空 sidecar clock epoch 应通过")
    for description, replacement in (
            ("混杂 sidecar clock epoch", "other-source-clock-epoch"),
            ("空 sidecar clock epoch", "")):
        invalid = copy.deepcopy(frames)
        invalid[-1]["source_clock_session_id"] = replacement
        rejected = False
        try:
            HOLDOUT._frame_index({"frames": invalid})
        except ValueError:
            rejected = True
        expect(rejected, f"{description} 必须 fail closed")


def main() -> int:
    test_contract_and_frozen_model_green_red()
    test_plan_binds_primary_f1_recurrence_and_timing_observation()
    test_cross_run_timing_observation_allows_same_clock_epoch()
    test_sidecar_frame_sessions_must_be_unique_and_nonempty()
    print("Physical B holdout analysis tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
