#!/usr/bin/env python3
"""归因 Physical B 多幅值残差的完成时刻相位；不具备 Mouse 或 Launch 能力。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys
from typing import Any


AMPLITUDES = (1, 1, 4, 4, 13, 13, 2, 2, 8, 8)
ROLES = ("estimation",) * 6 + ("confirmation",) * 4
POLARITIES = ("normal", "inverted") * 5
RESPONSE_ROWS = 49
WITNESSES = ("left", "right")


def _canonical_sha256(value: dict[str, Any]) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


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


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    pending = path.with_name(f".{path.name}.pending-{os.getpid()}")
    if not path.is_absolute() or path.exists() or pending.exists():
        raise ValueError("归因输出必须是尚不存在的绝对路径")
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


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def _exact_int(value: Any) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError("需要精确 JSON integer")
    return value


def _finite_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or \
            not math.isfinite(float(value)):
        raise ValueError(f"{context} 必须是有限数值")
    return float(value)


def _finite_curve(value: Any, context: str) -> list[float]:
    if not isinstance(value, list) or len(value) != RESPONSE_ROWS:
        raise ValueError(f"{context} 曲线长度无效")
    return [_finite_number(item, context) for item in value]


def _strictly_less(first: float, second: float) -> bool:
    return bool(first < math.nextafter(second, -math.inf))


def _metric_max(observed: list[float], predicted: list[float]) -> float:
    return max(abs(actual - expected)
               for actual, expected in zip(observed, predicted))


def analysis_contract() -> dict[str, Any]:
    contract: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_completion_phase_analysis_contract",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "question": {
            "hypothesis":
                "fixed_source_sample_offset_is_unique_plant_attribution_axis",
            "deletion_unit":
                "replicated_exact_signed_command_across_normal_inverted_blocks",
            "visible_transition":
                "first_response_frame_whose_exact_bgr_sha256_differs_from_anchor",
            "stable_transition_required": True,
            "source_time_basis": "NDI_SDK_SUBMISSION",
            "source_time_is_exposure": False,
            "completion_endpoint": "backend_completed_at_steady_ns",
            "completion_relative_interval":
                "open_last_anchor_equal_submission_to_closed_first_changed_submission",
            "interpolation_or_per_pulse_shift_allowed": False,
            "numeric_tolerance_or_speed_gate_used": False,
        },
        "deletion_test": {
            "exact_signed_command_pair_count": 10,
            "requires_at_least_one_source_sample_onset_counterexample": True,
            "requires_every_pair_completion_relative_bracket_overlap": True,
            "requires_every_failed_model_max_argmax_before_transition": True,
            "deleted_status": "SOURCE_SAMPLE_ALIGNMENT_CONFOUNDED",
            "unresolved_status": "TIMING_ATTRIBUTION_UNRESOLVED",
        },
        "epistemic_boundary": {
            "current_run_used_for_model_selection": True,
            "current_run_is_validation_evidence": False,
            "plant_nonlinearity_claimed": False,
            "unique_delay_or_measurement_model_claimed": False,
            "future_sealed_run_required_for_any_frozen_model": True,
        },
        "failure_semantics": {
            "input_identity_drift_allowed": False,
            "missing_or_duplicate_event_or_frame_allowed": False,
            "mixed_source_clock_session_allowed": False,
            "non_submission_timestamp_semantic_allowed": False,
            "physical_launch_or_dispatch_available": False,
        },
    }
    contract["contract_semantic_sha256"] = _canonical_sha256(contract)
    return contract


def _validate_analysis_header(analysis: dict[str, Any]) -> tuple[
        dict[str, Any], dict[str, Any], list[dict[str, Any]]]:
    semantic = analysis.get("analysis_semantic_sha256")
    semantic_input = dict(analysis)
    semantic_input.pop("analysis_semantic_sha256", None)
    if analysis.get("schema_version") != 1 or \
            analysis.get("evidence_type") != \
                "mouse_effect_probe_b_command_magnitude_primary_analysis" or \
            analysis.get("status") != "LINEAR_STEP_RESPONSE_DELETED" or \
            analysis.get("physical_output_capability") is not False or \
            analysis.get("physical_dispatch_count") != 0 or \
            analysis.get("production_aim_changed") is not False or \
            analysis.get("new_production_gain_claimed") is not False or \
            not _is_sha256(semantic) or \
            semantic != _canonical_sha256(semantic_input):
        raise ValueError("command-magnitude analysis header/semantic 无效")
    contract = analysis.get("analysis_contract")
    measurement = contract.get("measurement", {}) \
        if isinstance(contract, dict) else {}
    model = contract.get("model", {}) if isinstance(contract, dict) else {}
    deletion = contract.get("deletion_tests", {}) \
        if isinstance(contract, dict) else {}
    if not isinstance(contract, dict) or \
            contract.get("physical_output_capability") is not False or \
            contract.get("physical_dispatch_count") != 0 or \
            contract.get("production_aim_changed") is not False or \
            measurement.get("pulse_anchor") != \
                "exact_event_frame_immediately_before_pulse" or \
            measurement.get("source_join") != \
                "exact_int64_ndi_submission_timestamp" or \
            measurement.get("timestamp_semantic") != \
                "NDI_SDK_SUBMISSION_NOT_EXPOSURE" or \
            measurement.get("response_rows_including_pulse") != RESPONSE_ROWS or \
            model.get("input") != \
                "backend_completed_relative_command_dx_counts" or \
            model.get("fit") != \
                "least_squares_through_origin_at_each_response_offset" or \
            model.get("fit_amplitudes") != [1, 4, 13] or \
            model.get("within_run_confirmation_amplitudes") != [2, 8] or \
            model.get("confirmation_used_for_refit") is not False or \
            deletion.get("failure_status") != "LINEAR_STEP_RESPONSE_DELETED":
        raise ValueError("command-magnitude analysis contract 无效")
    primary = analysis.get("primary")
    evaluation = analysis.get("evaluation")
    pulses = analysis.get("pulse_measurements")
    if not isinstance(primary, dict) or not isinstance(evaluation, dict) or \
            not isinstance(pulses, list) or len(pulses) != 20 or \
            evaluation.get("status") != "LINEAR_STEP_RESPONSE_DELETED" or \
            evaluation.get("physical_output_capability") is not False or \
            evaluation.get("physical_dispatch_count") != 0 or \
            evaluation.get("production_aim_changed") is not False or \
            evaluation.get("new_production_gain_claimed") is not False or \
            evaluation.get("all_confirmation_whole_pulses_pass") is not False or \
            evaluation.get("cross_run_holdout_required_before_candidate") is \
                not True or \
            primary.get("physical_output_was_historical_input") is not True or \
            primary.get("physical_output_dispatched_by_this_analysis") is \
                not False or \
            not _is_sha256(primary.get("command_report_sha256")) or \
            not _is_sha256(primary.get("manifest_sha256")):
        raise ValueError("command-magnitude analysis deletion/identity 无效")
    return primary, evaluation, pulses


def _validate_frames(manifest: dict[str, Any], session: str) -> list[dict[str, Any]]:
    frames = manifest.get("frames")
    if manifest.get("evidence_type") != "output_off_capture" or \
            manifest.get("physical_output_capability") is not False or \
            not isinstance(frames, list) or not frames or \
            manifest.get("recorded_frame_count") != len(frames):
        raise ValueError("manifest header/frame count 无效")
    source_timestamps: set[int] = set()
    for expected_index, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise ValueError("manifest frame 必须是 object")
        timestamp = _exact_int(frame.get("source_timestamp"))
        _exact_int(frame.get("source_time_at_steady_ns"))
        uncertainty = _finite_number(
            frame.get("source_clock_uncertainty_ms"),
            "frame source clock uncertainty")
        if frame.get("index") != expected_index or timestamp <= 0 or \
                timestamp in source_timestamps or \
                frame.get("source_timestamp_valid") is not True or \
                frame.get("source_time_basis") != "NDI_SDK_SUBMISSION" or \
                frame.get("source_time_timing_valid") is not True or \
                frame.get("source_clock_status") != "VALID" or \
                str(frame.get("source_clock_session_id", "")) != session or \
                uncertainty < 0.0 or not _is_sha256(frame.get("bgr_sha256")):
            raise ValueError("manifest frame index/source/session/BGR 合同无效")
        source_timestamps.add(timestamp)
    return frames


def _validate_events(report: dict[str, Any], run_uuid: str,
                     session: str) -> dict[int, dict[str, Any]]:
    result = report.get("result")
    if report.get("run_uuid") != run_uuid or \
            report.get("dispatch_mode") != "physical_b" or \
            report.get("profile") != "physical_b_command_magnitude_primary" or \
            not isinstance(result, dict) or result.get("complete") is not True or \
            result.get("stop_reason") != "normal_completion" or \
            not isinstance(result.get("events"), list):
        raise ValueError("command report header/result 无效")
    nonzero = [event for event in result["events"]
               if isinstance(event, dict) and
               _exact_int(event.get("requested_dx_counts")) != 0]
    if len(nonzero) != 20:
        raise ValueError("command report 必须包含 20 个非零完成事件")
    by_timestamp: dict[int, dict[str, Any]] = {}
    for event in nonzero:
        timestamp = _exact_int(event.get("source_timestamp"))
        source_ns = _exact_int(event.get("source_time_at_steady_ns"))
        scheduled_ns = _exact_int(event.get("scheduled_at_steady_ns"))
        issued_ns = _exact_int(event.get("issued_at_steady_ns"))
        ack_ns = _exact_int(event.get(
            "protocol_ack_received_at_steady_ns"))
        completed_ns = _exact_int(event.get(
            "backend_completed_at_steady_ns"))
        returned_ns = _exact_int(event.get("returned_at_steady_ns"))
        requested = _exact_int(event.get("requested_dx_counts"))
        uncertainty = _finite_number(
            event.get("source_clock_uncertainty_ms"),
            "event source clock uncertainty")
        if timestamp <= 0 or timestamp in by_timestamp or source_ns <= 0 or \
                not (source_ns <= scheduled_ns <= issued_ns <= ack_ns <=
                     completed_ns <= returned_ns) or \
                event.get("source_timestamp_valid") is not True or \
                event.get("source_time_basis") != "NDI_SDK_SUBMISSION" or \
                event.get("source_clock_status") != "VALID" or \
                str(event.get("source_clock_session_id", "")) != session or \
                uncertainty < 0.0 or \
                _exact_int(event.get("nominal_dx_counts")) != requested or \
                _exact_int(event.get("nominal_dy_counts")) != 0 or \
                _exact_int(event.get("requested_dy_counts")) != 0 or \
                event.get("safety_allowed") is not True or \
                event.get("dispatch_attempted") is not True or \
                event.get("backend_succeeded") is not True or \
                event.get("protocol_ack_received") is not True:
            raise ValueError("nonzero event 完成时刻/source/X-only 合同无效")
        by_timestamp[timestamp] = event
    return by_timestamp


def _pulse_identity(pulse: dict[str, Any], index: int) -> tuple[int, ...]:
    block_index = index // 2
    pulse_ordinal = index % 2 + 1
    amplitude = AMPLITUDES[block_index]
    sign = 1 if POLARITIES[block_index] == "normal" else -1
    command = sign * amplitude if pulse_ordinal == 1 else -sign * amplitude
    expected = {
        "block_id": block_index + 1,
        "pair_index": block_index // 2 + 1,
        "role": ROLES[block_index],
        "polarity": POLARITIES[block_index],
        "amplitude_counts": amplitude,
        "pulse_ordinal": pulse_ordinal,
        "command_dx_counts": command,
    }
    if not isinstance(pulse, dict) or any(
            pulse.get(key) != value for key, value in expected.items()):
        raise ValueError("pulse identity/order/split 无效")
    return (expected["block_id"], expected["pair_index"],
            expected["amplitude_counts"], expected["pulse_ordinal"],
            expected["command_dx_counts"])


def _transition_brackets(pulses: list[dict[str, Any]],
                         events: dict[int, dict[str, Any]],
                         frames: list[dict[str, Any]]) -> list[dict[str, Any]]:
    brackets: list[dict[str, Any]] = []
    used_events: set[int] = set()
    for index, pulse in enumerate(pulses):
        block_id, pair_index, amplitude, pulse_ordinal, command = \
            _pulse_identity(pulse, index)
        source_timestamp = _exact_int(pulse.get("source_timestamp"))
        event = events.get(source_timestamp)
        if event is None or id(event) in used_events or \
                _exact_int(event.get("block_id")) != block_id or \
                _exact_int(event.get("requested_dx_counts")) != command:
            raise ValueError("pulse 与唯一 nonzero completion event 无法闭合")
        used_events.add(id(event))
        first = _exact_int(pulse.get("manifest_first_index"))
        last = _exact_int(pulse.get("manifest_last_index"))
        if first <= 0 or last != first + RESPONSE_ROWS - 1 or \
                last >= len(frames) or \
                _exact_int(frames[first].get("source_timestamp")) != \
                    source_timestamp:
            raise ValueError("pulse manifest response window 无效")
        anchor_hash = frames[first - 1].get("bgr_sha256")
        response_hashes = [frames[position].get("bgr_sha256")
                           for position in range(first, last + 1)]
        onset = next((offset for offset, value in enumerate(response_hashes)
                      if value != anchor_hash), None)
        if onset is None or onset <= 0 or any(
                value == anchor_hash for value in response_hashes[onset:]):
            raise ValueError("pulse exact BGR transition 不存在或发生回锚")
        last_same_frame = frames[first + onset - 1]
        first_changed_frame = frames[first + onset]
        last_same_ns = _exact_int(
            last_same_frame.get("source_time_at_steady_ns"))
        first_changed_ns = _exact_int(
            first_changed_frame.get("source_time_at_steady_ns"))
        completed_ns = _exact_int(event.get(
            "backend_completed_at_steady_ns"))
        if not completed_ns < last_same_ns < first_changed_ns:
            raise ValueError("completion 后 exact BGR transition bracket 无效")
        brackets.append({
            "block_id": block_id,
            "pair_index": pair_index,
            "role": pulse["role"],
            "polarity": pulse["polarity"],
            "amplitude_counts": amplitude,
            "pulse_ordinal": pulse_ordinal,
            "command_dx_counts": command,
            "source_timestamp": source_timestamp,
            "sample_index": _exact_int(event.get("sample_index")),
            "manifest_first_index": first,
            "visible_transition_offset_samples": onset,
            "source_event_to_scheduled_ns":
                _exact_int(event["scheduled_at_steady_ns"]) -
                _exact_int(event["source_time_at_steady_ns"]),
            "source_event_to_backend_completed_ns": completed_ns -
                _exact_int(event["source_time_at_steady_ns"]),
            "issued_to_backend_completed_ns": completed_ns -
                _exact_int(event["issued_at_steady_ns"]),
            "transition_after_backend_completed_ns": {
                "lower_open": last_same_ns - completed_ns,
                "upper_closed": first_changed_ns - completed_ns,
            },
            "source_clock_uncertainty_ms": {
                "event": _finite_number(
                    event["source_clock_uncertainty_ms"], "event uncertainty"),
                "last_same_frame": _finite_number(
                    last_same_frame["source_clock_uncertainty_ms"],
                    "last-same uncertainty"),
                "first_changed_frame": _finite_number(
                    first_changed_frame["source_clock_uncertainty_ms"],
                    "first-changed uncertainty"),
            },
        })
    if len(used_events) != len(events):
        raise ValueError("存在未被 pulse 消费的 nonzero completion event")
    return brackets


def _exact_command_pairs(brackets: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for bracket in brackets:
        key = (int(bracket["amplitude_counts"]),
               int(bracket["command_dx_counts"]))
        grouped.setdefault(key, []).append(bracket)
    if len(grouped) != 10 or any(len(rows) != 2
                                 for rows in grouped.values()):
        raise ValueError("exact signed-command replicate 分组无效")
    pairs: list[dict[str, Any]] = []
    for amplitude, command in sorted(grouped,
                                     key=lambda item: (item[0], item[1])):
        rows = grouped[(amplitude, command)]
        if {row["polarity"] for row in rows} != {"normal", "inverted"}:
            raise ValueError("exact command pair 必须跨 normal/inverted block")
        first_interval = rows[0]["transition_after_backend_completed_ns"]
        second_interval = rows[1]["transition_after_backend_completed_ns"]
        overlap_lower = max(int(first_interval["lower_open"]),
                            int(second_interval["lower_open"]))
        overlap_upper = min(int(first_interval["upper_closed"]),
                            int(second_interval["upper_closed"]))
        pairs.append({
            "amplitude_counts": amplitude,
            "command_dx_counts": command,
            "replicates": [{
                "block_id": row["block_id"],
                "pulse_ordinal": row["pulse_ordinal"],
                "polarity": row["polarity"],
                "visible_transition_offset_samples":
                    row["visible_transition_offset_samples"],
                "source_event_to_backend_completed_ns":
                    row["source_event_to_backend_completed_ns"],
                "transition_after_backend_completed_ns":
                    row["transition_after_backend_completed_ns"],
            } for row in rows],
            "source_sample_onset_differs":
                rows[0]["visible_transition_offset_samples"] !=
                rows[1]["visible_transition_offset_samples"],
            "completion_relative_brackets_overlap":
                overlap_lower < overlap_upper,
            "completion_relative_overlap_ns": {
                "lower_open": overlap_lower,
                "upper_closed": overlap_upper,
            } if overlap_lower < overlap_upper else None,
        })
    return pairs


def _confirmation_residual_locations(
        pulses: list[dict[str, Any]], evaluation: dict[str, Any],
        brackets: list[dict[str, Any]]) -> list[dict[str, Any]]:
    fit = evaluation.get("fit")
    frozen_f1 = evaluation.get("frozen_f1")
    confirmation = evaluation.get("confirmation")
    if not isinstance(fit, dict) or not isinstance(frozen_f1, dict) or \
            not isinstance(confirmation, list) or len(confirmation) != 8 or \
            fit.get("fit_role") != "estimation_whole_pulses_only" or \
            fit.get("confirmation_used_for_refit") is not False or \
            frozen_f1.get("delay_samples") != 4 or \
            not isinstance(frozen_f1.get("gains"), dict):
        raise ValueError("fit/F1/confirmation 合同无效")
    fit_curves = {
        witness: _finite_curve(
            fit.get(f"{witness}_step_response_px_per_count"),
            f"{witness} fit")
        for witness in WITNESSES
    }
    f1_gains = {
        witness: _finite_number(
            frozen_f1["gains"].get(witness), f"{witness} F1 gain")
        for witness in WITNESSES
    }
    pulse_by_identity = {
        (pulse["block_id"], pulse["pulse_ordinal"]): pulse
        for pulse in pulses if pulse.get("role") == "confirmation"
    }
    bracket_by_identity = {
        (row["block_id"], row["pulse_ordinal"]): row
        for row in brackets if row["role"] == "confirmation"
    }
    if len(pulse_by_identity) != 8 or len(bracket_by_identity) != 8:
        raise ValueError("confirmation pulse/bracket split 无效")
    locations: list[dict[str, Any]] = []
    for stored in confirmation:
        if not isinstance(stored, dict):
            raise ValueError("confirmation row 必须是 object")
        identity = (stored.get("block_id"), stored.get("pulse_ordinal"))
        pulse = pulse_by_identity.get(identity)
        bracket = bracket_by_identity.get(identity)
        if pulse is None or bracket is None or any(
                stored.get(key) != pulse.get(key) for key in (
                    "pair_index", "role", "polarity", "amplitude_counts",
                    "command_dx_counts")):
            raise ValueError("confirmation row/pulse identity 无效")
        command = _exact_int(pulse.get("command_dx_counts"))
        stored_witnesses = stored.get("witnesses")
        if not isinstance(stored_witnesses, dict):
            raise ValueError("confirmation witness map 无效")
        for witness in WITNESSES:
            observed = _finite_curve(
                pulse.get(f"{witness}_dx_px"), f"{witness} observed")
            predicted = [value * command for value in fit_curves[witness]]
            f1_prediction = [
                0.0 if offset < 4 else f1_gains[witness] * command
                for offset in range(RESPONSE_ROWS)
            ]
            model_residuals = [abs(actual - expected)
                               for actual, expected in zip(observed, predicted)]
            model_max = max(model_residuals)
            model_argmax = model_residuals.index(model_max)
            f1_max = _metric_max(observed, f1_prediction)
            stored_witness = stored_witnesses.get(witness)
            if not isinstance(stored_witness, dict):
                raise ValueError("confirmation witness row 无效")
            stored_model_max = _finite_number(
                stored_witness.get("model", {}).get("max_abs_error_px"),
                "stored model max")
            stored_f1_max = _finite_number(
                stored_witness.get("frozen_f1", {}).get("max_abs_error_px"),
                "stored F1 max")
            beats = _strictly_less(model_max, f1_max)
            if not math.isclose(model_max, stored_model_max,
                                rel_tol=0.0, abs_tol=1e-12) or \
                    not math.isclose(f1_max, stored_f1_max,
                                     rel_tol=0.0, abs_tol=1e-12) or \
                    stored_witness.get("model_strictly_beats_f1_max") is \
                        not beats:
                raise ValueError("confirmation max metric 与 pulse/fit 不闭合")
            locations.append({
                "block_id": identity[0],
                "pulse_ordinal": identity[1],
                "amplitude_counts": pulse["amplitude_counts"],
                "command_dx_counts": command,
                "witness": witness,
                "model_strictly_beats_f1_max": beats,
                "model_max_abs_error_px": model_max,
                "frozen_f1_max_abs_error_px": f1_max,
                "model_max_argmax_offset_samples": model_argmax,
                "visible_transition_offset_samples":
                    bracket["visible_transition_offset_samples"],
                "model_max_argmax_precedes_visible_transition":
                    model_argmax <
                    bracket["visible_transition_offset_samples"],
            })
    return locations


def evaluate_completion_phase(pulses: list[dict[str, Any]],
                              evaluation: dict[str, Any],
                              events: dict[int, dict[str, Any]],
                              frames: list[dict[str, Any]]) -> dict[str, Any]:
    brackets = _transition_brackets(pulses, events, frames)
    pairs = _exact_command_pairs(brackets)
    residuals = _confirmation_residual_locations(
        pulses, evaluation, brackets)
    failed = [row for row in residuals
              if not row["model_strictly_beats_f1_max"]]
    onset_counterexamples = [row for row in pairs
                             if row["source_sample_onset_differs"]]
    all_pairs_overlap = all(
        row["completion_relative_brackets_overlap"] for row in pairs)
    all_failed_pretransition = bool(failed) and all(
        row["model_max_argmax_precedes_visible_transition"] for row in failed)
    confounded = bool(onset_counterexamples) and all_pairs_overlap and \
        all_failed_pretransition
    return {
        "status": ("SOURCE_SAMPLE_ALIGNMENT_CONFOUNDED" if confounded else
                   "TIMING_ATTRIBUTION_UNRESOLVED"),
        "fixed_source_sample_offset_unique_attribution_deleted": confounded,
        "timing_or_measurement_mechanism_identified": False,
        "completion_time_model_validated": False,
        "pulse_transition_brackets": brackets,
        "exact_command_pairs": pairs,
        "source_sample_onset_counterexample_count":
            len(onset_counterexamples),
        "every_exact_command_pair_completion_bracket_overlaps":
            all_pairs_overlap,
        "confirmation_model_max_comparisons": residuals,
        "failed_model_max_comparison_count": len(failed),
        "all_failed_model_max_comparisons_precede_transition":
            all_failed_pretransition,
    }


def _load_bound_inputs(root: pathlib.Path, analysis_path: pathlib.Path) -> tuple[
        dict[str, Any], dict[str, Any], list[dict[str, Any]],
        dict[int, dict[str, Any]], list[dict[str, Any]], dict[str, Any]]:
    if not root.is_absolute() or not root.is_dir() or \
            not analysis_path.is_absolute() or not analysis_path.is_file():
        raise ValueError("Run 与 command-magnitude analysis 必须是已存在绝对路径")
    analysis = _load_json(analysis_path, "command-magnitude analysis")
    primary, evaluation, pulses = _validate_analysis_header(analysis)
    report_path = root / "command-report.json"
    manifest_path = root / "pixel-evidence" / "manifest.json"
    if not report_path.is_file() or not manifest_path.is_file() or \
            _file_sha256(report_path) != primary["command_report_sha256"] or \
            _file_sha256(manifest_path) != primary["manifest_sha256"]:
        raise ValueError("command report/manifest file identity 漂移")
    report = _load_json(report_path, "command report")
    manifest = _load_json(manifest_path, "manifest")
    run_uuid = str(primary.get("run_uuid", ""))
    session = str(primary.get("source_clock_session_id", ""))
    if not run_uuid or not session:
        raise ValueError("Primary run/session identity 缺失")
    frames = _validate_frames(manifest, session)
    events = _validate_events(report, run_uuid, session)
    identity = {
        "run_uuid": run_uuid,
        "source_clock_session_id": session,
        "command_magnitude_analysis_path": str(analysis_path),
        "command_magnitude_analysis_sha256": _file_sha256(analysis_path),
        "command_magnitude_analysis_semantic_sha256":
            analysis["analysis_semantic_sha256"],
        "command_report_sha256": primary["command_report_sha256"],
        "manifest_sha256": primary["manifest_sha256"],
        "completion_phase_analyzer_sha256": _file_sha256(
            pathlib.Path(__file__).resolve()),
        "historical_physical_output_consumed_read_only": True,
        "physical_output_dispatched_by_this_analysis": False,
    }
    return analysis, evaluation, pulses, events, frames, identity


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="归因 Physical B command-magnitude 完成时刻相位")
    parser.add_argument("--run-directory", required=True, type=pathlib.Path)
    parser.add_argument(
        "--command-magnitude-analysis", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    options = parser.parse_args(arguments)
    try:
        _, evaluation, pulses, events, frames, identity = _load_bound_inputs(
            options.run_directory.resolve(),
            options.command_magnitude_analysis.resolve())
        result = evaluate_completion_phase(
            pulses, evaluation, events, frames)
        output: dict[str, Any] = {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_b_completion_phase_attribution",
            "status": result["status"],
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "production_aim_changed": False,
            "new_production_gain_claimed": False,
            "used_for_model_selection": True,
            "validation_evidence": False,
            "plant_nonlinearity_claimed": False,
            "unique_delay_or_measurement_model_claimed": False,
            "analysis_contract": analysis_contract(),
            "input_identity": identity,
            "evaluation": result,
        }
        output["analysis_semantic_sha256"] = _canonical_sha256(output)
        _write_json_atomic(options.output, output)
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
        print(f"completion-phase analysis failed: {error}", file=sys.stderr)
        return 2
    print(
        "completion-phase analysis: "
        f"status={result['status']}, physical_output_capability=false, "
        "physical_dispatch_count=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
