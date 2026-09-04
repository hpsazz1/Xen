#!/usr/bin/env python3
"""评估 Physical B output-off 复合相位校准证据；不具备 Mouse 能力。"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import pathlib
import sys
import uuid
from typing import Any


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
Q32 = 1 << 32
WITNESSES = ("left", "right")
ORDER_FIELDS = (
    "pulse_id", "sequence_index", "block_id", "phase_cell",
    "repeat_index", "row_index", "position_index", "polarity_order",
    "sentinel_position", "pulse_ordinal", "command_dx_counts",
    "command_dy_counts",
)


class EvidenceError(ValueError):
    """携带稳定 reason code 的 fail-closed 输入错误。"""

    def __init__(self, reason_code: str, message: str) -> None:
        super().__init__(message)
        self.reason_code = reason_code


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
        "phase_policy_id": "b-meas-phase-d1-v1",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
    })


def _order_manifest_sha256(pulses: list[Any]) -> str:
    manifest = [
        {field: (pulse.get(field) if isinstance(pulse, dict) else None)
         for field in ORDER_FIELDS}
        for pulse in pulses
    ]
    return _canonical_sha256(manifest)


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def _exact_int(value: Any, context: str,
               reason_code: str = "EVIDENCE_HEADER_INVALID") -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise EvidenceError(
            reason_code, f"{context} 必须是精确 JSON integer")
    return value


def _reject(condition: bool, reason_code: str, message: str) -> None:
    if condition:
        raise EvidenceError(reason_code, message)


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"evidence 无法读取: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("evidence 必须是 JSON object")
    return value


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if not path.is_absolute() or path.exists() or pending.exists():
        raise ValueError("evaluation 输出必须是尚不存在的绝对路径")
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


def evaluation_contract() -> dict[str, Any]:
    contract: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_evaluation_contract",
        "run_role": "CALIBRATION_DELETION_ONLY",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "timestamp_boundary": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
        "phase_scale": "Q0.32_CYCLE",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "counterbalanced_rows": [list(row) for row in WILLIAMS_ROWS],
        "models": {
            "m_minus": "fixed_source_sample_onset",
            "m_plus":
                "single_global_completion_relative_transition_interval",
            "settled_guard":
                "single_global_sign_normalized_displacement_interval_q32",
        },
        "maximum_calibration_status":
            "READY_FOR_SEALED_PHASE_VALIDATION",
        "no_per_pulse_or_block_shift_gain_kernel": True,
        "sentinels_constrain_but_do_not_fit_global_interval": True,
        "no_command_controls_required": True,
        "numeric_tolerance_or_speed_gate_used": False,
    }
    contract["contract_semantic_sha256"] = _canonical_sha256(contract)
    return contract


def _validate_header(evidence: dict[str, Any]) -> str:
    dispatch_count = _exact_int(
        evidence.get("physical_dispatch_count"),
        "physical dispatch count", "OUTPUT_OFF_CONTRACT_INVALID")
    _reject(evidence.get("physical_output_capability") is not False or
            dispatch_count != 0 or
            evidence.get("production_aim_changed") is not False or
            evidence.get("new_production_gain_claimed") is not False,
            "OUTPUT_OFF_CONTRACT_INVALID",
            "evaluation 输入必须保持 output-off 且不得改变生产 Aim")
    semantic = evidence.get("evidence_semantic_sha256")
    semantic_input = dict(evidence)
    semantic_input.pop("evidence_semantic_sha256", None)
    _reject(not _is_sha256(semantic) or
            semantic != _canonical_sha256(semantic_input),
            "EVIDENCE_SEMANTIC_MISMATCH",
            "evidence semantic SHA-256 无效")
    _reject(evidence.get("schema_version") != 1 or
            evidence.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_calibration" or
            evidence.get("run_role") != "CALIBRATION_DELETION" or
            evidence.get("status") != "RECORDED_UNANALYZED" or
            evidence.get("current_seen_run_reused_as_validation") is not
                False or
            evidence.get("source_run_was_sealed_before_capture") is not True,
            "EVIDENCE_HEADER_INVALID", "calibration header 无效")
    seal = evidence.get("seal")
    _reject(not isinstance(seal, dict),
            "EVIDENCE_HEADER_INVALID", "calibration seal 缺失")
    assert isinstance(seal, dict)
    frozen_at = _exact_int(
        seal.get("frozen_at_steady_ns"), "seal frozen time")
    acquired_at = _exact_int(
        seal.get("acquisition_started_at_steady_ns"),
        "acquisition start time")
    revealed_at = _exact_int(
        seal.get("revealed_at_steady_ns"), "response reveal time")
    _reject(seal.get("schema_semantic_sha256") !=
                _schema_semantic_sha256() or
            seal.get("evaluator_file_sha256") !=
                _file_sha256(pathlib.Path(__file__).resolve()) or
            not _is_sha256(seal.get("order_manifest_sha256")) or
            seal.get("model_semantic_sha256") is not None or
            seal.get("response_revealed_before_freeze") is not False or
            frozen_at < 0 or not frozen_at < acquired_at < revealed_at,
            "EVIDENCE_HEADER_INVALID",
            "schema/evaluator/order/time seal 无效")
    run_uuid = evidence.get("run_uuid")
    try:
        canonical_uuid = str(uuid.UUID(str(run_uuid)))
    except (ValueError, AttributeError) as error:
        raise EvidenceError(
            "EVIDENCE_HEADER_INVALID", "run UUID 无效") from error
    activation_epoch = _exact_int(
        evidence.get("activation_epoch"), "activation epoch")
    denylist = evidence.get("seen_diagnosis_denylist")
    _reject(str(run_uuid) != canonical_uuid or activation_epoch <= 0 or
            not _is_sha256(evidence.get("scope_id")) or
            not isinstance(denylist, dict) or
            denylist.get("run_uuids") != [SEEN_RUN_UUID] or
            denylist.get("artifact_sha256s") !=
                list(SEEN_ARTIFACT_SHA256) or
            canonical_uuid in denylist.get("run_uuids", []),
            "EVIDENCE_HEADER_INVALID", "run identity/scope 无效")
    return semantic


def _validate_phase_policy(evidence: dict[str, Any]) -> int:
    policy = evidence.get("phase_policy")
    _reject(not isinstance(policy, dict),
            "PHASE_CELL_CONTRACT_INVALID", "phase policy 缺失")
    assert isinstance(policy, dict)
    expected_cells = [
        {"phase_cell": phase_cell,
         "center_numerator": PHASE_NUMERATORS[phase_cell],
         "center_denominator": 8}
        for phase_cell in PHASE_CELLS
    ]
    tolerance = _exact_int(
        policy.get("target_tolerance_q32"), "phase target tolerance",
        "PHASE_CELL_CONTRACT_INVALID")
    repeats = _exact_int(
        policy.get("minimum_repeats_per_phase_and_sign"),
        "minimum repeats", "PHASE_CELL_CONTRACT_INVALID")
    _reject(policy.get("policy_id") != "b-meas-phase-d1-v1" or
            policy.get("phase_scale") != "Q0.32_CYCLE" or
            policy.get("phase_cells") != expected_cells or
            tolerance != Q32 // 16 or repeats != 4 or
            policy.get("counterbalance_design") !=
                "WILLIAMS_4X4_FIRST_ORDER" or
            policy.get("counterbalanced_rows") !=
                [list(row) for row in WILLIAMS_ROWS] or
            policy.get("position_balance_required") is not True or
            policy.get("directed_predecessor_balance_required") is not True or
            policy.get("required_sentinel_positions") !=
                ["begin", "middle", "end"] or
            policy.get("phase_assignment_selected_before_capture") is not
                True or
            policy.get("per_pulse_or_block_tuning_allowed") is not False,
            "PHASE_CELL_CONTRACT_INVALID", "phase cell policy 无效")
    return tolerance


def _valid_positive_geometry(value: Any, count: int) -> bool:
    return isinstance(value, list) and len(value) == count and all(
        isinstance(item, int) and not isinstance(item, bool)
        for item in value)


def _validate_capture_binding(evidence: dict[str, Any]) -> tuple[
        str, str, str, str]:
    binding = evidence.get("capture_binding")
    _reject(not isinstance(binding, dict),
            "CAPTURE_BINDING_INVALID", "capture binding 缺失")
    assert isinstance(binding, dict)
    semantic = binding.get("semantic_sha256")
    semantic_input = dict(binding)
    semantic_input.pop("semantic_sha256", None)
    _reject(not _is_sha256(semantic) or
            semantic != _canonical_sha256(semantic_input),
            "CAPTURE_BINDING_INVALID", "capture binding semantic 无效")

    source_geometry = binding.get("source_geometry")
    roi_geometry = binding.get("roi_geometry")
    source_rate = binding.get("source_rate")
    capture_stack = binding.get("capture_stack")
    _reject(binding.get("clock_mapping_stale") is True,
            "CLOCK_MAPPING_STALE", "clock mapping 已过有效期")
    configured_method = (capture_stack.get("capture_method_configured")
                         if isinstance(capture_stack, dict) else None)
    resolved_method = (capture_stack.get("capture_method_resolved")
                       if isinstance(capture_stack, dict) else None)
    completion_session = binding.get("completion_clock_session_id")
    submission_session = binding.get("submission_clock_session_id")
    mapping_segment = binding.get("mapping_segment_id")
    _reject(not _valid_positive_geometry(source_geometry, 2) or
            any(item <= 0 for item in source_geometry) or
            not _valid_positive_geometry(roi_geometry, 4) or
            roi_geometry[0] < 0 or roi_geometry[1] < 0 or
            roi_geometry[2] <= 0 or roi_geometry[3] <= 0 or
            roi_geometry[0] + roi_geometry[2] > source_geometry[0] or
            roi_geometry[1] + roi_geometry[3] > source_geometry[1] or
            not isinstance(source_rate, dict) or
            not isinstance(source_rate.get("numerator"), int) or
            isinstance(source_rate.get("numerator"), bool) or
            source_rate.get("numerator", 0) <= 0 or
            not isinstance(source_rate.get("denominator"), int) or
            isinstance(source_rate.get("denominator"), bool) or
            source_rate.get("denominator", 0) <= 0 or
            not isinstance(capture_stack, dict) or
            any(not isinstance(capture_stack.get(field), str) or
                not capture_stack.get(field)
                for field in ("producer", "producer_version")) or
            configured_method not in ("AUTO", "DXGI", "WGC") or
            resolved_method not in ("DXGI", "WGC") or
            (configured_method != "AUTO" and
             configured_method != resolved_method) or
            not isinstance(binding.get("source_name"), str) or
            not binding.get("source_name") or
            binding.get("pixel_format") != "CPU_BGR" or
            not isinstance(binding.get("clock_mapping_policy_id"), str) or
            not binding.get("clock_mapping_policy_id") or
            not _is_sha256(binding.get("clock_mapping_evidence_sha256")) or
            binding.get("clock_mapping_stale") is not False or
            not isinstance(completion_session, str) or
            not completion_session or
            not isinstance(submission_session, str) or
            not submission_session or
            completion_session == submission_session or
            not isinstance(mapping_segment, str) or not mapping_segment or
            not _is_sha256(binding.get("scene_binding_sha256")) or
            binding.get("ndi_frame_sync_used") is not False or
            binding.get("boundary_claim_limit") !=
                "use_exact_selected_semantic_only",
            "CAPTURE_BINDING_INVALID", "capture stack/geometry 无效")

    for roi_name in ("left_witness_roi", "right_witness_roi"):
        witness_roi = binding.get(roi_name)
        _reject(not _valid_positive_geometry(witness_roi, 4) or
                witness_roi[0] < 0 or witness_roi[1] < 0 or
                witness_roi[2] <= 0 or witness_roi[3] <= 0 or
                witness_roi[0] + witness_roi[2] > roi_geometry[2] or
                witness_roi[1] + witness_roi[3] > roi_geometry[3],
                "CAPTURE_BINDING_INVALID", f"{roi_name} 无效")

    left_roi = binding["left_witness_roi"]
    right_roi = binding["right_witness_roi"]
    witness_rois_overlap = (
        max(left_roi[0], right_roi[0]) <
        min(left_roi[0] + left_roi[2], right_roi[0] + right_roi[2]) and
        max(left_roi[1], right_roi[1]) <
        min(left_roi[1] + left_roi[3], right_roi[1] + right_roi[3]))
    _reject(witness_rois_overlap, "CAPTURE_BINDING_INVALID",
            "left/right witness ROI 不得重叠")

    _reject(binding.get("boundary_semantic") !=
                "NDI_SDK_SUBMISSION_UTC" or
            binding.get("boundary_is_capture_or_exposure") is not False,
            "TIMESTAMP_SEMANTIC_INVALID",
            "只能声明 NDI submission boundary，不得声明 capture/exposure")
    return semantic, completion_session, submission_session, mapping_segment


def _validate_model_and_command_policy(
        evidence: dict[str, Any]) -> int:
    model = evidence.get("model_policy")
    _reject(not isinstance(model, dict) or
            model.get("m_minus") != "fixed_source_sample_onset" or
            model.get("m_plus") !=
                "single_global_completion_relative_transition_interval" or
            model.get("output_feedback_used") is not False or
            model.get("confirmation_or_validation_refit_allowed") is not
                False or
            model.get("plant_nonlinearity_claimed") is not False or
            model.get("unique_delay_or_measurement_model_claimed") is not
                False,
            "MODEL_POLICY_INVALID", "model policy 无效")
    command = evidence.get("command_policy")
    _reject(not isinstance(command, dict),
            "COMMAND_CONTRACT_INVALID", "command policy 缺失")
    assert isinstance(command, dict)
    magnitude = _exact_int(
        command.get("single_magnitude_counts"), "command magnitude",
        "COMMAND_CONTRACT_INVALID")
    net_x = _exact_int(
        command.get("net_x_required"), "required net X",
        "COMMAND_CONTRACT_INVALID")
    _reject(magnitude <= 0 or command.get("zero_y_required") is not True or
            net_x != 0 or command.get("fixed_speed_gate_used") is not False,
            "COMMAND_CONTRACT_INVALID", "command policy 无效")
    return magnitude


def _validate_witness(witness: Any, submission_session: str,
                      mapping_segment: str) -> tuple[int, int, int, int, int]:
    _reject(not isinstance(witness, dict),
            "MEASUREMENT_WITNESS_INVALID", "witness 缺失")
    assert isinstance(witness, dict)
    _reject(witness.get("visible_transition_found") is not True,
            "NO_VISIBLE_TRANSITION", "witness 未找到 exact-BGR transition")
    anchor = witness.get("anchor_roi_bgr_sha256")
    last_same = witness.get("last_same_roi_bgr_sha256")
    first_changed = witness.get("first_changed_roi_bgr_sha256")
    lower = _exact_int(
        witness.get("last_same_submission_after_completed_ns"),
        "transition lower", "MEASUREMENT_WITNESS_INVALID")
    upper = _exact_int(
        witness.get("first_changed_submission_after_completed_ns"),
        "transition upper", "MEASUREMENT_WITNESS_INVALID")
    uncertainty = _exact_int(
        witness.get("source_clock_uncertainty_ns"),
        "source clock uncertainty", "MEASUREMENT_WITNESS_INVALID")
    onset = _exact_int(
        witness.get("visible_transition_offset_samples"),
        "visible transition offset", "MEASUREMENT_WITNESS_INVALID")
    settled = witness.get("settled_normalized_displacement_interval_q32")
    _reject(not isinstance(settled, dict),
            "MEASUREMENT_WITNESS_INVALID",
            "settled normalized displacement interval 缺失")
    assert isinstance(settled, dict)
    settled_lower = _exact_int(
        settled.get("lower_closed"), "settled displacement lower",
        "MEASUREMENT_WITNESS_INVALID")
    settled_upper = _exact_int(
        settled.get("upper_closed"), "settled displacement upper",
        "MEASUREMENT_WITNESS_INVALID")
    _reject(not _is_sha256(anchor) or not _is_sha256(last_same) or
            not _is_sha256(first_changed) or anchor != last_same or
            anchor == first_changed or lower >= upper or uncertainty < 0 or
            onset < 0 or settled_lower > settled_upper,
            "MEASUREMENT_WITNESS_INVALID", "witness hash/transition 无效")
    _reject(witness.get("submission_clock_session_id") !=
                submission_session or
            witness.get("mapping_segment_id") != mapping_segment or
            witness.get("interval_endpoints_include_mapping_uncertainty") is
                not True,
            "CLOCK_MAPPING_INCOMPLETE",
            "witness clock session/mapping interval 无效")
    return lower, upper, onset, settled_lower, settled_upper


def _validate_phase_interval(value: Any, phase_cell: str,
                             tolerance: int) -> None:
    _reject(not isinstance(value, dict),
            "PHASE_CELL_CONTRACT_INVALID", "phase interval 缺失")
    assert isinstance(value, dict)
    lower = _exact_int(value.get("lower_closed"), "phase interval lower",
                       "PHASE_CELL_CONTRACT_INVALID")
    upper = _exact_int(value.get("upper_closed"), "phase interval upper",
                       "PHASE_CELL_CONTRACT_INVALID")
    center = PHASE_NUMERATORS[phase_cell] * Q32 // 8
    _reject(lower < 0 or upper >= Q32 or lower > center or upper < center or
            lower < center - tolerance or upper > center + tolerance,
            "PHASE_CELL_CONTRACT_INVALID",
            "completion phase interval 超出预注册 cell")


def _validate_pulses(evidence: dict[str, Any], capture_semantic: str,
                     completion_session: str, submission_session: str,
                     mapping_segment: str, tolerance: int,
                     magnitude: int) -> tuple[
                         dict[str, set[int]], list[int], list[int],
                         list[int], list[int], list[int], list[int],
                         list[int], list[int], int, int, int]:
    pulses = evidence.get("pulses")
    _reject(not isinstance(pulses, list),
            "BLOCK_BALANCE_INVALID", "pulses 必须是 array")
    assert isinstance(pulses, list)
    seal = evidence.get("seal")
    _reject(not isinstance(seal, dict) or
            seal.get("order_manifest_sha256") !=
                _order_manifest_sha256(pulses),
            "EVIDENCE_HEADER_INVALID", "order manifest seal 漂移")
    onsets_by_cell: dict[str, set[int]] = {
        phase_cell: set() for phase_cell in PHASE_CELLS
    }
    regular_lower: list[int] = []
    regular_upper: list[int] = []
    sentinel_lower: list[int] = []
    sentinel_upper: list[int] = []
    regular_settled_lower: list[int] = []
    regular_settled_upper: list[int] = []
    sentinel_settled_lower: list[int] = []
    sentinel_settled_upper: list[int] = []
    pulse_ids: set[str] = set()
    blocks: dict[str, list[dict[str, Any]]] = {}
    block_order: list[str] = []
    sentinel_counts = {position: 0
                       for position in ("begin", "middle", "end")}
    non_sentinel_count = 0
    net_x = 0
    for expected_sequence, pulse in enumerate(pulses):
        _reject(not isinstance(pulse, dict),
                "BLOCK_BALANCE_INVALID", "pulse 必须是 object")
        assert isinstance(pulse, dict)
        pulse_id = pulse.get("pulse_id")
        block_id = pulse.get("block_id")
        _reject(not isinstance(pulse_id, str) or not pulse_id or
                pulse_id in pulse_ids or
                pulse.get("sequence_index") != expected_sequence or
                not isinstance(block_id, str) or not block_id,
                "BLOCK_BALANCE_INVALID", "pulse identity/sequence 无效")
        pulse_ids.add(pulse_id)
        if block_id not in blocks:
            blocks[block_id] = []
            block_order.append(block_id)
        blocks[block_id].append(pulse)

        dx = _exact_int(pulse.get("command_dx_counts"), "command X",
                        "COMMAND_CONTRACT_INVALID")
        dy = _exact_int(pulse.get("command_dy_counts"), "command Y",
                        "COMMAND_CONTRACT_INVALID")
        _reject(dx not in (-magnitude, magnitude) or dy != 0,
                "COMMAND_CONTRACT_INVALID", "pulse command 无效")
        net_x += dx
        _reject(pulse.get("per_pulse_shift_samples") is not None or
                pulse.get("per_pulse_gain") is not None or
                pulse.get("per_pulse_kernel") is not None,
                "PER_PULSE_TUNING_FORBIDDEN",
                "不得使用 per-pulse shift/gain/kernel")
        _reject(pulse.get("scene_cut_or_occlusion_detected") is not False,
                "SCENE_OR_WITNESS_INVALID",
                "pulse 存在 scene cut/occlusion 或字段缺失")
        _reject(pulse.get("capture_binding_semantic_sha256") !=
                capture_semantic,
                "CAPTURE_BINDING_INVALID", "pulse capture binding 漂移")
        _reject(pulse.get("completion_clock_session_id") !=
                    completion_session or
                pulse.get("mapping_segment_id") != mapping_segment,
                "CLOCK_MAPPING_INCOMPLETE",
                "pulse completion clock session/mapping segment 无效")

        phase_cell = pulse.get("phase_cell")
        _reject(phase_cell not in onsets_by_cell,
                "PHASE_CELL_CONTRACT_INVALID", "phase cell 无效")
        _validate_phase_interval(
            pulse.get("completion_phase_interval_q32"),
            phase_cell, tolerance)

        sentinel_position = pulse.get("sentinel_position")
        if sentinel_position is None:
            non_sentinel_count += 1
        else:
            _reject(sentinel_position not in sentinel_counts,
                    "SENTINEL_CONTRACT_INVALID", "sentinel position 无效")
            sentinel_counts[sentinel_position] += 1
        witnesses = pulse.get("witnesses")
        _reject(not isinstance(witnesses, dict) or
                set(witnesses.keys()) != set(WITNESSES),
                "MEASUREMENT_WITNESS_INVALID",
                "pulse 必须恰有 left/right witnesses")
        assert isinstance(witnesses, dict)
        for witness_name in WITNESSES:
            (lower, upper, onset, settled_lower,
             settled_upper) = _validate_witness(
                witnesses.get(witness_name), submission_session,
                mapping_segment)
            if sentinel_position is None:
                onsets_by_cell[phase_cell].add(onset)
                regular_lower.append(lower)
                regular_upper.append(upper)
                regular_settled_lower.append(settled_lower)
                regular_settled_upper.append(settled_upper)
            else:
                sentinel_lower.append(lower)
                sentinel_upper.append(upper)
                sentinel_settled_lower.append(settled_lower)
                sentinel_settled_upper.append(settled_upper)

    _reject(sentinel_counts != {"begin": 2, "middle": 2, "end": 2},
            "SENTINEL_CONTRACT_INVALID", "begin/middle/end sentinel 不完整")
    _reject(any(len(block) != 2 for block in blocks.values()),
            "BLOCK_BALANCE_INVALID", "每个 block 必须是正负 pulse pair")

    for block in blocks.values():
        first, second = block
        fields = ("phase_cell", "repeat_index", "polarity_order",
                  "sentinel_position", "row_index", "position_index")
        _reject(any(first.get(field) != second.get(field)
                    for field in fields) or
                first.get("pulse_ordinal") != 1 or
                second.get("pulse_ordinal") != 2 or
                second.get("sequence_index") !=
                    first.get("sequence_index") + 1,
                "BLOCK_BALANCE_INVALID", "block pair identity 无效")
        if first.get("polarity_order") == "positive_first":
            expected_commands = (magnitude, -magnitude)
        elif first.get("polarity_order") == "negative_first":
            expected_commands = (-magnitude, magnitude)
        else:
            expected_commands = None
        _reject(expected_commands is None or
                (first.get("command_dx_counts"),
                 second.get("command_dx_counts")) != expected_commands,
                "BLOCK_BALANCE_INVALID", "block polarity/sign 无效")
    _reject(net_x != 0, "COMMAND_CONTRACT_INVALID", "pulse net X 非零")

    sentinel_blocks = [block_id for block_id in block_order
                       if blocks[block_id][0].get("sentinel_position")
                       is not None]
    regular_blocks = [block_id for block_id in block_order
                      if blocks[block_id][0].get("sentinel_position") is None]
    _reject(len(regular_blocks) != 16,
            "BLOCK_BALANCE_INVALID",
            "完整 Williams 设计必须包含 16 个 signed-pair blocks")
    _reject(len(block_order) != 19 or len(sentinel_blocks) != 3 or
            [blocks[block_id][0].get("sentinel_position")
             for block_id in sentinel_blocks] !=
                ["begin", "middle", "end"] or
            [block_order.index(block_id) for block_id in sentinel_blocks] !=
                [0, 9, 18] or
            any(blocks[block_id][0].get("phase_cell") != "P1_8" or
                blocks[block_id][0].get("repeat_index") != 0 or
                blocks[block_id][0].get("row_index") is not None or
                blocks[block_id][0].get("position_index") is not None
                for block_id in sentinel_blocks),
            "SENTINEL_CONTRACT_INVALID", "sentinel 顺序/位置无效")

    phase_order = [blocks[block_id][0].get("phase_cell")
                   for block_id in regular_blocks]
    repeats = [blocks[block_id][0].get("repeat_index")
               for block_id in regular_blocks]
    polarity_orders = [blocks[block_id][0].get("polarity_order")
                       for block_id in regular_blocks]
    row_indices = [blocks[block_id][0].get("row_index")
                   for block_id in regular_blocks]
    position_indices = [blocks[block_id][0].get("position_index")
                        for block_id in regular_blocks]
    actual_rows = [tuple(phase_order[index:index + 4])
                   for index in range(0, len(phase_order), 4)]
    position_balance_valid = all(
        {row[position] for row in actual_rows} == set(PHASE_CELLS)
        for position in range(4))
    predecessor_counts = collections.Counter(
        (row[index], row[index + 1])
        for row in actual_rows for index in range(3))
    expected_predecessors = collections.Counter(
        (first, second) for first in PHASE_CELLS for second in PHASE_CELLS
        if first != second)
    predecessor_balance_valid = predecessor_counts == expected_predecessors
    _reject(actual_rows != list(WILLIAMS_ROWS) or
            repeats != [1] * 4 + [2] * 4 + [3] * 4 + [4] * 4 or
            row_indices != [0] * 4 + [1] * 4 + [2] * 4 + [3] * 4 or
            position_indices != list(range(4)) * 4 or
            polarity_orders.count("positive_first") != 8 or
            polarity_orders.count("negative_first") != 8 or
            not position_balance_valid or not predecessor_balance_valid or
            any(sum(1 for block_id in regular_blocks
                    if blocks[block_id][0].get("phase_cell") == phase_cell)
                != 4 for phase_cell in PHASE_CELLS) or
            any(sum(1 for block_id in regular_blocks
                    if blocks[block_id][0].get("phase_cell") == phase_cell and
                    blocks[block_id][0].get("polarity_order") == order) != 2
                for phase_cell in PHASE_CELLS
                for order in ("positive_first", "negative_first")),
            "BLOCK_BALANCE_INVALID",
            "phase position/predecessor/repeat/sign counterbalance 无效")

    return (onsets_by_cell, regular_lower, regular_upper,
            sentinel_lower, sentinel_upper, regular_settled_lower,
            regular_settled_upper, sentinel_settled_lower,
            sentinel_settled_upper, non_sentinel_count,
            sum(sentinel_counts.values()), len(regular_blocks))


def _validate_negative_controls(evidence: dict[str, Any],
                                capture_semantic: str,
                                completion_session: str,
                                submission_session: str,
                                mapping_segment: str,
                                tolerance: int) -> bool:
    controls = evidence.get("negative_controls")
    _reject(not isinstance(controls, list) or len(controls) != 4,
            "NEGATIVE_CONTROL_CONTRACT_INVALID",
            "每个 phase cell 必须有一个 no-command control")
    assert isinstance(controls, list)
    control_ids: set[str] = set()
    control_cells: list[str] = []
    transition_detected = False
    for control in controls:
        _reject(not isinstance(control, dict),
                "NEGATIVE_CONTROL_CONTRACT_INVALID",
                "negative control 必须是 object")
        assert isinstance(control, dict)
        control_id = control.get("control_id")
        phase_cell = control.get("phase_cell")
        _reject(not isinstance(control_id, str) or not control_id or
                control_id in control_ids or phase_cell not in PHASE_CELLS,
                "NEGATIVE_CONTROL_CONTRACT_INVALID",
                "negative control identity/cell 无效")
        control_ids.add(control_id)
        control_cells.append(phase_cell)
        _validate_phase_interval(
            control.get("scheduled_phase_interval_q32"),
            phase_cell, tolerance)
        dx = _exact_int(control.get("command_dx_counts"), "control X",
                        "NEGATIVE_CONTROL_CONTRACT_INVALID")
        dy = _exact_int(control.get("command_dy_counts"), "control Y",
                        "NEGATIVE_CONTROL_CONTRACT_INVALID")
        event_count = _exact_int(
            control.get("source_event_count"), "control source event count",
            "NEGATIVE_CONTROL_CONTRACT_INVALID")
        _reject(dx != 0 or dy != 0 or
                control.get("mouse_or_kmbox_event_emitted") is not False,
                "OUTPUT_OFF_CONTRACT_INVALID",
                "no-command control 不得发 Mouse/KMBOX event")
        _reject(event_count < 2 or
                control.get("capture_binding_semantic_sha256") !=
                    capture_semantic,
                "NEGATIVE_CONTROL_CONTRACT_INVALID",
                "negative control capture 无效")
        _reject(
                control.get("completion_clock_session_id") !=
                    completion_session or
                control.get("submission_clock_session_id") !=
                    submission_session or
                control.get("mapping_segment_id") != mapping_segment,
                "CLOCK_MAPPING_INCOMPLETE",
                "negative control clock/session 无效")
        _reject(control.get("scene_cut_detected") is not False,
                "SCENE_OR_WITNESS_INVALID",
                "negative control 存在 scene cut 或字段缺失")
        hash_fields = (
            "left_anchor_roi_bgr_sha256", "left_final_roi_bgr_sha256",
            "right_anchor_roi_bgr_sha256", "right_final_roi_bgr_sha256")
        _reject(any(not _is_sha256(control.get(field))
                    for field in hash_fields),
                "NEGATIVE_CONTROL_CONTRACT_INVALID",
                "negative control ROI hash 无效")
        transition_detected = transition_detected or (
            control["left_anchor_roi_bgr_sha256"] !=
                control["left_final_roi_bgr_sha256"] or
            control["right_anchor_roi_bgr_sha256"] !=
                control["right_final_roi_bgr_sha256"])
    _reject(control_cells != list(PHASE_CELLS),
            "NEGATIVE_CONTROL_CONTRACT_INVALID",
            "negative control phase order/cell 不完整")
    return transition_detected


def _evaluate(evidence: dict[str, Any]) -> dict[str, Any]:
    semantic = _validate_header(evidence)
    tolerance = _validate_phase_policy(evidence)
    (capture_semantic, completion_session, submission_session,
     mapping_segment) = _validate_capture_binding(evidence)
    magnitude = _validate_model_and_command_policy(evidence)
    (onsets_by_cell, regular_lower, regular_upper,
     sentinel_lower, sentinel_upper, regular_settled_lower,
     regular_settled_upper, sentinel_settled_lower,
     sentinel_settled_upper, non_sentinel_count, sentinel_count,
     regular_block_count) = _validate_pulses(
         evidence, capture_semantic, completion_session,
         submission_session, mapping_segment, tolerance, magnitude)
    negative_control_failed = _validate_negative_controls(
        evidence, capture_semantic, completion_session, submission_session,
        mapping_segment, tolerance)

    fixed_sample_null_deleted = len({
        next(iter(onsets)) for onsets in onsets_by_cell.values()
        if len(onsets) == 1
    }) > 1 and all(len(onsets) == 1 for onsets in onsets_by_cell.values())
    global_lower = max(regular_lower)
    global_upper = min(regular_upper)
    global_interval = ({
        "lower_open": global_lower,
        "upper_closed": global_upper,
    } if global_lower < global_upper else None)
    settled_lower = max(regular_settled_lower)
    settled_upper = min(regular_settled_upper)
    settled_interval = ({
        "lower_closed": settled_lower,
        "upper_closed": settled_upper,
    } if settled_lower <= settled_upper else None)
    sentinel_conjunction_valid = (
        global_interval is not None and settled_interval is not None and all(
        max(global_lower, lower) < min(global_upper, upper)
        for lower, upper in zip(sentinel_lower, sentinel_upper)) and all(
        max(settled_lower, lower) <= min(settled_upper, upper)
        for lower, upper in zip(
            sentinel_settled_lower, sentinel_settled_upper)))
    if not fixed_sample_null_deleted:
        reason_code = "PHASE_EXCITATION_INSUFFICIENT"
    elif global_interval is None:
        reason_code = "GLOBAL_COMPLETION_INTERVAL_EMPTY"
    elif settled_interval is None:
        reason_code = "SETTLED_DISPLACEMENT_PHASE_DEPENDENT"
    elif not sentinel_conjunction_valid:
        reason_code = "SENTINEL_DRIFT"
    elif negative_control_failed:
        reason_code = "NEGATIVE_CONTROL_FAILED"
    else:
        reason_code = None
    status = ("READY_FOR_SEALED_PHASE_VALIDATION" if reason_code is None
              else "COMPOSITE_PHASE_RESPONSE_DELETED")

    report: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration_evaluation",
        "status": status,
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "source": {
            "run_uuid": evidence.get("run_uuid"),
            "run_role": evidence.get("run_role"),
            "evidence_semantic_sha256": semantic,
            "schema_semantic_sha256":
                evidence["seal"]["schema_semantic_sha256"],
            "evaluator_file_sha256":
                evidence["seal"]["evaluator_file_sha256"],
            "order_manifest_sha256":
                evidence["seal"]["order_manifest_sha256"],
            "capture_binding_semantic_sha256": capture_semantic,
            "clock_mapping_evidence_sha256": evidence[
                "capture_binding"]["clock_mapping_evidence_sha256"],
            "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
        },
        "evaluation_contract": evaluation_contract(),
        "evaluation": {
            "reason_code": reason_code,
            "phase_cell_count": len(onsets_by_cell),
            "non_sentinel_pulse_count": non_sentinel_count,
            "sentinel_pulse_count": sentinel_count,
            "regular_signed_pair_block_count": regular_block_count,
            "position_balance_valid": True,
            "directed_predecessor_balance_valid": True,
            "fixed_sample_null_deleted": fixed_sample_null_deleted,
            "global_completion_interval_ns": global_interval,
            "global_settled_displacement_interval_q32": settled_interval,
            "sentinel_conjunction_valid": sentinel_conjunction_valid,
            "negative_control_failed": negative_control_failed,
        },
    }
    report["evaluation_semantic_sha256"] = _canonical_sha256(report)
    return report


def _incomplete_report(evidence: dict[str, Any],
                       reason_code: str) -> dict[str, Any]:
    semantic = evidence.get("evidence_semantic_sha256")
    report: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration_evaluation",
        "status": "PHASE_INPUT_INCOMPLETE",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "source": {
            "run_uuid": (evidence.get("run_uuid")
                         if isinstance(evidence.get("run_uuid"), str)
                         else None),
            "evidence_semantic_sha256": (
                semantic if _is_sha256(semantic) else None),
        },
        "evaluation_contract": evaluation_contract(),
        "evaluation": {
            "reason_code": reason_code,
            "phase_cell_count": None,
            "non_sentinel_pulse_count": None,
            "sentinel_pulse_count": None,
            "regular_signed_pair_block_count": None,
            "position_balance_valid": None,
            "directed_predecessor_balance_valid": None,
            "fixed_sample_null_deleted": None,
            "global_completion_interval_ns": None,
            "global_settled_displacement_interval_q32": None,
            "sentinel_conjunction_valid": None,
            "negative_control_failed": None,
        },
    }
    report["evaluation_semantic_sha256"] = _canonical_sha256(report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args()
    evidence: dict[str, Any] = {}
    return_code = 0
    try:
        evidence = _load_json(options.evidence.resolve())
        report = _evaluate(evidence)
    except EvidenceError as error:
        report = _incomplete_report(evidence, error.reason_code)
        print(f"phase evaluation rejected: {error.reason_code}",
              file=sys.stderr)
        return_code = 2
    except ValueError:
        report = _incomplete_report(evidence, "EVIDENCE_UNREADABLE")
        print("phase evaluation rejected: EVIDENCE_UNREADABLE",
              file=sys.stderr)
        return_code = 2
    try:
        _write_json_atomic(options.output, report)
    except ValueError as error:
        print(f"phase evaluation output failed: {type(error).__name__}",
              file=sys.stderr)
        return 3
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
