#!/usr/bin/env python3
"""通过公开 file CLI 验证 composite-phase calibration binder。"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


Q32 = 1 << 32
PHASE_CELLS = (
    ("P1_8", 1),
    ("P3_8", 3),
    ("P5_8", 5),
    ("P7_8", 7),
)
PHASE_NUMERATORS = dict(PHASE_CELLS)
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


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def schema_semantic_sha256() -> str:
    return canonical_sha256({
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration",
        "run_role": "CALIBRATION_DELETION",
        "phase_policy_id": "b-meas-phase-d1-v1",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
    })


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) +
        "\n", encoding="utf-8")


def seal_semantic(value: dict[str, Any], field: str) -> None:
    value.pop(field, None)
    value[field] = canonical_sha256(value)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("ascii")).hexdigest()


def build_inputs(binder: pathlib.Path, evaluator: pathlib.Path) -> tuple[
        dict[str, Any], dict[str, Any], dict[str, Any]]:
    mapping: dict[str, Any] = {
        "policy_id": "fixture-source-clock-v1",
        "completion_clock_session_id":
            "fixture-completion-clock-session",
        "submission_clock_session_id":
            "fixture-submission-clock-session",
        "mapping_segment_id": "fixture-common-monotonic-segment",
        "valid_from_common_ns": 9_000_000_000,
        "valid_through_common_ns": 20_000_000_000,
        "source_clock_uncertainty_ns": 10_000,
        "qpc_quantization_ns": 100,
        "read_access_interval_ns": 1_000,
        "maximum_fit_residual_ns": 2_000,
        "ntp_offset_interval_ns": [-3_000, 3_000],
        "ntp_round_trip_delay_ns": 4_000,
        "ntp_dispersion_ns": 2_000,
        "ntp_jitter_ns": 1_000,
        "calibration_samples_sha256": "9" * 64,
        "policy_sha256": "8" * 64,
        "stale": False,
    }
    seal_semantic(mapping, "semantic_sha256")

    binding: dict[str, Any] = {
        "source_name": "synthetic-ndi-source",
        "source_geometry": [2560, 1440],
        "roi_geometry": [1120, 560, 320, 320],
        "source_rate": {"numerator": 240, "denominator": 1},
        "pixel_format": "CPU_BGR",
        "capture_stack": {
            "producer": "OBS",
            "producer_version": "fixture-version",
            "capture_method_configured": "DXGI",
            "capture_method_resolved": "DXGI",
        },
        "boundary_semantic": "NDI_SDK_SUBMISSION_UTC",
        "boundary_claim_limit": "use_exact_selected_semantic_only",
        "boundary_is_capture_or_exposure": False,
        "ndi_frame_sync_used": False,
        "clock_mapping_policy_id": mapping["policy_id"],
        "clock_mapping_evidence_sha256": mapping["semantic_sha256"],
        "clock_mapping_stale": False,
        "completion_clock_session_id":
            mapping["completion_clock_session_id"],
        "submission_clock_session_id":
            mapping["submission_clock_session_id"],
        "mapping_segment_id": mapping["mapping_segment_id"],
        "scene_binding_sha256": "c" * 64,
        "left_witness_roi": [16, 48, 96, 224],
        "right_witness_roi": [208, 48, 96, 224],
    }
    seal_semantic(binding, "semantic_sha256")

    tolerance = Q32 // 16
    half_width = Q32 // 64
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

    append_pair(
        "S-BEGIN", "P1_8", 0, None, None,
        "positive_first", "begin")
    for row_index, row in enumerate(WILLIAMS_ROWS):
        if row_index == 2:
            append_pair(
                "S-MIDDLE", "P1_8", 0, None, None,
                "negative_first", "middle")
        for position_index, phase_cell in enumerate(row):
            append_pair(
                f"R{row_index}-{position_index}-{phase_cell}", phase_cell,
                row_index + 1, row_index, position_index,
                ("positive_first"
                 if (row_index + position_index) % 2 == 0
                 else "negative_first"), None)
    append_pair(
        "S-END", "P1_8", 0, None, None,
        "positive_first", "end")

    controls = []
    for phase_cell, numerator in PHASE_CELLS:
        center = numerator * Q32 // 8
        controls.append({
            "control_id": f"NC-{phase_cell}",
            "phase_cell": phase_cell,
            "window_id": f"NC-{phase_cell}",
            "scheduled_phase_interval_q32": {
                "lower_closed": center - half_width,
                "upper_closed": center + half_width,
            },
            "command_dx_counts": 0,
            "command_dy_counts": 0,
            "mouse_or_kmbox_event_emitted": False,
        })

    order_manifest = [
        {field: pulse[field] for field in ORDER_FIELDS}
        for pulse in pulses
    ]
    control_after_sequence = {
        9: "NC-P1_8",
        17: "NC-P3_8",
        27: "NC-P5_8",
        35: "NC-P7_8",
    }
    window_order: list[str] = []
    for pulse in pulses:
        window_order.append(pulse["pulse_id"])
        control_id = control_after_sequence.get(pulse["sequence_index"])
        if control_id is not None:
            window_order.append(control_id)
    plan: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_calibration_plan",
        "run_role": "CALIBRATION_DELETION",
        "run_uuid": "12345678-1234-4234-8234-123456789abc",
        "activation_epoch": 123456789,
        "scope_id": "a" * 64,
        "status": "FROZEN_BEFORE_CAPTURE",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "frozen_at_steady_ns": 1_000_000_000,
        "seen_diagnosis_denylist": {
            "run_uuids": [SEEN_RUN_UUID],
            "artifact_sha256s": list(SEEN_ARTIFACT_SHA256),
        },
        "phase_policy": {
            "policy_id": "b-meas-phase-d1-v1",
            "phase_scale": "Q0.32_CYCLE",
            "phase_cells": [
                {"phase_cell": cell, "center_numerator": numerator,
                 "center_denominator": 8}
                for cell, numerator in PHASE_CELLS
            ],
            "target_tolerance_q32": tolerance,
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
        "capture_binding": binding,
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
                "CONSERVATIVE_INTEGER_Q0_32_FROM_ADJACENT_BOUNDARIES",
        },
        "pulses": pulses,
        "negative_controls": controls,
        "window_order": window_order,
        "seal": {
            "schema_semantic_sha256": schema_semantic_sha256(),
            "evaluator_file_sha256": file_sha256(evaluator),
            "binder_file_sha256": file_sha256(binder),
            "order_manifest_sha256": canonical_sha256(order_manifest),
            "model_semantic_sha256": None,
            "response_revealed_before_freeze": False,
        },
    }
    seal_semantic(plan, "plan_semantic_sha256")

    frames: list[dict[str, Any]] = []
    events: list[dict[str, Any]] = []
    source_sequence = 0
    window_ordinal = 0

    def append_frame(window_id: str, sample_index: int, boundary_ns: int,
                     left_hash: str, right_hash: str) -> None:
        nonlocal source_sequence
        frames.append({
            "frame_event_id": f"frame-{source_sequence}",
            "source_sequence": source_sequence,
            "receiver_sequence": source_sequence,
            "window_id": window_id,
            "sample_index": sample_index,
            "submission_clock_session_id":
                mapping["submission_clock_session_id"],
            "mapping_segment_id": mapping["mapping_segment_id"],
            "boundary_time_interval_ns": {
                "lower_closed": boundary_ns - 10_000,
                "upper_closed": boundary_ns + 10_000,
            },
            "full_bgr_sha256": sha256_text(
                f"full-{source_sequence}"),
            "left_roi_bgr_sha256": left_hash,
            "right_roi_bgr_sha256": right_hash,
            "left_normalized_displacement_interval_q32": {
                "lower_closed": 2_000_000_000,
                "upper_closed": 2_200_000_000,
            },
            "right_normalized_displacement_interval_q32": {
                "lower_closed": 2_000_000_000,
                "upper_closed": 2_200_000_000,
            },
            "scene_cut_or_occlusion_detected": False,
            "frame_sync_reused": False,
        })
        source_sequence += 1

    command_controls = []
    pulse_by_id = {pulse["pulse_id"]: pulse for pulse in pulses}
    control_by_window = {
        control["window_id"]: control for control in controls
    }
    for window_id in window_order:
        base_ns = 10_000_000_000 + window_ordinal * 100_000_000
        if window_id in pulse_by_id:
            pulse = pulse_by_id[window_id]
            numerator = PHASE_NUMERATORS[pulse["phase_cell"]]
            completion_ns = base_ns + numerator * 1_000_000
            onset = 3 if numerator in (1, 3) else 4
            left_anchor = sha256_text(
                f"{pulse['pulse_id']}-left-anchor")
            right_anchor = sha256_text(
                f"{pulse['pulse_id']}-right-anchor")
            left_changed = sha256_text(
                f"{pulse['pulse_id']}-left-changed")
            right_changed = sha256_text(
                f"{pulse['pulse_id']}-right-changed")
            for sample_index in range(6):
                append_frame(
                    pulse["pulse_id"], sample_index,
                    base_ns + sample_index * 8_000_000,
                    left_anchor if sample_index < onset else left_changed,
                    right_anchor if sample_index < onset else right_changed)
            events.append({
                "pulse_id": pulse["pulse_id"],
                "sequence_index": pulse["sequence_index"],
                "command_dx_counts": pulse["command_dx_counts"],
                "command_dy_counts": pulse["command_dy_counts"],
                "backend_completion_event_id":
                    f"completion-{pulse['sequence_index']}",
                "delivery_ack_id": f"ack-{pulse['sequence_index']}",
                "backend_completed": True,
                "delivery_acked": True,
                "completion_clock_session_id":
                    mapping["completion_clock_session_id"],
                "mapping_segment_id": mapping["mapping_segment_id"],
                "completion_time_interval_ns": {
                    "lower_closed": completion_ns - 5_000,
                    "upper_closed": completion_ns + 5_000,
                },
            })
        else:
            control = control_by_window[window_id]
            left_anchor = sha256_text(
                f"{control['control_id']}-left-anchor")
            right_anchor = sha256_text(
                f"{control['control_id']}-right-anchor")
            for sample_index in range(6):
                append_frame(
                    control["window_id"], sample_index,
                    base_ns + sample_index * 8_000_000,
                    left_anchor, right_anchor)
            command_controls.append({
                "control_id": control["control_id"],
                "command_dx_counts": 0,
                "command_dy_counts": 0,
                "mouse_or_kmbox_event_emitted": False,
                "schedule_marker_event_id":
                    f"schedule-{control['control_id']}",
                "completion_clock_session_id":
                    mapping["completion_clock_session_id"],
                "mapping_segment_id": mapping["mapping_segment_id"],
                "scheduled_marker_time_interval_ns": {
                    "lower_closed": base_ns +
                        PHASE_NUMERATORS[control["phase_cell"]] *
                        1_000_000 - 5_000,
                    "upper_closed": base_ns +
                        PHASE_NUMERATORS[control["phase_cell"]] *
                        1_000_000 + 5_000,
                },
            })
        window_ordinal += 1

    capture: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_capture_ledger",
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "scope_id": plan["scope_id"],
        "status": "CAPTURE_COMPLETE",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "capture_binding_semantic_sha256": binding["semantic_sha256"],
        "acquisition_started_at_steady_ns": 2_000_000_000,
        "revealed_at_steady_ns": 3_000_000_000,
        "clock_mapping": mapping,
        "frames": frames,
    }
    seal_semantic(capture, "capture_semantic_sha256")

    commands: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_command_ledger",
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "scope_id": plan["scope_id"],
        "status": "COMMANDS_COMPLETE",
        "ledger_is_read_only_record": True,
        "binder_physical_output_capability": False,
        "binder_physical_dispatch_count": 0,
        "capture_binding_semantic_sha256": binding["semantic_sha256"],
        "clock_mapping_semantic_sha256": mapping["semantic_sha256"],
        "source_dispatch_count": len(events),
        "source_backend_completion_count": len(events),
        "source_delivery_ack_count": len(events),
        "events": events,
        "negative_controls": command_controls,
    }
    seal_semantic(commands, "command_semantic_sha256")
    return plan, capture, commands


def invoke_binder(
        binder: pathlib.Path, root: pathlib.Path, stem: str,
        plan: dict[str, Any], capture: dict[str, Any],
        commands: dict[str, Any]) -> tuple[
            subprocess.CompletedProcess[str], dict[str, Any] | None,
            tuple[pathlib.Path, ...]]:
    paths = (
        root / f"{stem}-plan.json",
        root / f"{stem}-capture.json",
        root / f"{stem}-commands.json",
        root / f"{stem}-evidence.json",
    )
    for path, value in zip(paths[:3], (plan, capture, commands)):
        write_json(path, value)
    completed = subprocess.run([
        sys.executable, str(binder),
        "--plan", str(paths[0].resolve()),
        "--capture", str(paths[1].resolve()),
        "--commands", str(paths[2].resolve()),
        "--output", str(paths[3].resolve()),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    output = (json.loads(paths[3].read_text(encoding="utf-8"))
              if paths[3].is_file() else None)
    return completed, output, paths


def run_binder(
        binder: pathlib.Path, evaluator: pathlib.Path, root: pathlib.Path,
        stem: str) -> tuple[subprocess.CompletedProcess[str],
                            dict[str, Any] | None, tuple[pathlib.Path, ...]]:
    return invoke_binder(
        binder, root, stem, *build_inputs(binder, evaluator))


def expect_rejected(
        binder: pathlib.Path, root: pathlib.Path, stem: str,
        plan: dict[str, Any], capture: dict[str, Any],
        commands: dict[str, Any], reason_code: str) -> None:
    completed, evidence, paths = invoke_binder(
        binder, root, stem, plan, capture, commands)
    expect(completed.returncode == 2 and evidence is None and
           reason_code in completed.stderr and not paths[3].exists(),
           f"{stem} 必须 fail closed 为 {reason_code}: {completed.stderr}")


def run_evaluator(evaluator: pathlib.Path, evidence_path: pathlib.Path,
                  output_path: pathlib.Path) -> tuple[
                      subprocess.CompletedProcess[str], dict[str, Any]]:
    completed = subprocess.run([
        sys.executable, str(evaluator),
        "--evidence", str(evidence_path.resolve()),
        "--output", str(output_path.resolve()),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    report = json.loads(output_path.read_text(encoding="utf-8"))
    return completed, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binder", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    options = parser.parse_args()
    binder = options.binder.resolve()
    evaluator = options.evaluator.resolve()
    expect(binder.is_file(), "calibration binder 公开 CLI 尚不存在")

    source = binder.read_text(encoding="utf-8")
    parsed = ast.parse(source, filename=str(binder))
    imported_roots: set[str] = set()
    for node in ast.walk(parsed):
        if isinstance(node, ast.Import):
            imported_roots.update(
                alias.name.split(".", 1)[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module is not None:
            imported_roots.add(node.module.split(".", 1)[0])
    expect(not imported_roots.intersection({
        "ctypes", "keyboard", "mouse", "serial", "socket", "subprocess",
        "win32api", "win32con",
    }), "binder 不得 import Mouse/KMBOX/transport 能力")
    help_result = subprocess.run(
        [sys.executable, str(binder), "--help"], check=False,
        capture_output=True, text=True, encoding="utf-8")
    help_text = (help_result.stdout + help_result.stderr).lower()
    expect(help_result.returncode == 0 and
           all(flag in help_text for flag in (
               "--plan", "--capture", "--commands", "--output")) and
           all(term not in help_text for term in (
               "allowphysicaloutput", "confirmation", "device-address")),
           "公开 CLI 只能消费三个离线账本并创建 evidence")

    with tempfile.TemporaryDirectory(
            prefix="xen-composite-phase-binder-") as temporary:
        root = pathlib.Path(temporary)
        completed, evidence, paths = run_binder(
            binder, evaluator, root, "valid")
        expect(completed.returncode == 0 and evidence is not None,
               f"完整离线账本必须绑定为 evidence: {completed.stderr}")
        assert evidence is not None
        expect(evidence["status"] == "RECORDED_UNANALYZED" and
               evidence["physical_output_capability"] is False and
               evidence["physical_dispatch_count"] == 0 and
               evidence["source_run_was_sealed_before_capture"] is True,
               "binder 输出必须保持 output-off 与 pre-capture seal")
        bound_plan = json.loads(paths[0].read_text(encoding="utf-8"))
        bound_capture = json.loads(paths[1].read_text(encoding="utf-8"))
        expect(evidence["measurement_policy"] ==
                   bound_plan["measurement_policy"] and
               evidence["acquisition_summary"] == {
                   "source_dispatch_count": 38,
                   "source_backend_completion_count": 38,
                   "source_delivery_ack_count": 38,
                   "source_frame_event_count": 252,
                   "pulse_window_count": 38,
                   "negative_control_window_count": 4,
                   "clock_mapping_semantic_sha256":
                       bound_capture["clock_mapping"]["semantic_sha256"],
               }, "evidence 必须自包含 measurement 与 acquisition 摘要")
        expected_lower = (985_000 * Q32) // 8_020_000
        expected_upper = (
            1_015_000 * Q32 + 7_980_000 - 1) // 7_980_000
        first = evidence["pulses"][0]
        expect(first["completion_phase_interval_q32"] == {
                   "lower_closed": expected_lower,
                   "upper_closed": expected_upper,
               } and
               first["witnesses"]["left"][
                   "visible_transition_offset_samples"] == 3 and
               first["witnesses"]["left"][
                   "last_same_submission_after_completed_ns"] ==
                   14_985_000 and
               first["witnesses"]["left"][
                   "first_changed_submission_after_completed_ns"] ==
                   23_015_000 and
               first["completion_phase_source_boundaries"] == {
                   "previous_frame_event_id": "frame-0",
                   "previous_source_sequence": 0,
                   "following_frame_event_id": "frame-1",
                   "following_source_sequence": 1,
               } and
               first["source_command_evidence"] == {
                   "backend_completion_event_id": "completion-0",
                   "delivery_ack_id": "ack-0",
                   "completion_time_interval_ns": {
                       "lower_closed": 10_000_995_000,
                       "upper_closed": 10_001_005_000,
                   },
               } and
               first["witnesses"]["left"][
                   "anchor_frame_event_id"] == "frame-0" and
               first["witnesses"]["left"][
                   "last_same_frame_event_id"] == "frame-2" and
               first["witnesses"]["left"][
                   "first_changed_frame_event_id"] == "frame-3" and
               first["witnesses"]["left"][
                   "settled_frame_event_id"] == "frame-5",
               "phase 与 exact-BGR transition 必须由原始区间/帧账计算")
        expect(evidence["input_bindings"] == {
                   "plan_file_sha256": file_sha256(paths[0]),
                   "capture_file_sha256": file_sha256(paths[1]),
                   "command_file_sha256": file_sha256(paths[2]),
                   "plan_semantic_sha256":
                       json.loads(paths[0].read_text(encoding="utf-8"))[
                           "plan_semantic_sha256"],
                   "capture_semantic_sha256":
                       json.loads(paths[1].read_text(encoding="utf-8"))[
                           "capture_semantic_sha256"],
                   "command_semantic_sha256":
                       json.loads(paths[2].read_text(encoding="utf-8"))[
                           "command_semantic_sha256"],
                   "binder_file_sha256": file_sha256(binder),
               }, "输出必须绑定三个原始账本及 binder 文件身份")
        expect(evidence["negative_controls"][0][
                   "scheduled_phase_interval_q32"] == {
                       "lower_closed": expected_lower,
                       "upper_closed": expected_upper,
                   } and
               evidence["negative_controls"][0][
                   "schedule_marker_event_id"] == "schedule-NC-P1_8" and
               evidence["negative_controls"][0][
                   "source_event_count"] == 6 and
               evidence["negative_controls"][0][
                   "scheduled_phase_source_boundaries"] == {
                       "previous_frame_event_id": "frame-60",
                       "previous_source_sequence": 60,
                       "following_frame_event_id": "frame-61",
                       "following_source_sequence": 61,
                   },
               "no-command control phase 必须由实际 schedule marker 重算")
        semantic_input = dict(evidence)
        semantic = semantic_input.pop("evidence_semantic_sha256", None)
        expect(semantic == canonical_sha256(semantic_input),
               "binder evidence 必须受完整 semantic SHA-256 绑定")

        evaluation_path = root / "valid-evaluation.json"
        evaluated, report = run_evaluator(
            evaluator, paths[3], evaluation_path)
        expect(evaluated.returncode == 0 and
               report["status"] == "READY_FOR_SEALED_PHASE_VALIDATION" and
               report["evaluation"]["global_completion_interval_ns"] == {
                   "lower_open": 18_985_000,
                   "upper_closed": 21_015_000,
               }, "binder 产物必须被冻结 evaluator 直接接受")

        first_bytes = paths[3].read_bytes()
        collided = subprocess.run([
            sys.executable, str(binder),
            "--plan", str(paths[0].resolve()),
            "--capture", str(paths[1].resolve()),
            "--commands", str(paths[2].resolve()),
            "--output", str(paths[3].resolve()),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(collided.returncode == 3 and
               paths[3].read_bytes() == first_bytes,
               "binder 不得覆盖既有 evidence")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["status"] = "MUTATED_AFTER_FREEZE"
        expect_rejected(
            binder, root, "plan-semantic-drift", plan, capture, commands,
            "PLAN_SEAL_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["seal"]["binder_file_sha256"] = "f" * 64
        seal_semantic(plan, "plan_semantic_sha256")
        expect_rejected(
            binder, root, "binder-identity-drift", plan, capture, commands,
            "PLAN_SEAL_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["pulses"][0]["sequence_index"] = 99
        seal_semantic(plan, "plan_semantic_sha256")
        expect_rejected(
            binder, root, "order-seal-drift", plan, capture, commands,
            "PLAN_SEAL_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["pulses"] = plan["pulses"][:-2]
        plan["seal"]["order_manifest_sha256"] = canonical_sha256([
            {field: pulse[field] for field in ORDER_FIELDS}
            for pulse in plan["pulses"]
        ])
        seal_semantic(plan, "plan_semantic_sha256")
        expect_rejected(
            binder, root, "incomplete-williams-plan",
            plan, capture, commands, "DESIGN_BALANCE_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["window_order"][10], plan["window_order"][11] = \
            plan["window_order"][11], plan["window_order"][10]
        seal_semantic(plan, "plan_semantic_sha256")
        expect_rejected(
            binder, root, "window-order-drift", plan, capture, commands,
            "DESIGN_BALANCE_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["physical_output_capability"] = True
        seal_semantic(plan, "plan_semantic_sha256")
        expect_rejected(
            binder, root, "binder-output-capability",
            plan, capture, commands, "OUTPUT_OFF_CONTRACT_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["status"] = "MUTATED"
        expect_rejected(
            binder, root, "capture-semantic-drift", plan, capture, commands,
            "CAPTURE_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["status"] = "MUTATED"
        expect_rejected(
            binder, root, "command-semantic-drift", plan, capture, commands,
            "COMMAND_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["scope_id"] = "e" * 64
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "capture-identity-drift", plan, capture, commands,
            "INPUT_IDENTITY_MISMATCH")

        plan, capture, commands = build_inputs(binder, evaluator)
        plan["run_uuid"] = SEEN_RUN_UUID
        capture["run_uuid"] = SEEN_RUN_UUID
        commands["run_uuid"] = SEEN_RUN_UUID
        seal_semantic(plan, "plan_semantic_sha256")
        seal_semantic(capture, "capture_semantic_sha256")
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "seen-run-reuse", plan, capture, commands,
            "INPUT_IDENTITY_MISMATCH")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["frames"][1]["source_sequence"] = 0
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "duplicate-source-event", plan, capture, commands,
            "FRAME_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        short_control = plan["negative_controls"][0]["window_id"]
        removed = next(
            index for index, frame in enumerate(capture["frames"])
            if frame["window_id"] == short_control and
            frame["sample_index"] == 5)
        capture["frames"].pop(removed)
        for sequence, frame in enumerate(capture["frames"]):
            frame["source_sequence"] = sequence
            frame["receiver_sequence"] = sequence
            frame["frame_event_id"] = f"frame-{sequence}"
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "short-no-command-window",
            plan, capture, commands, "FRAME_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["frames"][0]["submission_clock_session_id"] = \
            "different-session"
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "mixed-clock-session", plan, capture, commands,
            "CLOCK_MAPPING_INCOMPLETE")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["clock_mapping"]["stale"] = True
        seal_semantic(capture["clock_mapping"], "semantic_sha256")
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "stale-clock-mapping", plan, capture, commands,
            "CLOCK_MAPPING_STALE")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["frames"][0]["boundary_time_interval_ns"][
            "lower_closed"] = 9_999_990_000.0
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "float-boundary", plan, capture, commands,
            "FRAME_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["events"][0]["delivery_acked"] = False
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "missing-ack", plan, capture, commands,
            "COMMAND_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["source_delivery_ack_count"] -= 1
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "ack-count-drift", plan, capture, commands,
            "COMMAND_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["events"][0]["completion_clock_session_id"] = \
            "different-session"
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "completion-clock-session-drift",
            plan, capture, commands, "CLOCK_MAPPING_INCOMPLETE")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["events"][0]["command_dy_counts"] = 1
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "nonzero-y", plan, capture, commands,
            "COMMAND_LEDGER_INVALID")

        plan, capture, commands = build_inputs(binder, evaluator)
        completion = commands["events"][0]["completion_time_interval_ns"]
        completion["lower_closed"] = 10_007_995_000
        completion["upper_closed"] = 10_008_005_000
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "phase-crosses-boundary", plan, capture, commands,
            "PHASE_CELL_AMBIGUOUS")

        plan, capture, commands = build_inputs(binder, evaluator)
        commands["events"][0]["completion_time_interval_ns"] = {
            "lower_closed": 10_002_995_000,
            "upper_closed": 10_003_005_000,
        }
        seal_semantic(commands, "command_semantic_sha256")
        expect_rejected(
            binder, root, "phase-outside-preassigned-cell",
            plan, capture, commands, "PHASE_CELL_AMBIGUOUS")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["frames"][1]["boundary_time_interval_ns"] = {
            "lower_closed": 10_009_990_000,
            "upper_closed": 10_010_010_000,
        }
        commands["events"][0]["completion_time_interval_ns"] = {
            "lower_closed": 10_001_245_000,
            "upper_closed": 10_001_255_000,
        }
        seal_semantic(capture, "capture_semantic_sha256")
        seal_semantic(commands, "command_semantic_sha256")
        completed, irregular, _ = invoke_binder(
            binder, root, "irregular-cadence", plan, capture, commands)
        irregular_lower = (1_235_000 * Q32) // 10_020_000
        irregular_upper = (
            1_265_000 * Q32 + 9_980_000 - 1) // 9_980_000
        expect(completed.returncode == 0 and irregular is not None and
               irregular["pulses"][0][
                   "completion_phase_interval_q32"] == {
                       "lower_closed": irregular_lower,
                       "upper_closed": irregular_upper,
                   }, "irregular cadence 必须按 boundary 时间区间计算 phase")

        plan, capture, commands = build_inputs(binder, evaluator)
        first_window = plan["pulses"][0]["pulse_id"]
        first_frames = [
            frame for frame in capture["frames"]
            if frame["window_id"] == first_window
        ]
        anchor_hash = first_frames[0]["left_roi_bgr_sha256"]
        for frame in first_frames:
            frame["left_roi_bgr_sha256"] = anchor_hash
        seal_semantic(capture, "capture_semantic_sha256")
        expect_rejected(
            binder, root, "flat-left-witness", plan, capture, commands,
            "NO_VISIBLE_TRANSITION")

        plan, capture, commands = build_inputs(binder, evaluator)
        control_id = plan["negative_controls"][0]["window_id"]
        control_frames = [
            frame for frame in capture["frames"]
            if frame["window_id"] == control_id
        ]
        control_frames[1]["left_roi_bgr_sha256"] = "d" * 64
        seal_semantic(capture, "capture_semantic_sha256")
        completed, changed_control, changed_paths = invoke_binder(
            binder, root, "changed-no-command-control",
            plan, capture, commands)
        expect(completed.returncode == 0 and changed_control is not None,
               "no-command pixel change 应保留为 deletion evidence")
        control_eval, control_report = run_evaluator(
            evaluator, changed_paths[3], root / "changed-control-eval.json")
        expect(control_eval.returncode == 0 and
               control_report["status"] ==
                   "COMPOSITE_PHASE_RESPONSE_DELETED" and
               control_report["evaluation"]["reason_code"] ==
                   "NEGATIVE_CONTROL_FAILED",
               "no-command transition 必须删除 phase-only mechanism")

        plan, capture, commands = build_inputs(binder, evaluator)
        capture["frames"][0]["scene_cut_or_occlusion_detected"] = True
        seal_semantic(capture, "capture_semantic_sha256")
        completed, scene_evidence, scene_paths = invoke_binder(
            binder, root, "scene-cut", plan, capture, commands)
        expect(completed.returncode == 0 and scene_evidence is not None,
               "scene 标志必须作为原始事实进入 evidence")
        scene_eval, scene_report = run_evaluator(
            evaluator, scene_paths[3], root / "scene-cut-eval.json")
        expect(scene_eval.returncode == 2 and
               scene_report["status"] == "PHASE_INPUT_INCOMPLETE" and
               scene_report["evaluation"]["reason_code"] ==
                   "SCENE_OR_WITNESS_INVALID",
               "scene cut 不得被 binder 或 evaluator 隐去")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
