#!/usr/bin/env python3
"""把三个只读账本绑定为 composite-phase calibration evidence。"""

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
MIN_SOURCE_CLOCK_RATE_Q32 = 99 * Q32 // 100
MAX_SOURCE_CLOCK_RATE_Q32 = (101 * Q32 + 99) // 100
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


class BinderError(ValueError):
    """携带稳定 reason code 的 fail-closed 账本错误。"""

    def __init__(self, reason_code: str, message: str) -> None:
        super().__init__(message)
        self.reason_code = reason_code


class OutputCollisionError(ValueError):
    """输出路径已经存在。"""


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _schema_semantic_sha256() -> str:
    return _canonical_sha256({
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration",
        "run_role": "CALIBRATION_DELETION",
        "phase_policy_id": "b-meas-phase-d1-v2",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
    })


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def _exact_int(value: Any, context: str,
               reason_code: str = "INPUT_LEDGER_INVALID") -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise BinderError(reason_code, f"{context} 必须是精确 JSON integer")
    return value


def _reject(condition: bool, reason_code: str, message: str) -> None:
    if condition:
        raise BinderError(reason_code, message)


def _load_json(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BinderError(
            "INPUT_LEDGER_INVALID", f"{label} 无法读取: {error}") from error
    if not isinstance(value, dict):
        raise BinderError("INPUT_LEDGER_INVALID", f"{label} 必须是 object")
    return value


def _verify_semantic(value: dict[str, Any], field: str,
                     reason_code: str) -> str:
    claimed = value.get(field)
    semantic_input = dict(value)
    semantic_input.pop(field, None)
    _reject(not _is_sha256(claimed) or
            claimed != _canonical_sha256(semantic_input),
            reason_code, f"{field} 漂移")
    assert isinstance(claimed, str)
    return claimed


def _interval(value: Any, context: str,
              reason_code: str = "INPUT_LEDGER_INVALID") -> tuple[int, int]:
    _reject(not isinstance(value, dict), reason_code, f"{context} 缺失")
    assert isinstance(value, dict)
    lower = _exact_int(value.get("lower_closed"), f"{context} lower",
                       reason_code)
    upper = _exact_int(value.get("upper_closed"), f"{context} upper",
                       reason_code)
    _reject(lower > upper, reason_code, f"{context} 端点倒置")
    return lower, upper


def _ceil_ratio(numerator: int, denominator: int) -> int:
    return (numerator + denominator - 1) // denominator


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    if not path.is_absolute():
        raise OutputCollisionError("output 必须是绝对路径")
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if path.exists() or pending.exists():
        raise OutputCollisionError("output 必须是尚不存在的路径")
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


def _validate_identity(value: dict[str, Any], plan: dict[str, Any],
                       label: str) -> None:
    _reject(value.get("run_uuid") != plan.get("run_uuid") or
            value.get("activation_epoch") != plan.get("activation_epoch") or
            value.get("scope_id") != plan.get("scope_id"),
            "INPUT_IDENTITY_MISMATCH", f"{label} Run identity 漂移")


def _order_manifest_sha256(pulses: list[Any]) -> str:
    return _canonical_sha256([
        {field: (pulse.get(field) if isinstance(pulse, dict) else None)
         for field in ORDER_FIELDS}
        for pulse in pulses
    ])


def _validate_capture_policy(binding: Any) -> str:
    _reject(not isinstance(binding, dict), "CAPTURE_BINDING_INVALID",
            "capture policy 缺失")
    assert isinstance(binding, dict)
    semantic = _verify_semantic(
        binding, "semantic_sha256", "CAPTURE_BINDING_INVALID")
    geometry = binding.get("source_geometry")
    roi = binding.get("roi_geometry")
    rate = binding.get("source_rate")
    stack = binding.get("capture_stack")
    _reject(not isinstance(geometry, list) or len(geometry) != 2 or
            any(not isinstance(item, int) or isinstance(item, bool) or
                item <= 0 for item in geometry) or
            not isinstance(roi, list) or len(roi) != 4 or
            any(not isinstance(item, int) or isinstance(item, bool)
                for item in roi) or roi[0] < 0 or roi[1] < 0 or
            roi[2] <= 0 or roi[3] <= 0 or
            roi[0] + roi[2] > geometry[0] or
            roi[1] + roi[3] > geometry[1] or
            not isinstance(rate, dict) or
            not isinstance(rate.get("numerator"), int) or
            isinstance(rate.get("numerator"), bool) or
            rate.get("numerator", 0) <= 0 or
            not isinstance(rate.get("denominator"), int) or
            isinstance(rate.get("denominator"), bool) or
            rate.get("denominator", 0) <= 0 or
            not isinstance(stack, dict),
            "CAPTURE_BINDING_INVALID", "capture geometry/rate/stack 无效")
    configured = stack.get("capture_method_configured")
    resolved = stack.get("capture_method_resolved")
    _reject(configured not in ("AUTO", "DXGI", "WGC") or
            resolved not in ("DXGI", "WGC") or
            (configured != "AUTO" and configured != resolved) or
            any(not isinstance(stack.get(field), str) or
                not stack.get(field)
                for field in ("producer", "producer_version")) or
            binding.get("boundary_semantic") != "NDI_SDK_SUBMISSION_UTC" or
            binding.get("boundary_claim_limit") !=
                "use_exact_selected_semantic_only" or
            binding.get("boundary_is_capture_or_exposure") is not False or
            binding.get("ndi_frame_sync_used") is not False or
            binding.get("pixel_format") != "CPU_BGR" or
            not isinstance(binding.get("source_name"), str) or
            not binding.get("source_name"),
            "CAPTURE_BINDING_INVALID",
            "capture provenance 不是冻结的 submission-only 合同")
    for field in ("scene_binding_sha256",):
        _reject(not _is_sha256(binding.get(field)),
                "CAPTURE_BINDING_INVALID", f"{field} 无效")
    for field in ("clock_mapping_policy_id",):
        _reject(not isinstance(binding.get(field), str) or
                not binding.get(field), "CAPTURE_BINDING_INVALID",
                f"{field} 缺失")
    dynamic_fields = {
        "capture_policy_semantic_sha256", "clock_mapping_evidence_sha256",
        "clock_mapping_stale", "completion_clock_session_id",
        "submission_clock_session_id", "mapping_segment_id",
    }
    _reject(any(field in binding for field in dynamic_fields),
            "CAPTURE_BINDING_INVALID",
            "Prepare capture policy 不得预填辅机动态 session/mapping")
    for field in ("left_witness_roi", "right_witness_roi"):
        witness = binding.get(field)
        _reject(not isinstance(witness, list) or len(witness) != 4 or
                any(not isinstance(item, int) or isinstance(item, bool)
                    for item in witness) or
                witness[0] < 0 or witness[1] < 0 or
                witness[2] <= 0 or witness[3] <= 0 or
                witness[0] + witness[2] > roi[2] or
                witness[1] + witness[3] > roi[3],
                "CAPTURE_BINDING_INVALID", f"{field} 无效")
    left = binding["left_witness_roi"]
    right = binding["right_witness_roi"]
    overlap = (max(left[0], right[0]) <
               min(left[0] + left[2], right[0] + right[2]) and
               max(left[1], right[1]) <
               min(left[1] + left[3], right[1] + right[3]))
    _reject(overlap, "CAPTURE_BINDING_INVALID", "witness ROI 重叠")
    return semantic


def _validate_capture_binding(binding: Any,
                              policy: dict[str, Any]) -> str:
    _reject(not isinstance(binding, dict), "CAPTURE_BINDING_INVALID",
            "actual capture binding 缺失")
    assert isinstance(binding, dict)
    semantic = _verify_semantic(
        binding, "semantic_sha256", "CAPTURE_BINDING_INVALID")
    policy_semantic = policy["semantic_sha256"]
    _reject(binding.get("capture_policy_semantic_sha256") !=
                policy_semantic,
            "CAPTURE_BINDING_INVALID", "actual binding 未绑定冻结 policy")
    for field, expected in policy.items():
        if field == "semantic_sha256":
            continue
        _reject(binding.get(field) != expected,
                "CAPTURE_BINDING_INVALID",
                f"actual capture binding 的 {field} 与冻结 policy 不符")
    dynamic_fields = (
        "clock_mapping_evidence_sha256", "clock_mapping_stale",
        "completion_clock_session_id", "submission_clock_session_id",
        "mapping_segment_id",
    )
    _reject(set(binding) !=
                ((set(policy) - {"semantic_sha256"}) |
                 set(dynamic_fields) |
                 {"capture_policy_semantic_sha256", "semantic_sha256"}),
            "CAPTURE_BINDING_INVALID", "actual capture binding 字段集合漂移")
    _reject(not _is_sha256(binding.get("clock_mapping_evidence_sha256")) or
            binding.get("clock_mapping_stale") is not False,
            "CAPTURE_BINDING_INVALID", "actual clock mapping identity 无效")
    for field in ("completion_clock_session_id",
                  "submission_clock_session_id", "mapping_segment_id"):
        _reject(not isinstance(binding.get(field), str) or
                not binding.get(field), "CAPTURE_BINDING_INVALID",
                f"actual {field} 缺失")
    _reject(binding["completion_clock_session_id"] ==
                binding["submission_clock_session_id"],
            "CAPTURE_BINDING_INVALID", "completion/submission clock 混同")
    return semantic


def _validate_plan(plan: dict[str, Any], binder_path: pathlib.Path,
                   evaluator_path: pathlib.Path) -> tuple[
                       str, list[dict[str, Any]], list[dict[str, Any]],
                       list[str]]:
    plan_semantic = _verify_semantic(
        plan, "plan_semantic_sha256", "PLAN_SEAL_INVALID")
    _reject(plan.get("schema_version") != 1 or
            plan.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_calibration_plan" or
            plan.get("run_role") != "CALIBRATION_DELETION" or
            plan.get("status") != "FROZEN_BEFORE_CAPTURE",
            "PLAN_SEAL_INVALID", "plan header 无效")
    dispatch_count = _exact_int(
        plan.get("physical_dispatch_count"), "plan dispatch count",
        "OUTPUT_OFF_CONTRACT_INVALID")
    _reject(plan.get("physical_output_capability") is not False or
            dispatch_count != 0 or
            plan.get("production_aim_changed") is not False or
            plan.get("new_production_gain_claimed") is not False,
            "OUTPUT_OFF_CONTRACT_INVALID", "plan 必须保持 binder output-off")
    try:
        canonical_uuid = str(uuid.UUID(str(plan.get("run_uuid"))))
    except (ValueError, AttributeError) as error:
        raise BinderError("INPUT_IDENTITY_MISMATCH", "plan Run UUID 无效") \
            from error
    activation = _exact_int(
        plan.get("activation_epoch"), "activation epoch",
        "INPUT_IDENTITY_MISMATCH")
    denylist = plan.get("seen_diagnosis_denylist")
    _reject(plan.get("run_uuid") != canonical_uuid or
            canonical_uuid == SEEN_RUN_UUID or activation <= 0 or
            not _is_sha256(plan.get("scope_id")) or
            not isinstance(denylist, dict) or
            denylist.get("run_uuids") != [SEEN_RUN_UUID] or
            denylist.get("artifact_sha256s") !=
                list(SEEN_ARTIFACT_SHA256),
            "INPUT_IDENTITY_MISMATCH", "plan identity/deny-list 无效")
    _validate_capture_policy(plan.get("capture_policy"))

    seal = plan.get("seal")
    _reject(not isinstance(seal, dict), "PLAN_SEAL_INVALID", "plan seal 缺失")
    assert isinstance(seal, dict)
    frozen = _exact_int(
        plan.get("frozen_at_utc_unix_ns"), "plan frozen UTC time",
        "PLAN_SEAL_INVALID")
    _reject(frozen <= 0 or
            seal.get("schema_semantic_sha256") !=
                _schema_semantic_sha256() or
             seal.get("evaluator_file_sha256") !=
                 _file_sha256(evaluator_path) or
             seal.get("binder_file_sha256") != _file_sha256(binder_path) or
             not _is_sha256(seal.get("producer_file_sha256")) or
             not _is_sha256(seal.get("report_verifier_file_sha256")) or
             seal.get("model_semantic_sha256") is not None or
            seal.get("response_revealed_before_freeze") is not False,
            "PLAN_SEAL_INVALID", "schema/evaluator/binder/time seal 无效")

    phase = plan.get("phase_policy")
    expected_cells = [
        {"phase_cell": cell, "center_numerator": numerator,
         "center_denominator": 8}
        for cell, numerator in zip(PHASE_CELLS, (1, 3, 5, 7))
    ]
    _reject(not isinstance(phase, dict) or
            phase.get("policy_id") != "b-meas-phase-d1-v2" or
            phase.get("phase_scale") != "Q0.32_CYCLE" or
            phase.get("phase_cells") != expected_cells or
            phase.get("target_tolerance_q32") != Q32 // 16 or
            phase.get("minimum_repeats_per_phase_and_sign") != 4 or
            phase.get("counterbalance_design") !=
                "WILLIAMS_4X4_FIRST_ORDER" or
            phase.get("counterbalanced_rows") !=
                [list(row) for row in WILLIAMS_ROWS] or
            phase.get("position_balance_required") is not True or
            phase.get("directed_predecessor_balance_required") is not True or
            phase.get("required_sentinel_positions") !=
                ["begin", "middle", "end"] or
            phase.get("phase_assignment_selected_before_capture") is not
                True or
            phase.get("per_pulse_or_block_tuning_allowed") is not False,
            "PLAN_SEAL_INVALID", "phase policy 未按冻结 schema 注册")
    measurement = plan.get("measurement_policy")
    _reject(measurement != {
                "transition_rule": "EXACT_BGR_SHA256_FIRST_CHANGE",
                "anchor_sample_index": 0,
                "settled_sample": "LAST_QUALIFIED_SOURCE_EVENT",
                "window_sample_count": 6,
                "phase_interval_arithmetic":
                    "CONSERVATIVE_INTEGER_Q0_32_FROM_SOURCE_TIMESTAMP_RATE_AND_BOUNDARY_OFFSET",
            }, "PLAN_SEAL_INVALID", "measurement policy 无效")

    model = plan.get("model_policy")
    _reject(model != {
                "m_minus": "fixed_source_sample_onset",
                "m_plus":
                    "single_global_completion_relative_transition_interval",
                "output_feedback_used": False,
                "confirmation_or_validation_refit_allowed": False,
                "plant_nonlinearity_claimed": False,
                "unique_delay_or_measurement_model_claimed": False,
            }, "PLAN_SEAL_INVALID", "model policy 无效")
    command = plan.get("command_policy")
    _reject(not isinstance(command, dict), "PLAN_SEAL_INVALID",
            "command policy 缺失")
    assert isinstance(command, dict)
    magnitude = _exact_int(
        command.get("single_magnitude_counts"), "command magnitude",
        "PLAN_SEAL_INVALID")
    _reject(magnitude <= 0 or command.get("zero_y_required") is not True or
            command.get("net_x_required") != 0 or
            command.get("fixed_speed_gate_used") is not False,
            "PLAN_SEAL_INVALID", "command policy 无效")

    pulses = plan.get("pulses")
    controls = plan.get("negative_controls")
    _reject(not isinstance(pulses, list) or
            any(not isinstance(item, dict) for item in pulses) or
            not isinstance(controls, list) or
            any(not isinstance(item, dict) for item in controls),
            "PLAN_SEAL_INVALID", "plan pulse/control 清单无效")
    assert isinstance(pulses, list) and isinstance(controls, list)
    _reject(seal.get("order_manifest_sha256") !=
                _order_manifest_sha256(pulses),
            "PLAN_SEAL_INVALID", "order manifest seal 漂移")
    _reject(len(pulses) != 38, "DESIGN_BALANCE_INVALID",
            "plan 必须包含 32 个 regular 与 6 个 sentinel pulses")
    pulse_ids: set[str] = set()
    blocks: dict[str, list[dict[str, Any]]] = {}
    block_order: list[str] = []
    net_x = 0
    for expected_sequence, pulse in enumerate(pulses):
        pulse_id = pulse.get("pulse_id")
        block_id = pulse.get("block_id")
        dx = _exact_int(
            pulse.get("command_dx_counts"), "planned command X",
            "DESIGN_BALANCE_INVALID")
        dy = _exact_int(
            pulse.get("command_dy_counts"), "planned command Y",
            "DESIGN_BALANCE_INVALID")
        _reject(not isinstance(pulse_id, str) or not pulse_id or
                pulse_id in pulse_ids or
                pulse.get("sequence_index") != expected_sequence or
                not isinstance(block_id, str) or not block_id or
                pulse.get("phase_cell") not in PHASE_CELLS or
                dx not in (-magnitude, magnitude) or dy != 0,
                "DESIGN_BALANCE_INVALID", "planned pulse identity/order 无效")
        pulse_ids.add(pulse_id)
        net_x += dx
        if block_id not in blocks:
            blocks[block_id] = []
            block_order.append(block_id)
        blocks[block_id].append(pulse)
    _reject(net_x != 0 or len(block_order) != 19 or
            any(len(block) != 2 for block in blocks.values()),
            "DESIGN_BALANCE_INVALID", "planned pair/net-X 无效")
    for block in blocks.values():
        first, second = block
        pair_fields = ("phase_cell", "repeat_index", "row_index",
                       "position_index", "polarity_order",
                       "sentinel_position")
        expected_commands = (
            (magnitude, -magnitude)
            if first.get("polarity_order") == "positive_first" else
            (-magnitude, magnitude)
            if first.get("polarity_order") == "negative_first" else None)
        _reject(any(first.get(field) != second.get(field)
                    for field in pair_fields) or
                first.get("pulse_ordinal") != 1 or
                second.get("pulse_ordinal") != 2 or
                expected_commands is None or
                (first.get("command_dx_counts"),
                 second.get("command_dx_counts")) != expected_commands,
                "DESIGN_BALANCE_INVALID", "planned signed pair 无效")

    sentinel_indices = [0, 9, 18]
    sentinel_positions = ["begin", "middle", "end"]
    for block_index, position in zip(sentinel_indices, sentinel_positions):
        first = blocks[block_order[block_index]][0]
        _reject(first.get("sentinel_position") != position or
                first.get("phase_cell") != "P1_8" or
                first.get("repeat_index") != 0 or
                first.get("row_index") is not None or
                first.get("position_index") is not None,
                "DESIGN_BALANCE_INVALID", "sentinel block 顺序无效")
    regular_blocks = [
        blocks[block_id][0] for block_id in block_order
        if blocks[block_id][0].get("sentinel_position") is None
    ]
    _reject(len(regular_blocks) != 16,
            "DESIGN_BALANCE_INVALID", "regular block 数量无效")
    actual_rows = [
        tuple(block["phase_cell"] for block in regular_blocks[index:index + 4])
        for index in range(0, 16, 4)
    ]
    _reject(actual_rows != list(WILLIAMS_ROWS) or
            [block.get("repeat_index") for block in regular_blocks] !=
                [1] * 4 + [2] * 4 + [3] * 4 + [4] * 4 or
            [block.get("row_index") for block in regular_blocks] !=
                [0] * 4 + [1] * 4 + [2] * 4 + [3] * 4 or
            [block.get("position_index") for block in regular_blocks] !=
                list(range(4)) * 4 or
            any(sum(1 for block in regular_blocks
                    if block["phase_cell"] == cell and
                    block["polarity_order"] == polarity) != 2
                for cell in PHASE_CELLS
                for polarity in ("positive_first", "negative_first")),
            "DESIGN_BALANCE_INVALID",
            "Williams/repeat/position/sign balance 无效")

    _reject(len(controls) != 4 or
            [control.get("phase_cell") for control in controls] !=
                list(PHASE_CELLS),
            "DESIGN_BALANCE_INVALID", "negative control cell 顺序无效")
    control_ids: set[str] = set()
    controls_by_cell: dict[str, str] = {}
    tolerance = Q32 // 16
    for control in controls:
        control_id = control.get("control_id")
        window_id = control.get("window_id")
        cell = control.get("phase_cell")
        lower, upper = _interval(
            control.get("scheduled_phase_interval_q32"),
            "control scheduled phase", "DESIGN_BALANCE_INVALID")
        center = PHASE_NUMERATORS[cell] * Q32 // 8
        _reject(not isinstance(control_id, str) or not control_id or
                control_id in control_ids or window_id != control_id or
                control.get("command_dx_counts") != 0 or
                control.get("command_dy_counts") != 0 or
                control.get("mouse_or_kmbox_event_emitted") is not False or
                lower < center - tolerance or upper > center + tolerance or
                lower > center or upper < center,
                "DESIGN_BALANCE_INVALID", "negative control plan 无效")
        control_ids.add(control_id)
        controls_by_cell[cell] = window_id

    expected_window_order: list[str] = []
    controls_after = {
        9: controls_by_cell["P1_8"],
        17: controls_by_cell["P3_8"],
        27: controls_by_cell["P5_8"],
        35: controls_by_cell["P7_8"],
    }
    for pulse in pulses:
        expected_window_order.append(pulse["pulse_id"])
        control_window = controls_after.get(pulse["sequence_index"])
        if control_window is not None:
            expected_window_order.append(control_window)
    window_order = plan.get("window_order")
    _reject(window_order != expected_window_order,
            "DESIGN_BALANCE_INVALID",
            "pulse/control acquisition window order 无效")
    sequence_binding = plan.get("sequence_binding")
    _reject(not isinstance(sequence_binding, dict) or
            sequence_binding.get("sequence_schema") != 7 or
            sequence_binding.get("sequence_profile") !=
                "physical_b_composite_phase_calibration" or
            not _is_sha256(sequence_binding.get("sequence_file_sha256")) or
            not _is_sha256(
                sequence_binding.get("sequence_semantic_sha256")) or
            sequence_binding.get("sample_count") != 295 or
            sequence_binding.get("window_count") != 42 or
            sequence_binding.get("window_order") != expected_window_order,
            "PLAN_SEAL_INVALID", "sequence binding 未按冻结合同注册")
    scheduler = plan.get("scheduler_policy")
    _reject(not isinstance(scheduler, dict) or
            scheduler != {
                "clock_kind": "WINDOWS_QPC",
                "timer_mode": "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL",
                "deadline_basis":
                    "PREDICTOR_NEXT_NDI_SUBMISSION_BOUNDARY",
                "issue_lead_ns": 400_000,
                "issue_lead_applies_to": "NONZERO_PULSE_ONLY",
                "negative_control_marker_lead_ns": 0,
                "target_tolerance_q32": Q32 // 16,
                "active_guard_ns": 300_000,
                "max_wake_lateness_ns": 150_000,
                "max_event_interval_width_ns": 100_000,
                "max_active_wait_ns_per_event": 350_000,
                "max_active_wait_ns_total": 42 * 350_000,
                "preflight_required": True,
                "preflight_file_sha256":
                    scheduler.get("preflight_file_sha256")
                    if isinstance(scheduler, dict) else None,
                "per_event_tuning_allowed": False,
                "process_priority": "NORMAL",
                "thread_priority": "NORMAL",
                "cpu_affinity_used": False,
                "time_begin_period_used": False,
                "periodic_timer_used": False,
            } or
            not _is_sha256(scheduler.get("preflight_file_sha256")),
            "PLAN_SEAL_INVALID", "scheduler policy 未按冻结合同注册")
    return plan_semantic, pulses, controls, expected_window_order


def _validate_mapping(mapping: Any, binding: dict[str, Any]) -> tuple[
        str, int, int, int]:
    _reject(not isinstance(mapping, dict), "CLOCK_MAPPING_INCOMPLETE",
            "clock mapping 缺失")
    assert isinstance(mapping, dict)
    semantic = _verify_semantic(
        mapping, "semantic_sha256", "CLOCK_MAPPING_INCOMPLETE")
    valid_from = _exact_int(
        mapping.get("valid_from_common_ns"), "mapping valid from",
        "CLOCK_MAPPING_INCOMPLETE")
    valid_through = _exact_int(
        mapping.get("valid_through_common_ns"), "mapping valid through",
        "CLOCK_MAPPING_INCOMPLETE")
    uncertainty = _exact_int(
        mapping.get("source_clock_uncertainty_ns"), "clock uncertainty",
        "CLOCK_MAPPING_INCOMPLETE")
    _reject(mapping.get("stale") is True, "CLOCK_MAPPING_STALE",
            "clock mapping 已过有效期")
    _reject(mapping.get("stale") is not False or
            semantic != binding.get("clock_mapping_evidence_sha256") or
            mapping.get("policy_id") !=
                binding.get("clock_mapping_policy_id") or
            mapping.get("completion_clock_session_id") !=
                binding.get("completion_clock_session_id") or
            mapping.get("submission_clock_session_id") !=
                binding.get("submission_clock_session_id") or
            mapping.get("mapping_segment_id") !=
                binding.get("mapping_segment_id") or
            valid_from < 0 or valid_from >= valid_through or uncertainty < 0,
            "CLOCK_MAPPING_INCOMPLETE", "clock mapping identity/window 无效")
    for field in ("qpc_quantization_ns", "read_access_interval_ns",
                  "source_clock_round_trip_max_ns",
                  "source_clock_mapping_age_max_ns"):
        _reject(_exact_int(mapping.get(field), field,
                           "CLOCK_MAPPING_INCOMPLETE") < 0,
                "CLOCK_MAPPING_INCOMPLETE", f"{field} 不得为负")
    sample_count = _exact_int(
        mapping.get("source_clock_sample_count_min"),
        "source clock sample count", "CLOCK_MAPPING_INCOMPLETE")
    rate_lower, rate_upper = _interval(
        mapping.get("source_clock_rate_interval_q32"),
        "source clock rate interval", "CLOCK_MAPPING_INCOMPLETE")
    _reject(sample_count <= 0 or rate_lower <= 0 or
            rate_lower < MIN_SOURCE_CLOCK_RATE_Q32 or
            rate_upper > MAX_SOURCE_CLOCK_RATE_Q32 or
            mapping.get("uncertainty_includes_mapping_fit_and_transport")
                is not True or
            mapping.get("raw_ntp_statistics_exported") is not False or
            not _is_sha256(mapping.get("source_timing_evidence_sha256")) or
             not _is_sha256(mapping.get("policy_sha256")),
            "CLOCK_MAPPING_INCOMPLETE",
            "clock mapping 可导出来源/保守上界 seal 缺失")
    return semantic, valid_from, valid_through, uncertainty


def _validate_capture(
        capture: dict[str, Any], plan: dict[str, Any],
        expected_windows: list[str]) -> tuple[
            str, str, int, int, int, dict[str, Any],
            dict[str, list[dict[str, Any]]]]:
    semantic = _verify_semantic(
        capture, "capture_semantic_sha256", "CAPTURE_LEDGER_INVALID")
    _validate_identity(capture, plan, "capture ledger")
    _reject(capture.get("schema_version") != 1 or
            capture.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_capture_ledger" or
            capture.get("status") != "CAPTURE_COMPLETE" or
            capture.get("physical_output_capability") is not False or
            capture.get("physical_dispatch_count") != 0,
            "CAPTURE_LEDGER_INVALID", "capture ledger header 无效")
    policy = plan["capture_policy"]
    policy_semantic = policy["semantic_sha256"]
    _reject(capture.get("plan_semantic_sha256") !=
                plan["plan_semantic_sha256"] or
            capture.get("capture_policy_semantic_sha256") !=
                policy_semantic or
            capture.get("report_verifier_file_sha256") !=
                plan["seal"]["report_verifier_file_sha256"] or
            not _is_sha256(capture.get("human_assessment_semantic_sha256")) or
            not _is_sha256(capture.get("schedule_ledger_semantic_sha256")) or
            not _is_sha256(capture.get("command_report_semantic_sha256")),
            "INPUT_IDENTITY_MISMATCH", "capture plan/policy identity 漂移")
    binding = capture.get("capture_binding")
    capture_binding_semantic = _validate_capture_binding(binding, policy)
    assert isinstance(binding, dict)
    _reject(capture.get("capture_binding_semantic_sha256") !=
                capture_binding_semantic,
            "INPUT_IDENTITY_MISMATCH", "capture binding identity 漂移")
    mapping_semantic, valid_from, valid_through, uncertainty = \
        _validate_mapping(capture.get("clock_mapping"), binding)
    scheduler_clock = capture.get("scheduler_clock")
    _reject(not isinstance(scheduler_clock, dict) or
            scheduler_clock.get("clock_kind") != "WINDOWS_QPC" or
            not isinstance(scheduler_clock.get("clock_session_id"), str) or
            not scheduler_clock.get("clock_session_id") or
            _exact_int(scheduler_clock.get("frequency_hz"),
                       "scheduler QPC frequency", "PLAN_SEAL_INVALID") <= 0 or
            _exact_int(scheduler_clock.get("producer_process_id"),
                       "scheduler producer process", "PLAN_SEAL_INVALID") <= 0,
            "PLAN_SEAL_INVALID", "辅机 scheduler QPC session 无效")
    accepted = _exact_int(
        capture.get("plan_accepted_at_qpc"), "plan accepted QPC",
        "PLAN_SEAL_INVALID")
    acquired = _exact_int(
        capture.get("acquisition_started_at_qpc"),
        "acquisition start QPC", "PLAN_SEAL_INVALID")
    finished = _exact_int(
        capture.get("acquisition_finished_at_qpc"),
        "acquisition finish QPC", "PLAN_SEAL_INVALID")
    revealed = _exact_int(
        capture.get("revealed_at_qpc"), "response reveal QPC",
        "PLAN_SEAL_INVALID")
    _reject(not 0 < accepted < acquired < finished or revealed != finished,
            "PLAN_SEAL_INVALID",
            "必须在同一辅机 QPC session 先 accept plan、再 acquisition、最后 reveal")

    frames = capture.get("frames")
    _reject(not isinstance(frames, list) or not frames,
            "FRAME_LEDGER_INVALID", "frame ledger 为空")
    assert isinstance(frames, list)
    windows: dict[str, list[dict[str, Any]]] = {}
    event_ids: set[str] = set()
    previous_upper: int | None = None
    previous_source_timestamp: int | None = None
    for expected_sequence, frame in enumerate(frames):
        _reject(not isinstance(frame, dict), "FRAME_LEDGER_INVALID",
                "frame 必须是 object")
        assert isinstance(frame, dict)
        event_id = frame.get("frame_event_id")
        window_id = frame.get("window_id")
        _reject(not isinstance(event_id, str) or not event_id or
                event_id in event_ids or
                frame.get("source_sequence") != expected_sequence or
                frame.get("receiver_sequence") != expected_sequence or
                not isinstance(window_id, str) or not window_id or
                frame.get("frame_sync_reused") is not False,
                "FRAME_LEDGER_INVALID", "frame identity/order 无效")
        _reject(frame.get("submission_clock_session_id") !=
                    binding["submission_clock_session_id"] or
                frame.get("mapping_segment_id") !=
                    binding["mapping_segment_id"],
                "CLOCK_MAPPING_INCOMPLETE",
                "frame clock session/mapping segment 漂移")
        event_ids.add(event_id)
        lower, upper = _interval(
            frame.get("boundary_time_interval_ns"), "frame boundary",
            "FRAME_LEDGER_INVALID")
        source_timestamp = _exact_int(
            frame.get("source_timestamp_100ns"), "source timestamp",
            "FRAME_LEDGER_INVALID")
        _reject(lower < valid_from or upper > valid_through or
                (previous_upper is not None and
                 previous_upper >= lower) or source_timestamp <= 0 or
                (previous_source_timestamp is not None and
                 previous_source_timestamp >= source_timestamp),
                "FRAME_LEDGER_INVALID", "frame boundary 缺失/重复/乱序")
        previous_upper = upper
        previous_source_timestamp = source_timestamp
        for field in ("full_bgr_sha256", "left_roi_bgr_sha256",
                      "right_roi_bgr_sha256"):
            _reject(not _is_sha256(frame.get(field)),
                    "FRAME_LEDGER_INVALID", f"{field} 无效")
        for witness in ("left", "right"):
            _interval(
                frame.get(
                    f"{witness}_normalized_displacement_interval_q32"),
                f"{witness} settled displacement",
                "FRAME_LEDGER_INVALID")
        _reject(frame.get("scene_cut_or_occlusion_detected") not in
                    (True, False), "FRAME_LEDGER_INVALID",
                "scene flag 缺失")
        windows.setdefault(window_id, []).append(frame)

    _reject(list(windows.keys()) != expected_windows,
            "FRAME_LEDGER_INVALID", "capture window 顺序/集合漂移")
    expected_sample_count = plan["measurement_policy"][
        "window_sample_count"]
    for window_id, window in windows.items():
        _reject(len(window) != expected_sample_count or
                [frame.get("sample_index") for frame in window] !=
                    list(range(len(window))),
                "FRAME_LEDGER_INVALID",
                f"{window_id} capture 长度/sample index 无效")
    return (semantic, mapping_semantic, valid_from, valid_through,
            uncertainty, binding, windows)


def _validate_commands(
        commands: dict[str, Any], plan: dict[str, Any],
        pulses: list[dict[str, Any]], controls: list[dict[str, Any]],
        binding: dict[str, Any],
        mapping_semantic: str, valid_from: int, valid_through: int) -> tuple[
            str, dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    semantic = _verify_semantic(
        commands, "command_semantic_sha256", "COMMAND_LEDGER_INVALID")
    _validate_identity(commands, plan, "command ledger")
    _reject(commands.get("schema_version") != 1 or
            commands.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_command_ledger" or
            commands.get("status") != "COMMANDS_COMPLETE" or
            commands.get("ledger_is_read_only_record") is not True or
            commands.get("binder_physical_output_capability") is not False or
            commands.get("binder_physical_dispatch_count") != 0,
            "COMMAND_LEDGER_INVALID", "command ledger header 无效")
    _reject(commands.get("plan_semantic_sha256") !=
                plan["plan_semantic_sha256"] or
            commands.get("capture_policy_semantic_sha256") !=
                plan["capture_policy"]["semantic_sha256"] or
            commands.get("capture_binding_semantic_sha256") !=
                binding["semantic_sha256"] or
            commands.get("clock_mapping_semantic_sha256") !=
                mapping_semantic,
            "INPUT_IDENTITY_MISMATCH", "command ledger input seal 漂移")
    events = commands.get("events")
    command_controls = commands.get("negative_controls")
    _reject(not isinstance(events, list) or
            not isinstance(command_controls, list),
            "COMMAND_LEDGER_INVALID", "command event/control 清单缺失")
    assert isinstance(events, list) and isinstance(command_controls, list)
    _reject(len(events) != len(pulses) or
            commands.get("source_dispatch_count") != len(events) or
            commands.get("source_backend_completion_count") != len(events) or
            commands.get("source_delivery_ack_count") != len(events),
            "COMMAND_LEDGER_INVALID",
            "source dispatch/completion/ACK 数量与 plan 不符")
    by_pulse: dict[str, dict[str, Any]] = {}
    completion_ids: set[str] = set()
    ack_ids: set[str] = set()
    net_x = 0
    for pulse, event in zip(pulses, events):
        _reject(not isinstance(event, dict), "COMMAND_LEDGER_INVALID",
                "command event 必须是 object")
        assert isinstance(event, dict)
        completion_id = event.get("backend_completion_event_id")
        ack_id = event.get("delivery_ack_id")
        dx = _exact_int(
            event.get("command_dx_counts"), "command X",
            "COMMAND_LEDGER_INVALID")
        dy = _exact_int(
            event.get("command_dy_counts"), "command Y",
            "COMMAND_LEDGER_INVALID")
        _reject(event.get("pulse_id") != pulse.get("pulse_id") or
                event.get("sequence_index") != pulse.get("sequence_index") or
                dx != pulse.get("command_dx_counts") or
                dy != pulse.get("command_dy_counts") or
                event.get("backend_completed") is not True or
                event.get("delivery_acked") is not True or
                not isinstance(completion_id, str) or not completion_id or
                completion_id in completion_ids or
                not isinstance(ack_id, str) or not ack_id or
                ack_id in ack_ids,
                "COMMAND_LEDGER_INVALID",
                "command completion/ACK/identity 无效")
        _reject(event.get("completion_clock_session_id") !=
                    binding["completion_clock_session_id"] or
                event.get("mapping_segment_id") !=
                    binding["mapping_segment_id"],
                "CLOCK_MAPPING_INCOMPLETE",
                "completion clock session/mapping segment 漂移")
        completion_ids.add(completion_id)
        ack_ids.add(ack_id)
        lower, upper = _interval(
            event.get("completion_time_interval_ns"),
            "completion time", "COMMAND_LEDGER_INVALID")
        _reject(lower < valid_from or upper > valid_through,
                "CLOCK_MAPPING_INCOMPLETE",
                "completion time 超出 mapping segment")
        net_x += dx
        by_pulse[pulse["pulse_id"]] = event
    _reject(net_x != plan.get("command_policy", {}).get("net_x_required"),
            "COMMAND_LEDGER_INVALID", "command ledger net X 无效")

    _reject(len(command_controls) != len(controls),
            "COMMAND_LEDGER_INVALID", "no-command control 数量无效")
    by_control: dict[str, dict[str, Any]] = {}
    marker_ids: set[str] = set()
    for planned, recorded in zip(controls, command_controls):
        marker_id = (recorded.get("schedule_marker_event_id")
                     if isinstance(recorded, dict) else None)
        _reject(not isinstance(recorded, dict) or
                recorded.get("control_id") != planned.get("control_id") or
                recorded.get("command_dx_counts") != 0 or
                recorded.get("command_dy_counts") != 0 or
                recorded.get("mouse_or_kmbox_event_emitted") is not False or
                not isinstance(marker_id, str) or not marker_id or
                marker_id in marker_ids,
                "OUTPUT_OFF_CONTRACT_INVALID",
                "no-command control 出现 command/event")
        assert isinstance(recorded, dict)
        _reject(recorded.get("completion_clock_session_id") !=
                    binding["completion_clock_session_id"] or
                recorded.get("mapping_segment_id") !=
                    binding["mapping_segment_id"],
                "CLOCK_MAPPING_INCOMPLETE",
                "control schedule marker clock/session 漂移")
        lower, upper = _interval(
            recorded.get("scheduled_marker_time_interval_ns"),
            "control schedule marker", "COMMAND_LEDGER_INVALID")
        _reject(lower < valid_from or upper > valid_through,
                "CLOCK_MAPPING_INCOMPLETE",
                "control schedule marker 超出 mapping segment")
        marker_ids.add(marker_id)
        by_control[recorded["control_id"]] = recorded
    return semantic, by_pulse, by_control


def _phase_interval(frames: list[dict[str, Any]], time_interval: Any,
                    mapping: dict[str, Any],
                    context: str) -> tuple[
                        dict[str, int], dict[str, Any]]:
    completion_lower, completion_upper = _interval(
        time_interval, context, "COMMAND_LEDGER_INVALID")
    adjacent: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for previous, following in zip(frames, frames[1:]):
        previous_lower, previous_upper = _interval(
            previous["boundary_time_interval_ns"], "previous boundary",
            "FRAME_LEDGER_INVALID")
        following_lower, _ = _interval(
            following["boundary_time_interval_ns"], "following boundary",
            "FRAME_LEDGER_INVALID")
        if (previous_upper < completion_lower and
                completion_upper < following_lower):
            adjacent.append((previous, following))
    _reject(len(adjacent) != 1, "PHASE_CELL_AMBIGUOUS",
            "completion 未被唯一相邻 source boundary 包围")
    previous, following = adjacent[0]
    previous_lower, previous_upper = _interval(
        previous["boundary_time_interval_ns"], "previous boundary")
    following_lower, following_upper = _interval(
        following["boundary_time_interval_ns"], "following boundary")
    previous_source_timestamp = _exact_int(
        previous.get("source_timestamp_100ns"),
        "previous source timestamp", "FRAME_LEDGER_INVALID")
    following_source_timestamp = _exact_int(
        following.get("source_timestamp_100ns"),
        "following source timestamp", "FRAME_LEDGER_INVALID")
    source_delta_100ns = following_source_timestamp - \
        previous_source_timestamp
    rate_lower, rate_upper = _interval(
        mapping.get("source_clock_rate_interval_q32"),
        "source clock rate interval", "CLOCK_MAPPING_INCOMPLETE")
    source_period_ns = source_delta_100ns * 100
    period_lower = source_period_ns * rate_lower // Q32
    period_upper = _ceil_ratio(source_period_ns * rate_upper, Q32)
    numerator_lower = completion_lower - previous_upper
    numerator_upper = completion_upper - previous_lower
    mapped_period_lower = following_lower - previous_upper
    mapped_period_upper = following_upper - previous_lower
    _reject(source_delta_100ns <= 0 or period_lower <= 0 or
            period_upper < period_lower or
            period_upper < mapped_period_lower or
            period_lower > mapped_period_upper or
            numerator_lower <= 0 or numerator_upper >= period_lower,
            "PHASE_CELL_AMBIGUOUS", "phase uncertainty 跨 source boundary")
    lower = numerator_lower * Q32 // period_upper
    upper = _ceil_ratio(numerator_upper * Q32, period_lower)
    _reject(lower < 0 or upper >= Q32 or lower > upper,
            "PHASE_CELL_AMBIGUOUS", "phase interval 超出一个 cycle")
    return (
        {"lower_closed": lower, "upper_closed": upper},
        {
            "previous_frame_event_id": previous["frame_event_id"],
            "previous_source_sequence": previous["source_sequence"],
            "following_frame_event_id": following["frame_event_id"],
            "following_source_sequence": following["source_sequence"],
        },
    )


def _validate_assigned_phase(interval: dict[str, int], phase_cell: str,
                             tolerance: int) -> None:
    center = PHASE_NUMERATORS[phase_cell] * Q32 // 8
    _reject(
        interval["lower_closed"] < center - tolerance or
        interval["upper_closed"] > center + tolerance or
        interval["lower_closed"] > interval["upper_closed"],
        "PHASE_CELL_AMBIGUOUS",
        "实际 phase interval 超出预分配 cell")


def _transition_witness(
        frames: list[dict[str, Any]], completion: dict[str, Any],
        witness: str, uncertainty: int,
        binding: dict[str, Any]) -> dict[str, Any]:
    anchor = frames[0]
    hash_field = f"{witness}_roi_bgr_sha256"
    anchor_hash = anchor[hash_field]
    changed_index: int | None = None
    for index, frame in enumerate(frames[1:], 1):
        if frame[hash_field] != anchor_hash:
            changed_index = index
            break
    _reject(changed_index is None, "NO_VISIBLE_TRANSITION",
            f"{witness} witness 没有 exact-BGR transition")
    assert changed_index is not None
    _reject(any(frame[hash_field] == anchor_hash
                for frame in frames[changed_index + 1:]),
            "MEASUREMENT_WITNESS_INVALID",
            f"{witness} witness 首次变化后回到 anchor")
    last_same = frames[changed_index - 1]
    first_changed = frames[changed_index]
    completion_lower, completion_upper = _interval(
        completion["completion_time_interval_ns"], "completion time")
    last_lower, _ = _interval(
        last_same["boundary_time_interval_ns"], "last same boundary")
    _, changed_upper = _interval(
        first_changed["boundary_time_interval_ns"],
        "first changed boundary")
    settled_lower, settled_upper = _interval(
        frames[-1][
            f"{witness}_normalized_displacement_interval_q32"],
        f"{witness} settled displacement", "FRAME_LEDGER_INVALID")
    return {
        "visible_transition_found": True,
        "anchor_frame_event_id": anchor["frame_event_id"],
        "anchor_source_sequence": anchor["source_sequence"],
        "last_same_frame_event_id": last_same["frame_event_id"],
        "last_same_source_sequence": last_same["source_sequence"],
        "first_changed_frame_event_id": first_changed["frame_event_id"],
        "first_changed_source_sequence": first_changed["source_sequence"],
        "settled_frame_event_id": frames[-1]["frame_event_id"],
        "settled_source_sequence": frames[-1]["source_sequence"],
        "anchor_roi_bgr_sha256": anchor_hash,
        "last_same_roi_bgr_sha256": last_same[hash_field],
        "first_changed_roi_bgr_sha256": first_changed[hash_field],
        "last_same_submission_after_completed_ns":
            last_lower - completion_upper,
        "first_changed_submission_after_completed_ns":
            changed_upper - completion_lower,
        "visible_transition_offset_samples":
            first_changed["source_sequence"] - anchor["source_sequence"],
        "source_clock_uncertainty_ns": uncertainty,
        "submission_clock_session_id":
            binding["submission_clock_session_id"],
        "mapping_segment_id": binding["mapping_segment_id"],
        "interval_endpoints_include_mapping_uncertainty": True,
        "settled_normalized_displacement_interval_q32": {
            "lower_closed": settled_lower,
            "upper_closed": settled_upper,
        },
    }


def bind_calibration(
        plan_path: pathlib.Path, capture_path: pathlib.Path,
        command_path: pathlib.Path) -> dict[str, Any]:
    binder_path = pathlib.Path(__file__).resolve()
    evaluator_path = binder_path.with_name(
        "evaluate_mouse_effect_probe_b_composite_phase.py")
    _reject(not evaluator_path.is_file(), "PLAN_SEAL_INVALID",
            "冻结 evaluator 文件缺失")
    plan = _load_json(plan_path, "plan")
    capture = _load_json(capture_path, "capture ledger")
    commands = _load_json(command_path, "command ledger")
    plan_semantic, pulses, controls, expected_windows = _validate_plan(
        plan, binder_path, evaluator_path)
    denylist = set(SEEN_ARTIFACT_SHA256)
    input_paths = (plan_path, capture_path, command_path)
    input_hashes = [_file_sha256(path) for path in input_paths]
    _reject(any(value in denylist for value in input_hashes),
            "INPUT_IDENTITY_MISMATCH",
            "当前 seen diagnosis artifact 不得作为 calibration 输入")
    (capture_semantic, mapping_semantic, valid_from, valid_through,
     uncertainty, binding, windows) = _validate_capture(
         capture, plan, expected_windows)
    command_semantic, events, control_events = _validate_commands(
        commands, plan, pulses, controls, binding, mapping_semantic,
        valid_from, valid_through)

    bound_pulses: list[dict[str, Any]] = []
    for pulse in pulses:
        event = events[pulse["pulse_id"]]
        window = windows[pulse["pulse_id"]]
        phase_interval, phase_boundaries = _phase_interval(
            window, event["completion_time_interval_ns"],
            capture["clock_mapping"],
            "completion time")
        tolerance = plan["phase_policy"]["target_tolerance_q32"]
        _validate_assigned_phase(
            phase_interval, pulse["phase_cell"], tolerance)
        bound = {field: pulse[field] for field in ORDER_FIELDS}
        bound.update({
            "completion_clock_session_id":
                binding["completion_clock_session_id"],
            "mapping_segment_id": binding["mapping_segment_id"],
            "scene_cut_or_occlusion_detected": any(
                frame["scene_cut_or_occlusion_detected"]
                for frame in window),
            "completion_phase_interval_q32": phase_interval,
            "completion_phase_source_boundaries": phase_boundaries,
            "source_command_evidence": {
                "backend_completion_event_id":
                    event["backend_completion_event_id"],
                "delivery_ack_id": event["delivery_ack_id"],
                "completion_time_interval_ns":
                    event["completion_time_interval_ns"],
            },
            "capture_binding_semantic_sha256":
                binding["semantic_sha256"],
            "per_pulse_shift_samples": None,
            "per_pulse_gain": None,
            "per_pulse_kernel": None,
            "witnesses": {
                witness: _transition_witness(
                    window, event, witness, uncertainty, binding)
                for witness in ("left", "right")
            },
        })
        bound_pulses.append(bound)

    bound_controls: list[dict[str, Any]] = []
    for control in controls:
        window = windows[control["window_id"]]
        control_event = control_events[control["control_id"]]
        scheduled_phase, scheduled_boundaries = _phase_interval(
            window, control_event["scheduled_marker_time_interval_ns"],
            capture["clock_mapping"],
            "control schedule marker")
        _validate_assigned_phase(
            scheduled_phase, control["phase_cell"],
            plan["phase_policy"]["target_tolerance_q32"])
        anchor = window[0]

        def control_final_hash(witness: str) -> str:
            field = f"{witness}_roi_bgr_sha256"
            anchor_hash = anchor[field]
            return next((frame[field] for frame in window[1:]
                         if frame[field] != anchor_hash), window[-1][field])

        bound_controls.append({
            "control_id": control["control_id"],
            "phase_cell": control["phase_cell"],
            "scheduled_phase_interval_q32": scheduled_phase,
            "schedule_marker_event_id":
                control_event["schedule_marker_event_id"],
            "scheduled_phase_source_boundaries": scheduled_boundaries,
            "command_dx_counts": 0,
            "command_dy_counts": 0,
            "mouse_or_kmbox_event_emitted": False,
            "capture_binding_semantic_sha256":
                binding["semantic_sha256"],
            "completion_clock_session_id":
                binding["completion_clock_session_id"],
            "submission_clock_session_id":
                binding["submission_clock_session_id"],
            "mapping_segment_id": binding["mapping_segment_id"],
            "source_event_count": len(window),
            "scene_cut_detected": any(
                frame["scene_cut_or_occlusion_detected"]
                for frame in window),
            "left_anchor_roi_bgr_sha256":
                anchor["left_roi_bgr_sha256"],
            "left_final_roi_bgr_sha256": control_final_hash("left"),
            "right_anchor_roi_bgr_sha256":
                anchor["right_roi_bgr_sha256"],
            "right_final_roi_bgr_sha256": control_final_hash("right"),
        })

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration",
        "run_role": "CALIBRATION_DELETION",
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "scope_id": plan["scope_id"],
        "status": "RECORDED_UNANALYZED",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "current_seen_run_reused_as_validation": False,
        "source_run_was_sealed_before_capture": True,
        "seal": {
            "schema_semantic_sha256":
                plan["seal"]["schema_semantic_sha256"],
            "evaluator_file_sha256":
                plan["seal"]["evaluator_file_sha256"],
            "order_manifest_sha256":
                plan["seal"]["order_manifest_sha256"],
            "model_semantic_sha256": None,
            "frozen_at_utc_unix_ns": plan["frozen_at_utc_unix_ns"],
            "scheduler_clock": capture["scheduler_clock"],
            "plan_accepted_at_qpc": capture["plan_accepted_at_qpc"],
            "acquisition_started_at_qpc":
                capture["acquisition_started_at_qpc"],
            "revealed_at_qpc": capture["revealed_at_qpc"],
            "response_revealed_before_freeze": False,
        },
        "seen_diagnosis_denylist": plan["seen_diagnosis_denylist"],
        "phase_policy": plan["phase_policy"],
        "model_policy": plan["model_policy"],
        "measurement_policy": plan["measurement_policy"],
        "sequence_binding": plan["sequence_binding"],
        "scheduler_policy": plan["scheduler_policy"],
        "capture_policy": plan["capture_policy"],
        "capture_binding": binding,
        "command_policy": plan["command_policy"],
        "acquisition_summary": {
            "source_dispatch_count": commands["source_dispatch_count"],
            "source_backend_completion_count":
                commands["source_backend_completion_count"],
            "source_delivery_ack_count":
                commands["source_delivery_ack_count"],
            "source_frame_event_count": len(capture["frames"]),
            "pulse_window_count": len(pulses),
            "negative_control_window_count": len(controls),
            "clock_mapping_semantic_sha256": mapping_semantic,
        },
        "input_bindings": {
            "plan_file_sha256": input_hashes[0],
            "capture_file_sha256": input_hashes[1],
            "command_file_sha256": input_hashes[2],
            "plan_semantic_sha256": plan_semantic,
            "capture_semantic_sha256": capture_semantic,
            "command_semantic_sha256": command_semantic,
            "binder_file_sha256": _file_sha256(binder_path),
        },
        "pulses": bound_pulses,
        "negative_controls": bound_controls,
    }
    evidence["evidence_semantic_sha256"] = _canonical_sha256(evidence)
    return evidence


def main() -> int:
    # Windows 重定向流默认可能继承本地代码页；CLI 合同固定为 UTF-8，避免
    # 自动门读取稳定 reason code 时被代码页破坏。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Bind sealed plan/capture/command ledgers into evidence")
    parser.add_argument("--plan", required=True, type=pathlib.Path)
    parser.add_argument("--capture", required=True, type=pathlib.Path)
    parser.add_argument("--commands", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args()
    try:
        evidence = bind_calibration(
            options.plan.resolve(), options.capture.resolve(),
            options.commands.resolve())
        _write_json_atomic(options.output, evidence)
    except OutputCollisionError as error:
        print(f"binder 未发布 evidence: OUTPUT_COLLISION: {error}",
              file=sys.stderr)
        return 3
    except BinderError as error:
        print(f"binder 未生成 evidence: {error.reason_code}: {error}",
              file=sys.stderr)
        return 2
    except (OSError, UnicodeError, ValueError) as error:
        print(f"binder 未生成 evidence: INPUT_LEDGER_INVALID: {error}",
              file=sys.stderr)
        return 2
    print(f"calibration evidence 已原子创建: {options.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
