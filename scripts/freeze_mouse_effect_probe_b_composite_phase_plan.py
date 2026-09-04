#!/usr/bin/env python3
"""生成 composite-phase plan seed，或用同机 preflight 封存最终 plan。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys
import uuid
from typing import Any


Q32 = 1 << 32
PHASE_CELLS = ("P1_8", "P3_8", "P5_8", "P7_8")
PHASE_NUMERATORS = dict(zip(PHASE_CELLS, (1, 3, 5, 7)))
WILLIAMS_ROWS = (
    ("P1_8", "P3_8", "P7_8", "P5_8"),
    ("P3_8", "P5_8", "P1_8", "P7_8"),
    ("P5_8", "P7_8", "P3_8", "P1_8"),
    ("P7_8", "P1_8", "P5_8", "P3_8"),
)
SEEN_RUN_UUID = "574e8e49-4219-4424-8a59-d3b1bf345a11"
SEEN_ARTIFACT_SHA256 = (
    "5194a97c01420f693275e9c7dc4130b03e526e1bfd7aa0de1376e33c2ed1000e",
    "06a0a8c6db6d16f086b638c7a8bf6b45861d2e51ca714cd82d627cf477110b47",
    "126d4b7590b7a88749158cba1a8ccf3bc8f8f15db817bc36ffbff479b4038c52",
    "d7171ade6196f32a69c9fcd686827041322c7307e540f2b514e86abd626e7abc",
)
ORDER_FIELDS = (
    "pulse_id", "sequence_index", "block_id", "phase_cell",
    "repeat_index", "row_index", "position_index", "polarity_order",
    "sentinel_position", "pulse_ordinal", "command_dx_counts",
    "command_dy_counts",
)


class PlanError(ValueError):
    pass


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def read_object(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PlanError(f"{label} 无法读取: {error}") from error
    if not isinstance(value, dict):
        raise PlanError(f"{label} 必须是 JSON object")
    return value


def write_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    if not path.is_absolute():
        raise PlanError("output 必须为绝对路径")
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if path.exists() or pending.exists():
        raise PlanError("output 已存在，拒绝覆盖")
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


def schema_semantic_sha256() -> str:
    return canonical_sha256({
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration",
        "run_role": "CALIBRATION_DELETION",
        "phase_policy_id": "b-meas-phase-d1-v2",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
    })


def planned_design() -> tuple[list[dict[str, Any]],
                              list[dict[str, Any]], list[str]]:
    pulses: list[dict[str, Any]] = []
    sequence_index = 0

    def append_pair(block_id: str, phase_cell: str, repeat_index: int,
                    row_index: int | None, position_index: int | None,
                    polarity_order: str,
                    sentinel_position: str | None) -> None:
        nonlocal sequence_index
        commands = ((1, -1) if polarity_order == "positive_first"
                    else (-1, 1))
        for pulse_ordinal, command in enumerate(commands, 1):
            pulses.append({
                "pulse_id": f"{block_id}-{pulse_ordinal}",
                "sequence_index": sequence_index,
                "block_id": block_id,
                "phase_cell": phase_cell,
                "repeat_index": repeat_index,
                "row_index": row_index,
                "position_index": position_index,
                "polarity_order": polarity_order,
                "sentinel_position": sentinel_position,
                "pulse_ordinal": pulse_ordinal,
                "command_dx_counts": command,
                "command_dy_counts": 0,
            })
            sequence_index += 1

    append_pair("S-BEGIN", "P1_8", 0, None, None,
                "positive_first", "begin")
    for row_index, row in enumerate(WILLIAMS_ROWS):
        if row_index == 2:
            append_pair("S-MIDDLE", "P1_8", 0, None, None,
                        "negative_first", "middle")
        for position_index, phase_cell in enumerate(row):
            append_pair(
                f"R{row_index}-{position_index}-{phase_cell}", phase_cell,
                row_index + 1, row_index, position_index,
                ("positive_first" if (row_index + position_index) % 2 == 0
                 else "negative_first"), None)
    append_pair("S-END", "P1_8", 0, None, None,
                "positive_first", "end")

    half_width = Q32 // 64
    controls = [{
        "control_id": f"NC-{cell}",
        "phase_cell": cell,
        "window_id": f"NC-{cell}",
        "scheduled_phase_interval_q32": {
            "lower_closed": PHASE_NUMERATORS[cell] * Q32 // 8 - half_width,
            "upper_closed": PHASE_NUMERATORS[cell] * Q32 // 8 + half_width,
        },
        "command_dx_counts": 0,
        "command_dy_counts": 0,
        "mouse_or_kmbox_event_emitted": False,
    } for cell in PHASE_CELLS]
    controls_after = {
        9: "NC-P1_8", 17: "NC-P3_8", 27: "NC-P5_8", 35: "NC-P7_8",
    }
    order: list[str] = []
    for pulse in pulses:
        order.append(pulse["pulse_id"])
        control = controls_after.get(pulse["sequence_index"])
        if control is not None:
            order.append(control)
    return pulses, controls, order


def validate_sequence(sequence: dict[str, Any], order: list[str],
                      pulses: list[dict[str, Any]]) -> None:
    request = sequence.get("request")
    windows = sequence.get("windows")
    samples = sequence.get("samples")
    summary = sequence.get("summary")
    expected_request = {
        "predictor_sample_count": 1,
        "window_sample_count": 6,
        "single_magnitude_counts": 1,
        "issue_lead_ns": 400_000,
        "target_tolerance_q32": Q32 // 16,
        "active_guard_ns": 300_000,
        "max_wake_lateness_ns": 150_000,
        "max_event_interval_width_ns": 100_000,
        "max_active_wait_ns_per_event": 350_000,
        "max_active_wait_ns_total": 42 * 350_000,
        "timer_mode": "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL",
    }
    if (sequence.get("schema") != 7 or
            sequence.get("profile") !=
            "physical_b_composite_phase_calibration" or
            request != expected_request or not is_sha256(
                sequence.get("sequence_sha256")) or
            not isinstance(windows, list) or len(windows) != 42 or
            not isinstance(samples, list) or len(samples) != 295 or
            summary != {"net_x_counts": 0,
                        "max_abs_prefix_x_counts": 1}):
        raise PlanError("sequence header/request/summary 合同无效")
    pulse_by_id = {pulse["pulse_id"]: pulse for pulse in pulses}
    for ordinal, (window, expected_id) in enumerate(zip(windows, order)):
        first = 1 + ordinal * 7
        if (not isinstance(window, dict) or
                window.get("window_ordinal") != ordinal or
                window.get("window_id") != expected_id or
                window.get("first_sample_index") != first or
                window.get("sample_count") != 7 or
                window.get("phase_cell") not in PHASE_CELLS or
                window.get("negative_control") !=
                expected_id.startswith("NC-")):
            raise PlanError("sequence window identity/order 合同无效")
        planned_dx = (0 if expected_id.startswith("NC-")
                      else pulse_by_id[expected_id]["command_dx_counts"])
        expected_samples = samples[first:first + 7]
        if (len(expected_samples) != 7 or
                expected_samples[0].get("phase") != "pulse" or
                expected_samples[0].get("dx_counts") != planned_dx or
                any(sample.get("dy_counts") != 0 for sample in
                    expected_samples) or
                any(sample.get("dx_counts") != 0 or
                    sample.get("phase") != "response"
                    for sample in expected_samples[1:])):
            raise PlanError("sequence window sample/X/Y 合同无效")


def validate_capture_policy(policy: dict[str, Any]) -> None:
    semantic = policy.get("semantic_sha256")
    semantic_input = dict(policy)
    semantic_input.pop("semantic_sha256", None)
    if not is_sha256(semantic) or semantic != canonical_sha256(semantic_input):
        raise PlanError("capture policy semantic SHA-256 漂移")


def build_plan(options: argparse.Namespace) -> dict[str, Any]:
    sequence_path = options.sequence.resolve()
    binder_path = options.binder.resolve()
    evaluator_path = options.evaluator.resolve()
    producer_path = options.producer.resolve()
    report_verifier_path = options.report_verifier.resolve()
    capture_policy_path = options.capture_policy.resolve()
    for path, label in ((sequence_path, "sequence"),
                        (binder_path, "binder"),
                        (evaluator_path, "evaluator"),
                        (producer_path, "ledger producer"),
                        (report_verifier_path, "report verifier"),
                        (capture_policy_path, "capture policy")):
        if not path.is_file():
            raise PlanError(f"{label} 不是普通文件")
    sequence = read_object(sequence_path, "sequence")
    capture_policy = read_object(capture_policy_path, "capture policy")
    pulses, controls, order = planned_design()
    validate_sequence(sequence, order, pulses)
    validate_capture_policy(capture_policy)
    try:
        run_uuid = str(uuid.UUID(options.run_uuid))
    except ValueError as error:
        raise PlanError("run UUID 无效") from error
    if (run_uuid != options.run_uuid or run_uuid == SEEN_RUN_UUID or
            options.activation_epoch <= 0 or
            not is_sha256(options.scope_id)):
        raise PlanError("Run identity/scope/deny-list 合同无效")

    preflight_hash: str | None = None
    frozen_ns: int | None = None
    status = "AWAITING_AUXILIARY_PREFLIGHT"
    if options.preflight is not None:
        preflight_path = options.preflight.resolve()
        preflight = read_object(preflight_path, "scheduler preflight")
        if (preflight.get("schema_version") != 1 or
                preflight.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_scheduler_preflight" or
                preflight.get("status") != "PASS" or
                preflight.get("run_uuid") != run_uuid or
                preflight.get("activation_epoch") !=
                options.activation_epoch or
                preflight.get("sequence_semantic_sha256") !=
                sequence["sequence_sha256"] or
                preflight.get("physical_output_capability") is not False or
                preflight.get("physical_dispatch_count") != 0):
            raise PlanError("scheduler preflight identity/result 无效")
        preflight_hash = file_sha256(preflight_path)
        if options.frozen_at_utc_unix_ns is None or \
                options.frozen_at_utc_unix_ns <= 0:
            raise PlanError("最终 plan 必须显式给出正整数 freeze UTC")
        frozen_ns = options.frozen_at_utc_unix_ns
        status = "FROZEN_BEFORE_CAPTURE"
    elif options.frozen_at_utc_unix_ns is not None:
        raise PlanError("plan seed 不得预填 freeze UTC")

    order_manifest = [
        {field: pulse[field] for field in ORDER_FIELDS}
        for pulse in pulses
    ]
    request = sequence["request"]
    plan: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration_plan",
        "run_role": "CALIBRATION_DELETION",
        "run_uuid": run_uuid,
        "activation_epoch": options.activation_epoch,
        "scope_id": options.scope_id,
        "status": status,
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "frozen_at_utc_unix_ns": frozen_ns,
        "seen_diagnosis_denylist": {
            "run_uuids": [SEEN_RUN_UUID],
            "artifact_sha256s": list(SEEN_ARTIFACT_SHA256),
        },
        "phase_policy": {
            "policy_id": "b-meas-phase-d1-v2",
            "phase_scale": "Q0.32_CYCLE",
            "phase_cells": [
                {"phase_cell": cell,
                 "center_numerator": PHASE_NUMERATORS[cell],
                 "center_denominator": 8}
                for cell in PHASE_CELLS
            ],
            "target_tolerance_q32": Q32 // 16,
            "minimum_repeats_per_phase_and_sign": 4,
            "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
            "counterbalanced_rows": [list(row) for row in WILLIAMS_ROWS],
            "position_balance_required": True,
            "directed_predecessor_balance_required": True,
            "required_sentinel_positions": ["begin", "middle", "end"],
            "phase_assignment_selected_before_capture": True,
            "per_pulse_or_block_tuning_allowed": False,
        },
        "model_policy": {
            "m_minus": "fixed_source_sample_onset",
            "m_plus":
                "single_global_completion_relative_transition_interval",
            "output_feedback_used": False,
            "confirmation_or_validation_refit_allowed": False,
            "plant_nonlinearity_claimed": False,
            "unique_delay_or_measurement_model_claimed": False,
        },
        "capture_policy": capture_policy,
        "command_policy": {
            "single_magnitude_counts": 1,
            "zero_y_required": True,
            "net_x_required": 0,
            "fixed_speed_gate_used": False,
        },
        "measurement_policy": {
            "transition_rule": "EXACT_BGR_SHA256_FIRST_CHANGE",
            "anchor_sample_index": 0,
            "settled_sample": "LAST_QUALIFIED_SOURCE_EVENT",
            "window_sample_count": 6,
            "phase_interval_arithmetic":
                "CONSERVATIVE_INTEGER_Q0_32_FROM_SOURCE_TIMESTAMP_RATE_AND_BOUNDARY_OFFSET",
        },
        "sequence_binding": {
            "sequence_schema": 7,
            "sequence_profile": sequence["profile"],
            "sequence_file_sha256": file_sha256(sequence_path),
            "sequence_semantic_sha256": sequence["sequence_sha256"],
            "sample_count": len(sequence["samples"]),
            "window_count": len(sequence["windows"]),
            "window_order": order,
        },
        "scheduler_policy": {
            "clock_kind": "WINDOWS_QPC",
            "timer_mode": request["timer_mode"],
            "deadline_basis":
                "PREDICTOR_NEXT_NDI_SUBMISSION_BOUNDARY",
            "issue_lead_ns": request["issue_lead_ns"],
            "issue_lead_applies_to": "NONZERO_PULSE_ONLY",
            "negative_control_marker_lead_ns": 0,
            "target_tolerance_q32": request["target_tolerance_q32"],
            "active_guard_ns": request["active_guard_ns"],
            "max_wake_lateness_ns": request["max_wake_lateness_ns"],
            "max_event_interval_width_ns":
                request["max_event_interval_width_ns"],
            "max_active_wait_ns_per_event":
                request["max_active_wait_ns_per_event"],
            "max_active_wait_ns_total":
                request["max_active_wait_ns_total"],
            "preflight_required": True,
            "preflight_file_sha256": preflight_hash,
            "per_event_tuning_allowed": False,
            "process_priority": "NORMAL",
            "thread_priority": "NORMAL",
            "cpu_affinity_used": False,
            "time_begin_period_used": False,
            "periodic_timer_used": False,
        },
        "pulses": pulses,
        "negative_controls": controls,
        "window_order": order,
        "seal": {
            "schema_semantic_sha256": schema_semantic_sha256(),
            "evaluator_file_sha256": file_sha256(evaluator_path),
            "binder_file_sha256": file_sha256(binder_path),
            "producer_file_sha256": file_sha256(producer_path),
            "report_verifier_file_sha256":
                file_sha256(report_verifier_path),
            "order_manifest_sha256": canonical_sha256(order_manifest),
            "model_semantic_sha256": None,
            "response_revealed_before_freeze": False,
        },
    }
    semantic_field = ("plan_semantic_sha256" if preflight_hash is not None
                      else "plan_seed_semantic_sha256")
    plan[semantic_field] = canonical_sha256(plan)
    return plan


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Prepare/freeze composite-phase sealed plan")
    parser.add_argument("--sequence", required=True, type=pathlib.Path)
    parser.add_argument("--capture-policy", required=True,
                        type=pathlib.Path)
    parser.add_argument("--binder", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--report-verifier", required=True,
                        type=pathlib.Path)
    parser.add_argument("--run-uuid", required=True)
    parser.add_argument("--activation-epoch", required=True, type=int)
    parser.add_argument("--scope-id", required=True)
    parser.add_argument("--preflight", type=pathlib.Path)
    parser.add_argument("--frozen-at-utc-unix-ns", type=int)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args()
    try:
        plan = build_plan(options)
        write_atomic(options.output, plan)
    except (PlanError, OSError, UnicodeError, ValueError) as error:
        print(f"composite plan 未生成: {error}", file=sys.stderr)
        return 2
    print(f"composite plan 已原子创建: {options.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
