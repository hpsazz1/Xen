#!/usr/bin/env python3
"""通过公开 CLI 验证 composite-phase calibration evaluator。"""

from __future__ import annotations

import argparse
import ast
import copy
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
WILLIAMS_ROWS = (
    ("P1_8", "P3_8", "P7_8", "P5_8"),
    ("P3_8", "P5_8", "P1_8", "P7_8"),
    ("P5_8", "P7_8", "P3_8", "P1_8"),
    ("P7_8", "P1_8", "P5_8", "P3_8"),
)
PHASE_NUMERATORS = dict(PHASE_CELLS)
SEEN_RUN_UUID = "574e8e49-4219-4424-8a59-d3b1bf345a11"
SEEN_ARTIFACT_SHA256 = (
    "5194a97c01420f693275e9c7dc4130b03e526e1bfd7aa0de1376e33c2ed1000e",
    "06a0a8c6db6d16f086b638c7a8bf6b45861d2e51ca714cd82d627cf477110b47",
    "126d4b7590b7a88749158cba1a8ccf3bc8f8f15db817bc36ffbff479b4038c52",
    "d7171ade6196f32a69c9fcd686827041322c7307e540f2b514e86abd626e7abc",
)
EVALUATOR_UNDER_TEST: pathlib.Path | None = None


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
        "phase_policy_id": "b-meas-phase-d1-v2",
        "counterbalance_design": "WILLIAMS_4X4_FIRST_ORDER",
        "timestamp_boundary": "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE",
    })


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def capture_binding() -> dict[str, Any]:
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
        "clock_mapping_policy_id": "fixture-source-clock-v1",
        "clock_mapping_evidence_sha256": "b" * 64,
        "clock_mapping_stale": False,
        "completion_clock_session_id": "fixture-completion-clock-session",
        "submission_clock_session_id": "fixture-submission-clock-session",
        "mapping_segment_id": "fixture-common-monotonic-segment",
        "scene_binding_sha256": "c" * 64,
        "left_witness_roi": [16, 48, 96, 224],
        "right_witness_roi": [208, 48, 96, 224],
    }
    binding["semantic_sha256"] = canonical_sha256(binding)
    return binding


def transition_witness(lower_ns: int, upper_ns: int,
                       offset_samples: int, salt: str) -> dict[str, Any]:
    return {
        "visible_transition_found": True,
        "anchor_roi_bgr_sha256": hashlib.sha256(
            f"{salt}-anchor".encode("ascii")).hexdigest(),
        "last_same_roi_bgr_sha256": hashlib.sha256(
            f"{salt}-anchor".encode("ascii")).hexdigest(),
        "first_changed_roi_bgr_sha256": hashlib.sha256(
            f"{salt}-changed".encode("ascii")).hexdigest(),
        "last_same_submission_after_completed_ns": lower_ns,
        "first_changed_submission_after_completed_ns": upper_ns,
        "visible_transition_offset_samples": offset_samples,
        "source_clock_uncertainty_ns": 150_000,
        "submission_clock_session_id": "fixture-submission-clock-session",
        "mapping_segment_id": "fixture-common-monotonic-segment",
        "interval_endpoints_include_mapping_uncertainty": True,
        "settled_normalized_displacement_interval_q32": {
            "lower_closed": 2_000_000_000,
            "upper_closed": 2_200_000_000,
        },
    }


def build_evidence() -> dict[str, Any]:
    if EVALUATOR_UNDER_TEST is None:
        raise RuntimeError("evaluator identity 尚未绑定")
    binding = capture_binding()
    dynamic_capture_fields = {
        "clock_mapping_evidence_sha256", "clock_mapping_stale",
        "completion_clock_session_id", "submission_clock_session_id",
        "mapping_segment_id", "semantic_sha256",
    }
    capture_policy = {
        key: value for key, value in binding.items()
        if key not in dynamic_capture_fields
    }
    capture_policy["semantic_sha256"] = canonical_sha256(capture_policy)
    binding["capture_policy_semantic_sha256"] = \
        capture_policy["semantic_sha256"]
    binding.pop("semantic_sha256", None)
    binding["semantic_sha256"] = canonical_sha256(binding)
    tolerance = Q32 // 16
    phase_interval_half_width = Q32 // 64
    pulses: list[dict[str, Any]] = []
    sequence_index = 0

    def append_pair(block_id: str, phase_cell: str, numerator: int,
                    repeat_index: int, row_index: int | None,
                    position_index: int | None, polarity_order: str,
                    sentinel_position: str | None) -> None:
        nonlocal sequence_index
        center = numerator * Q32 // 8
        commands = (1, -1) if polarity_order == "positive_first" else (-1, 1)
        onset = 4 if numerator in (1, 3) else 3
        for pulse_ordinal, command in enumerate(commands, 1):
            salt = f"{block_id}-{pulse_ordinal}"
            pulses.append({
                "pulse_id": salt,
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
                "completion_clock_session_id":
                    "fixture-completion-clock-session",
                "mapping_segment_id": "fixture-common-monotonic-segment",
                "scene_cut_or_occlusion_detected": False,
                "completion_phase_interval_q32": {
                    "lower_closed": center - phase_interval_half_width,
                    "upper_closed": center + phase_interval_half_width,
                },
                "capture_binding_semantic_sha256":
                    binding["semantic_sha256"],
                "per_pulse_shift_samples": None,
                "per_pulse_gain": None,
                "per_pulse_kernel": None,
                "witnesses": {
                    "left": transition_witness(
                        10_000_000, 14_000_000, onset, f"{salt}-left"),
                    "right": transition_witness(
                        10_500_000, 14_500_000, onset, f"{salt}-right"),
                },
            })
            sequence_index += 1

    append_pair(
        "S-BEGIN", "P1_8", 1, 0, None, None,
        "positive_first", "begin")
    for row_index, row in enumerate(WILLIAMS_ROWS):
        if row_index == 2:
            append_pair(
                "S-MIDDLE", "P1_8", 1, 0, None, None,
                "negative_first", "middle")
        for position_index, cell_id in enumerate(row):
            append_pair(
                f"R{row_index}-{position_index}-{cell_id}", cell_id,
                PHASE_NUMERATORS[cell_id], row_index + 1,
                row_index, position_index,
                ("positive_first"
                 if (row_index + position_index) % 2 == 0
                 else "negative_first"), None)
    append_pair(
        "S-END", "P1_8", 1, 0, None, None,
        "positive_first", "end")

    negative_controls: list[dict[str, Any]] = []
    for cell_id, numerator in PHASE_CELLS:
        center = numerator * Q32 // 8
        anchor = hashlib.sha256(
            f"control-{cell_id}-anchor".encode("ascii")).hexdigest()
        negative_controls.append({
            "control_id": f"NC-{cell_id}",
            "phase_cell": cell_id,
            "scheduled_phase_interval_q32": {
                "lower_closed": center - phase_interval_half_width,
                "upper_closed": center + phase_interval_half_width,
            },
            "command_dx_counts": 0,
            "command_dy_counts": 0,
            "mouse_or_kmbox_event_emitted": False,
            "capture_binding_semantic_sha256":
                binding["semantic_sha256"],
            "completion_clock_session_id":
                "fixture-completion-clock-session",
            "submission_clock_session_id":
                "fixture-submission-clock-session",
            "mapping_segment_id": "fixture-common-monotonic-segment",
            "source_event_count": 8,
            "scene_cut_detected": False,
            "left_anchor_roi_bgr_sha256": anchor,
            "left_final_roi_bgr_sha256": anchor,
            "right_anchor_roi_bgr_sha256": anchor,
            "right_final_roi_bgr_sha256": anchor,
        })

    order_manifest = [{
        field: pulse[field]
        for field in (
            "pulse_id", "sequence_index", "block_id", "phase_cell",
            "repeat_index", "row_index", "position_index",
            "polarity_order", "sentinel_position", "pulse_ordinal",
            "command_dx_counts", "command_dy_counts")
    } for pulse in pulses]

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_b_composite_phase_calibration",
        "run_role": "CALIBRATION_DELETION",
        "run_uuid": "12345678-1234-4234-8234-123456789abc",
        "activation_epoch": 123456789,
        "scope_id": "a" * 64,
        "status": "RECORDED_UNANALYZED",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "current_seen_run_reused_as_validation": False,
        "source_run_was_sealed_before_capture": True,
        "seal": {
            "schema_semantic_sha256": schema_semantic_sha256(),
            "evaluator_file_sha256": file_sha256(EVALUATOR_UNDER_TEST),
            "order_manifest_sha256": canonical_sha256(order_manifest),
            "model_semantic_sha256": None,
            "frozen_at_utc_unix_ns": 1_000_000_000,
            "scheduler_clock": {
                "clock_kind": "WINDOWS_QPC",
                "clock_session_id": "fixture-scheduler-qpc",
                "frequency_hz": 10_000_000,
                "producer_process_id": 4242,
            },
            "plan_accepted_at_qpc": 10_000_000,
            "acquisition_started_at_qpc": 20_000_000,
            "revealed_at_qpc": 30_000_000,
            "response_revealed_before_freeze": False,
        },
        "seen_diagnosis_denylist": {
            "run_uuids": [SEEN_RUN_UUID],
            "artifact_sha256s": list(SEEN_ARTIFACT_SHA256),
        },
        "phase_policy": {
            "policy_id": "b-meas-phase-d1-v2",
            "phase_scale": "Q0.32_CYCLE",
            "phase_cells": [
                {"phase_cell": cell_id, "center_numerator": numerator,
                 "center_denominator": 8}
                for cell_id, numerator in PHASE_CELLS
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
        "capture_policy": capture_policy,
        "capture_binding": binding,
        "command_policy": {
            "single_magnitude_counts": 1,
            "zero_y_required": True,
            "net_x_required": 0,
            "fixed_speed_gate_used": False,
        },
        "pulses": pulses,
        "negative_controls": negative_controls,
    }
    evidence["evidence_semantic_sha256"] = canonical_sha256(evidence)
    return evidence


def order_manifest_sha256(evidence: dict[str, Any]) -> str:
    return canonical_sha256([{
        field: pulse[field]
        for field in (
            "pulse_id", "sequence_index", "block_id", "phase_cell",
            "repeat_index", "row_index", "position_index",
            "polarity_order", "sentinel_position", "pulse_ordinal",
            "command_dx_counts", "command_dy_counts")
    } for pulse in evidence["pulses"]])


def reseal_evidence(evidence: dict[str, Any],
                    reseal_order: bool = True) -> None:
    if reseal_order and isinstance(evidence.get("seal"), dict):
        evidence["seal"]["order_manifest_sha256"] = \
            order_manifest_sha256(evidence)
    evidence.pop("evidence_semantic_sha256", None)
    evidence["evidence_semantic_sha256"] = canonical_sha256(evidence)


def reseal_capture_binding(evidence: dict[str, Any]) -> None:
    binding = evidence["capture_binding"]
    policy = evidence["capture_policy"]
    for field in tuple(policy):
        if field != "semantic_sha256" and field in binding:
            policy[field] = copy.deepcopy(binding[field])
    policy.pop("semantic_sha256", None)
    policy["semantic_sha256"] = canonical_sha256(policy)
    binding["capture_policy_semantic_sha256"] = policy["semantic_sha256"]
    binding.pop("semantic_sha256", None)
    binding["semantic_sha256"] = canonical_sha256(binding)
    for pulse in evidence["pulses"]:
        pulse["capture_binding_semantic_sha256"] = \
            binding["semantic_sha256"]
    for control in evidence["negative_controls"]:
        control["capture_binding_semantic_sha256"] = \
            binding["semantic_sha256"]
    reseal_evidence(evidence)


def expect_incomplete(evaluator: pathlib.Path, evidence: dict[str, Any],
                      root: pathlib.Path, stem: str,
                      reason_code: str) -> None:
    completed, output = run_evaluator(evaluator, evidence, root, stem)
    expect(completed.returncode == 2 and output is not None and
           output["status"] == "PHASE_INPUT_INCOMPLETE" and
           output["physical_output_capability"] is False and
           output["physical_dispatch_count"] == 0 and
           output["production_aim_changed"] is False and
           output["new_production_gain_claimed"] is False and
           output["evaluation"]["reason_code"] == reason_code,
           f"{stem} 必须 fail closed 为 {reason_code}: {completed.stderr}")
    assert output is not None
    semantic_input = dict(output)
    semantic = semantic_input.pop("evaluation_semantic_sha256", None)
    expect(semantic == canonical_sha256(semantic_input),
           f"{stem} 的 fail-closed 报告也必须受 semantic SHA-256 绑定")


def run_evaluator(evaluator: pathlib.Path, evidence: dict[str, Any],
                  root: pathlib.Path, stem: str) -> tuple[
                      subprocess.CompletedProcess[str], dict[str, Any] | None]:
    evidence_path = root / f"{stem}-evidence.json"
    output_path = root / f"{stem}-evaluation.json"
    write_json(evidence_path, evidence)
    completed = subprocess.run([
        sys.executable, str(evaluator),
        "--evidence", str(evidence_path.resolve()),
        "--output", str(output_path.resolve()),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    output = (json.loads(output_path.read_text(encoding="utf-8"))
              if output_path.is_file() else None)
    return completed, output


def main() -> int:
    global EVALUATOR_UNDER_TEST
    parser = argparse.ArgumentParser()
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    options = parser.parse_args()
    evaluator = options.evaluator.resolve()
    EVALUATOR_UNDER_TEST = evaluator
    evaluator_source = evaluator.read_text(encoding="utf-8")
    parsed = ast.parse(evaluator_source, filename=str(evaluator))
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
    }), "evaluator 不得 import Mouse/KMBOX/transport 能力")
    help_result = subprocess.run(
        [sys.executable, str(evaluator), "--help"], check=False,
        capture_output=True, text=True, encoding="utf-8")
    help_text = (help_result.stdout + help_result.stderr).lower()
    expect(help_result.returncode == 0 and "--evidence" in help_text and
           "--output" in help_text and "physical" not in help_text and
           "kmbox" not in help_text and "device" not in help_text,
           "公开 CLI 只能暴露 evidence/output，不得接受物理授权或设备参数")

    with tempfile.TemporaryDirectory(
            prefix="xen-composite-phase-evaluator-") as temporary:
        root = pathlib.Path(temporary)
        completed, output = run_evaluator(
            evaluator, build_evidence(), root, "ready")
        expect(completed.returncode == 0 and output is not None,
               f"完整 calibration evidence 应产生报告: {completed.stderr}")
        expect(output["status"] == "READY_FOR_SEALED_PHASE_VALIDATION" and
               output["physical_output_capability"] is False and
               output["physical_dispatch_count"] == 0 and
               output["production_aim_changed"] is False and
               output["new_production_gain_claimed"] is False,
               "合成 phase evidence 最高只能进入 sealed validation")
        expect(output["source"]["timestamp_boundary"] ==
               "NDI_SDK_SUBMISSION_UTC_NOT_EXPOSURE" and
               output["source"]["evaluator_file_sha256"] ==
                   file_sha256(evaluator) and
               output["source"]["schema_semantic_sha256"] ==
                   schema_semantic_sha256(),
               "公开报告必须保留 submission-only 与 schema/evaluator seal")
        evaluation = output["evaluation"]
        expect(evaluation["phase_cell_count"] == 4 and
               evaluation["non_sentinel_pulse_count"] == 32 and
               evaluation["sentinel_pulse_count"] == 6 and
               evaluation["regular_signed_pair_block_count"] == 16 and
               evaluation["position_balance_valid"] is True and
               evaluation["directed_predecessor_balance_valid"] is True and
               evaluation["negative_control_failed"] is False and
               evaluation["fixed_sample_null_deleted"] is True and
               evaluation["global_settled_displacement_interval_q32"] == {
                   "lower_closed": 2_000_000_000,
                   "upper_closed": 2_200_000_000,
               } and
               evaluation["global_completion_interval_ns"] == {
                   "lower_open": 10_500_000,
                   "upper_closed": 14_000_000,
               },
               "公开报告必须给出完整平衡、M- deletion 与全局 M+ interval")
        output_semantic = output.pop("evaluation_semantic_sha256")
        expect(output_semantic == canonical_sha256(output),
               "evaluation semantic SHA-256 必须绑定完整公开报告")
        contract = output["evaluation_contract"]
        contract_semantic = contract.pop("contract_semantic_sha256")
        expect(contract_semantic == canonical_sha256(contract),
               "evaluation contract 必须具有独立 semantic SHA-256")

        sentinel_must_not_fit = build_evidence()
        for witness in sentinel_must_not_fit["pulses"][0][
                "witnesses"].values():
            witness["last_same_submission_after_completed_ns"] = 11_000_000
            witness["first_changed_submission_after_completed_ns"] = \
                15_000_000
        reseal_evidence(sentinel_must_not_fit)
        completed, output = run_evaluator(
            evaluator, sentinel_must_not_fit, root, "sentinel-no-fit")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "READY_FOR_SEALED_PHASE_VALIDATION" and
               output["evaluation"]["sentinel_conjunction_valid"] is True and
               output["evaluation"]["global_completion_interval_ns"] == {
                   "lower_open": 10_500_000,
                   "upper_closed": 14_000_000,
               },
               "sentinel 只能检查 D_cal conjunction，不能参与拟合或缩窄 D_cal")

        sentinel_drift = build_evidence()
        for witness in sentinel_drift["pulses"][0]["witnesses"].values():
            witness["last_same_submission_after_completed_ns"] = 14_100_000
            witness["first_changed_submission_after_completed_ns"] = \
                15_000_000
        reseal_evidence(sentinel_drift)
        completed, output = run_evaluator(
            evaluator, sentinel_drift, root, "sentinel-drift")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "COMPOSITE_PHASE_RESPONSE_DELETED" and
               output["evaluation"]["reason_code"] == "SENTINEL_DRIFT" and
               output["evaluation"]["sentinel_conjunction_valid"] is False,
               "sentinel 与冻结 D_cal 无 conjunction 时必须删除 phase-only 机制")

        phase_dependent_settled = build_evidence()
        settled = next(
            pulse for pulse in phase_dependent_settled["pulses"]
            if pulse["sentinel_position"] is None)["witnesses"]["left"][
                "settled_normalized_displacement_interval_q32"]
        settled["lower_closed"] = 2_300_000_000
        settled["upper_closed"] = 2_400_000_000
        reseal_evidence(phase_dependent_settled)
        completed, output = run_evaluator(
            evaluator, phase_dependent_settled, root,
            "phase-dependent-settled")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "COMPOSITE_PHASE_RESPONSE_DELETED" and
               output["evaluation"]["reason_code"] ==
                   "SETTLED_DISPLACEMENT_PHASE_DEPENDENT" and
               output["evaluation"][
                   "global_settled_displacement_interval_q32"] is None,
               "settled displacement 无全局 conjunction 时必须删除 phase-only 机制")

        no_global_interval = build_evidence()
        witness = next(
            pulse for pulse in no_global_interval["pulses"]
            if pulse["sentinel_position"] is None)["witnesses"]["left"]
        witness["last_same_submission_after_completed_ns"] = 14_250_000
        witness["first_changed_submission_after_completed_ns"] = 15_000_000
        reseal_evidence(no_global_interval)
        completed, output = run_evaluator(
            evaluator, no_global_interval, root, "no-global-interval")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "COMPOSITE_PHASE_RESPONSE_DELETED" and
               output["evaluation"]["fixed_sample_null_deleted"] is True and
               output["evaluation"]["global_completion_interval_ns"] is None,
               "不存在单一 completion-relative interval 时必须删除 M+")

        fixed_sample_onset = build_evidence()
        for pulse in fixed_sample_onset["pulses"]:
            for witness in pulse["witnesses"].values():
                witness["visible_transition_offset_samples"] = 4
        reseal_evidence(fixed_sample_onset)
        completed, output = run_evaluator(
            evaluator, fixed_sample_onset, root, "fixed-sample-onset")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "COMPOSITE_PHASE_RESPONSE_DELETED" and
               output["evaluation"]["fixed_sample_null_deleted"] is False and
               output["evaluation"]["global_completion_interval_ns"] == {
                   "lower_open": 10_500_000,
                   "upper_closed": 14_000_000,
               },
               "M- 未被 phase-cell 反例删除时不得进入 sealed validation")

        semantic_drift = build_evidence()
        semantic_drift["status"] = "MUTATED_AFTER_SEAL"
        expect_incomplete(
            evaluator, semantic_drift, root, "semantic-drift",
            "EVIDENCE_SEMANTIC_MISMATCH")

        evaluator_identity_drift = build_evidence()
        evaluator_identity_drift["seal"]["evaluator_file_sha256"] = "e" * 64
        reseal_evidence(evaluator_identity_drift)
        expect_incomplete(
            evaluator, evaluator_identity_drift, root,
            "evaluator-identity-drift", "EVIDENCE_HEADER_INVALID")

        order_manifest_drift = build_evidence()
        order_manifest_drift["seal"]["order_manifest_sha256"] = "f" * 64
        reseal_evidence(order_manifest_drift, reseal_order=False)
        expect_incomplete(
            evaluator, order_manifest_drift, root,
            "order-manifest-drift", "EVIDENCE_HEADER_INVALID")

        revealed_before_freeze = build_evidence()
        revealed_before_freeze["seal"]["revealed_at_qpc"] = 15_000_000
        reseal_evidence(revealed_before_freeze)
        expect_incomplete(
            evaluator, revealed_before_freeze, root,
            "revealed-before-freeze", "EVIDENCE_HEADER_INVALID")

        phase_outside_target = build_evidence()
        phase = phase_outside_target["pulses"][0][
            "completion_phase_interval_q32"]
        center = Q32 // 8
        tolerance = Q32 // 16
        phase["lower_closed"] = center + tolerance + 1
        phase["upper_closed"] = center + tolerance + 2
        reseal_evidence(phase_outside_target)
        expect_incomplete(
            evaluator, phase_outside_target, root, "phase-outside-target",
            "PHASE_CELL_CONTRACT_INVALID")

        missing_sentinel = build_evidence()
        missing_sentinel["pulses"] = [
            pulse for pulse in missing_sentinel["pulses"]
            if pulse["sentinel_position"] != "end"]
        reseal_evidence(missing_sentinel)
        expect_incomplete(
            evaluator, missing_sentinel, root, "missing-sentinel",
            "SENTINEL_CONTRACT_INVALID")

        capture_hash_drift = build_evidence()
        capture_hash_drift["capture_binding"]["source_name"] = "mutated"
        reseal_evidence(capture_hash_drift)
        expect_incomplete(
            evaluator, capture_hash_drift, root, "capture-hash-drift",
            "CAPTURE_BINDING_INVALID")

        unresolved_capture = build_evidence()
        unresolved_capture["capture_binding"]["capture_stack"][
            "capture_method_resolved"] = False
        reseal_capture_binding(unresolved_capture)
        expect_incomplete(
            evaluator, unresolved_capture, root, "unresolved-capture",
            "CAPTURE_BINDING_INVALID")

        overlapping_witness_rois = build_evidence()
        overlapping_witness_rois["capture_binding"]["right_witness_roi"] = \
            list(overlapping_witness_rois["capture_binding"][
                "left_witness_roi"])
        reseal_capture_binding(overlapping_witness_rois)
        expect_incomplete(
            evaluator, overlapping_witness_rois, root,
            "overlapping-witness-rois", "CAPTURE_BINDING_INVALID")

        exposure_boundary = build_evidence()
        exposure_boundary["capture_binding"]["boundary_semantic"] = \
            "EXPOSURE"
        exposure_boundary["capture_binding"][
            "boundary_is_capture_or_exposure"] = True
        reseal_capture_binding(exposure_boundary)
        expect_incomplete(
            evaluator, exposure_boundary, root, "exposure-boundary",
            "TIMESTAMP_SEMANTIC_INVALID")

        frame_sync_reuse = build_evidence()
        frame_sync_reuse["capture_binding"]["ndi_frame_sync_used"] = True
        reseal_capture_binding(frame_sync_reuse)
        expect_incomplete(
            evaluator, frame_sync_reuse, root, "frame-sync-reuse",
            "CAPTURE_BINDING_INVALID")

        stale_clock_mapping = build_evidence()
        stale_clock_mapping["capture_binding"]["clock_mapping_stale"] = True
        reseal_capture_binding(stale_clock_mapping)
        expect_incomplete(
            evaluator, stale_clock_mapping, root, "stale-clock-mapping",
            "CLOCK_MAPPING_STALE")

        mixed_clock_session = build_evidence()
        mixed_clock_session["pulses"][0]["witnesses"]["left"][
            "submission_clock_session_id"] = "different-session"
        reseal_evidence(mixed_clock_session)
        expect_incomplete(
            evaluator, mixed_clock_session, root, "mixed-clock-session",
            "CLOCK_MAPPING_INCOMPLETE")

        output_capability = build_evidence()
        output_capability["physical_dispatch_count"] = 1
        reseal_evidence(output_capability)
        expect_incomplete(
            evaluator, output_capability, root, "output-capability",
            "OUTPUT_OFF_CONTRACT_INVALID")

        per_pulse_tuning = build_evidence()
        per_pulse_tuning["pulses"][0]["per_pulse_shift_samples"] = 0
        reseal_evidence(per_pulse_tuning)
        expect_incomplete(
            evaluator, per_pulse_tuning, root, "per-pulse-tuning",
            "PER_PULSE_TUNING_FORBIDDEN")

        sign_imbalance = build_evidence()
        sign_imbalance["pulses"][1]["command_dx_counts"] = 1
        reseal_evidence(sign_imbalance)
        expect_incomplete(
            evaluator, sign_imbalance, root, "sign-imbalance",
            "BLOCK_BALANCE_INVALID")

        output_feedback = build_evidence()
        output_feedback["model_policy"]["output_feedback_used"] = True
        reseal_evidence(output_feedback)
        expect_incomplete(
            evaluator, output_feedback, root, "output-feedback",
            "MODEL_POLICY_INVALID")

        validation_refit = build_evidence()
        validation_refit["model_policy"][
            "confirmation_or_validation_refit_allowed"] = True
        reseal_evidence(validation_refit)
        expect_incomplete(
            evaluator, validation_refit, root, "validation-refit",
            "MODEL_POLICY_INVALID")

        seen_run_reuse = build_evidence()
        seen_run_reuse["current_seen_run_reused_as_validation"] = True
        reseal_evidence(seen_run_reuse)
        expect_incomplete(
            evaluator, seen_run_reuse, root, "seen-run-reuse",
            "EVIDENCE_HEADER_INVALID")

        seen_run_as_calibration = build_evidence()
        seen_run_as_calibration["run_uuid"] = SEEN_RUN_UUID
        reseal_evidence(seen_run_as_calibration)
        expect_incomplete(
            evaluator, seen_run_as_calibration, root,
            "seen-run-as-calibration", "EVIDENCE_HEADER_INVALID")

        repeat_imbalance = build_evidence()
        for pulse in repeat_imbalance["pulses"]:
            if pulse["block_id"] == "R2-0-P5_8":
                pulse["repeat_index"] = 9
        reseal_evidence(repeat_imbalance)
        expect_incomplete(
            evaluator, repeat_imbalance, root, "repeat-imbalance",
            "BLOCK_BALANCE_INVALID")

        incomplete_williams = build_evidence()
        incomplete_williams["pulses"] = [
            pulse for pulse in incomplete_williams["pulses"]
            if pulse["row_index"] not in (2, 3)]
        for sequence_index, pulse in enumerate(incomplete_williams["pulses"]):
            pulse["sequence_index"] = sequence_index
        reseal_evidence(incomplete_williams)
        expect_incomplete(
            evaluator, incomplete_williams, root, "incomplete-williams",
            "BLOCK_BALANCE_INVALID")

        invalid_witness = build_evidence()
        bad_witness = invalid_witness["pulses"][0]["witnesses"]["left"]
        bad_witness["first_changed_roi_bgr_sha256"] = \
            bad_witness["anchor_roi_bgr_sha256"]
        reseal_evidence(invalid_witness)
        expect_incomplete(
            evaluator, invalid_witness, root, "invalid-witness",
            "MEASUREMENT_WITNESS_INVALID")

        no_visible_transition = build_evidence()
        no_visible_transition["pulses"][0]["witnesses"]["left"][
            "visible_transition_found"] = False
        reseal_evidence(no_visible_transition)
        expect_incomplete(
            evaluator, no_visible_transition, root,
            "no-visible-transition", "NO_VISIBLE_TRANSITION")

        scene_cut = build_evidence()
        scene_cut["pulses"][0]["scene_cut_or_occlusion_detected"] = True
        reseal_evidence(scene_cut)
        expect_incomplete(
            evaluator, scene_cut, root, "scene-cut",
            "SCENE_OR_WITNESS_INVALID")

        nonzero_y = build_evidence()
        nonzero_y["pulses"][0]["command_dy_counts"] = 1
        reseal_evidence(nonzero_y)
        expect_incomplete(
            evaluator, nonzero_y, root, "nonzero-y",
            "COMMAND_CONTRACT_INVALID")

        changed_without_command = build_evidence()
        changed_without_command["negative_controls"][0][
            "left_final_roi_bgr_sha256"] = "d" * 64
        reseal_evidence(changed_without_command)
        completed, output = run_evaluator(
            evaluator, changed_without_command, root,
            "changed-without-command")
        expect(completed.returncode == 0 and output is not None and
               output["status"] == "COMPOSITE_PHASE_RESPONSE_DELETED" and
               output["evaluation"]["negative_control_failed"] is True and
               output["evaluation"]["reason_code"] ==
                   "NEGATIVE_CONTROL_FAILED",
               "no-command control 出现同构变化时必须删除 phase-only 机制")

        collision_evidence = build_evidence()
        first, first_output = run_evaluator(
            evaluator, collision_evidence, root, "output-collision")
        second, second_output = run_evaluator(
            evaluator, collision_evidence, root, "output-collision")
        expect(first.returncode == 0 and first_output is not None and
               second.returncode == 3 and second_output == first_output,
               "既有 evaluation 不得被覆盖，失败后必须保留原子发布结果")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
