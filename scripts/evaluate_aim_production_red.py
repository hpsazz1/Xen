#!/usr/bin/env python3
"""对 hash-bound Aim production red trace 做 output-off 离线裁决。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
from typing import Any


RED_SCHEMA_VERSION = 1
EVIDENCE_TYPE = "aim_production_red_manifest"

_SOURCE_FIELDS = (
    "source_sequence",
    "source_timestamp",
    "source_clock_session_id",
)
_COMPLETED_FIELDS = (
    "backend_completed_dx",
    "backend_completed_dy",
    "backend_completed_at_ns",
    "completion_status",
    "completion_zero_reason",
    "protocol_acknowledged",
    "aim_actual_history_dx",
    "aim_actual_history_dy",
    "plant_input_dx",
    "plant_input_dy",
)
_REQUIRED_ROW_FIELDS = (
    "red_schema",
    "asset_id",
    "source_relative_path",
    "source_sha256",
    "extractor",
    "configuration_sha256",
    "block_id",
    "role",
    "reset",
    "sample_index_in_block",
    "score_begin",
    "score_end",
    "plant_profile_id",
    *_SOURCE_FIELDS,
    "captured_at_ns",
    "control_at_ns",
    "controller_dt_ns",
    "observation_age_ns",
    *_COMPLETED_FIELDS,
    "world_delta_x",
    "world_delta_y",
    "box_width",
    "box_height",
    "pose",
    "visible",
    "target_id",
    "aim_status",
    "matched_observation_box",
    "base_point",
    "delay_compensated_point",
    "prediction_point",
    "control_center",
    "controller_x",
    "issued_dx",
    "issued_dy",
    "plant_due_queue",
    "plant_prefix",
    "camera_visible_effect",
    "observed_box",
    "quantization_zero_x",
    "quantization_zero_y",
    "limit_signature",
    "lock_active",
    "backend_failure",
    "physical_dispatch_count",
)
_INTEGER_METRICS = (
    "outside_samples",
    "outside_duration_ns",
    "longest_outside_samples",
    "longest_outside_ns",
)
_FLOAT_METRICS = (
    "outside_area_px_ns",
    "max_excess_x_px",
    "max_abs_error_x_px",
)
_REQUIRED_TRACE_DESCRIPTOR_FIELDS = (
    "trace_id",
    "block_id",
    "role",
    "variant",
    "plant_profile_id",
    "relative_path",
    "sha256",
    "sample_count",
    "score_begin",
    "score_end",
)
_TAIL_NON_REGRESSION_METRICS = (
    "outside_samples",
    "outside_duration_ns",
    "longest_outside_samples",
    "longest_outside_ns",
    "max_excess_x_px",
    "max_abs_error_x_px",
)
_PAIRED_INPUT_FIELDS = (
    "source_relative_path",
    "source_sha256",
    "extractor",
    "configuration_sha256",
    "sample_index_in_block",
    "score_begin",
    "score_end",
    "source_sequence",
    "source_timestamp",
    "source_clock_session_id",
    "captured_at_ns",
    "control_at_ns",
    "controller_dt_ns",
    "observation_age_ns",
    "backend_completed_at_ns",
    "world_delta_x",
    "world_delta_y",
    "box_width",
    "box_height",
    "pose",
    "visible",
    "target_id",
    "backend_failure",
    "completion_status",
    "protocol_acknowledged",
    "lock_active",
    "physical_dispatch_count",
)


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_sha256(value: Any) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def red_schema_contract() -> dict[str, Any]:
    """返回 evaluator 实际执行的 v1 schema，供生成器冻结并复算身份。"""

    contract: dict[str, Any] = {
        "red_schema": RED_SCHEMA_VERSION,
        "input_evidence_type": EVIDENCE_TYPE,
        "output_evidence_type": "aim_production_red_evaluation",
        "required_manifest_fields": [
            "red_schema", "evidence_type", "asset_id", "generator",
            "configuration_sha256", "physical_output_capability",
            "physical_dispatch_count", "candidate_uses_f1",
            "production_plant_profile_id", "mandatory_block_profiles",
            "traces",
        ],
        "required_trace_descriptor_fields": list(
            _REQUIRED_TRACE_DESCRIPTOR_FIELDS),
        "required_trace_row_fields": list(_REQUIRED_ROW_FIELDS),
        "variants": ["B0", "C", "C-F1-OFF", "C-D0"],
        "units": {
            "source_timestamp": "source_clock_native",
            "captured_at_ns": "nanoseconds",
            "control_at_ns": "nanoseconds",
            "controller_dt_ns": "nanoseconds",
            "observation_age_ns": "nanoseconds",
            "backend_completed_at_ns": "nanoseconds",
            "world_delta_x": "source_pixels",
            "world_delta_y": "source_pixels",
            "issued_dx": "integer_counts",
            "issued_dy": "integer_counts",
            "backend_completed_dx": "integer_counts",
            "backend_completed_dy": "integer_counts",
            "camera_visible_effect": "source_pixels_xy",
            "physical_dispatch_count": "events",
        },
        "scoring": {
            "evaluation_unit": "block_id+plant_profile_id+variant",
            "tail_primary_metrics": [
                "outside_samples", "outside_duration_ns",
                "longest_outside_samples", "longest_outside_ns",
                "outside_area_px_ns", "max_excess_x_px",
                "max_abs_error_x_px",
            ],
            "diagnostic_only_metrics": [
                "p50_abs_error_x_px", "p95_abs_error_x_px"],
            "candidate_rule": (
                "strict_area_improvement_and_per_metric_non_regression"),
            "block_aggregation": "conjunction_only",
            "float64_tolerance": (
                "64*epsilon*max(1,abs(a),abs(b))*max(1,n)"),
            "physical_dispatch_required": 0,
        },
        "terminal_statuses": [
            "RED_INPUT_INCOMPLETE", "RED_INVALID",
            "BASELINE_REPLAY_FIDELITY_INVALID",
            "BASELINE_RED_NOT_ESTABLISHED", "BASELINE_RED_LOCKED",
            "CANDIDATE_DELETED", "CANDIDATE_DEVELOPMENT_GREEN",
        ],
        "physical_output_capability": False,
        "production_aim_changed": False,
    }
    contract["contract_semantic_sha256"] = _canonical_sha256(contract)
    return contract


def _is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdefABCDEF" for character in value)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and \
        math.isfinite(float(value))


def _is_numeric_vector(value: Any, length: int) -> bool:
    return isinstance(value, list) and len(value) == length and all(
        _is_number(item) for item in value)


def _base_report(status: str, failure_codes: list[str]) -> dict[str, Any]:
    return {
        "red_schema": RED_SCHEMA_VERSION,
        "evidence_type": "aim_production_red_evaluation",
        "status": status,
        "failure_codes": list(dict.fromkeys(failure_codes)),
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "trace_results": [],
    }


def _load_json_lines(payload: bytes) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    text = payload.decode("utf-8")
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        if not raw_line.strip():
            raise ValueError(f"EMPTY_TRACE_LINE:{line_number}")
        value = json.loads(raw_line)
        if not isinstance(value, dict):
            raise ValueError(f"TRACE_ROW_NOT_OBJECT:{line_number}")
        rows.append(value)
    return rows


def _resolve_owned_file(root: pathlib.Path, relative_path: Any) -> pathlib.Path:
    if not isinstance(relative_path, str) or not relative_path:
        raise ValueError("TRACE_RELATIVE_PATH_INVALID")
    candidate = pathlib.Path(relative_path)
    if candidate.is_absolute():
        raise ValueError("TRACE_PATH_NOT_RELATIVE")
    resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise ValueError("TRACE_PATH_ESCAPES_MANIFEST_ROOT") from error
    return resolved


def _percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _score_trace(
        rows: list[dict[str, Any]], begin: int, end: int) -> dict[str, Any]:
    score_rows = rows[begin:end]
    outside_flags: list[bool] = []
    absolute_errors: list[float] = []
    excess_values: list[float] = []
    outside_duration_ns = 0
    outside_area_px_ns = 0.0
    longest_samples = 0
    longest_duration_ns = 0
    current_samples = 0
    current_duration_ns = 0

    for row in score_rows:
        center_x = float(row["control_center"][0])
        base_x = float(row["base_point"][0])
        x1 = float(row["matched_observation_box"][0])
        x2 = float(row["matched_observation_box"][2])
        dt_ns = int(row["controller_dt_ns"])
        outside = center_x < x1 or center_x > x2
        excess = max(x1 - center_x, center_x - x2, 0.0)
        outside_flags.append(outside)
        absolute_errors.append(abs(base_x - center_x))
        excess_values.append(excess)
        if outside:
            outside_duration_ns += dt_ns
            outside_area_px_ns += excess * dt_ns
            current_samples += 1
            current_duration_ns += dt_ns
            longest_samples = max(longest_samples, current_samples)
            longest_duration_ns = max(
                longest_duration_ns, current_duration_ns)
        else:
            current_samples = 0
            current_duration_ns = 0

    nonzero_directions = [
        1 if row["backend_completed_dx"] > 0 else -1
        for row in rows if row["backend_completed_dx"] != 0]
    command_reversals = sum(
        current != previous for previous, current in
        zip(nonzero_directions, nonzero_directions[1:]))
    identities = [[
        row["source_sequence"], row["source_timestamp"],
        row["source_clock_session_id"]] for row in rows]
    return {
        "source_identity_sha256": _canonical_sha256(identities),
        "outside_sequence_sha256": _canonical_sha256(outside_flags),
        "outside_samples": sum(outside_flags),
        "outside_duration_ns": outside_duration_ns,
        "longest_outside_samples": longest_samples,
        "longest_outside_ns": longest_duration_ns,
        "outside_area_px_ns": outside_area_px_ns,
        "max_excess_x_px": max(excess_values, default=0.0),
        "max_abs_error_x_px": max(absolute_errors, default=0.0),
        "p50_abs_error_x_px": _percentile(absolute_errors, 0.50),
        "p95_abs_error_x_px": _percentile(absolute_errors, 0.95),
        "nonzero_completed_command_reversals": command_reversals,
        "nonzero_completed_x_samples": sum(
            row["backend_completed_dx"] != 0 for row in score_rows),
        "nonzero_completed_y_samples": sum(
            row["backend_completed_dy"] != 0 for row in score_rows),
        "visible_effect_abs_area_px_ns": sum(
            (abs(float(row["camera_visible_effect"][0])) +
             abs(float(row["camera_visible_effect"][1]))) *
            int(row["controller_dt_ns"]) for row in score_rows),
        "quantization_zero_outside_samples": sum(
            outside and (row["quantization_zero_x"] or
                         row["quantization_zero_y"])
            for row, outside in zip(score_rows, outside_flags)),
    }


def _validate_trace_rows(
        root: pathlib.Path, manifest: dict[str, Any], trace: dict[str, Any],
        rows: list[dict[str, Any]]) -> tuple[list[str], list[str]]:
    incomplete: list[str] = []
    invalid: list[str] = []
    if any(any(field not in row for field in _SOURCE_FIELDS)
           for row in rows):
        incomplete.append("MISSING_SOURCE_IDENTITY")
    if any(any(field not in row for field in _COMPLETED_FIELDS)
           for row in rows):
        incomplete.append("MISSING_COMPLETED_LEDGER")
    if any(any(field not in row for field in _REQUIRED_ROW_FIELDS)
           for row in rows):
        incomplete.append("MISSING_REQUIRED_ROW_FIELDS")
    if incomplete:
        return incomplete, invalid

    begin = trace.get("score_begin")
    end = trace.get("score_end")
    if not isinstance(begin, int) or isinstance(begin, bool) or \
            not isinstance(end, int) or isinstance(end, bool) or \
            begin < 0 or end <= begin or end > len(rows):
        invalid.append("TRACE_SCORE_INTERVAL_INVALID")
        return incomplete, invalid

    sessions: set[Any] = set()
    source_assets: set[tuple[str, str]] = set()
    previous_sequence: int | None = None
    previous_timestamp: int | None = None
    for index, row in enumerate(rows):
        if row["red_schema"] != RED_SCHEMA_VERSION or \
                row["asset_id"] != manifest.get("asset_id") or \
                row["configuration_sha256"] != \
                manifest.get("configuration_sha256") or \
                row["block_id"] != trace.get("block_id") or \
                row["role"] != trace.get("role") or \
                row["plant_profile_id"] != trace.get("plant_profile_id") or \
                row["score_begin"] != begin or row["score_end"] != end:
            invalid.append("ROW_DESCRIPTOR_BINDING_INVALID")
        if row["sample_index_in_block"] != index or \
                row["reset"] is not (index == 0):
            invalid.append("ROW_SEQUENCE_OR_RESET_INVALID")
        sequence = row["source_sequence"]
        timestamp = row["source_timestamp"]
        session = row["source_clock_session_id"]
        if not isinstance(sequence, int) or isinstance(sequence, bool) or \
                not isinstance(timestamp, int) or isinstance(timestamp, bool) or \
                not isinstance(session, (str, int)) or isinstance(session, bool):
            invalid.append("SOURCE_IDENTITY_TYPE_INVALID")
        else:
            if previous_sequence is not None and sequence <= previous_sequence:
                invalid.append("SOURCE_SEQUENCE_NOT_STRICTLY_INCREASING")
            if previous_timestamp is not None and timestamp <= previous_timestamp:
                invalid.append("SOURCE_TIMESTAMP_NOT_STRICTLY_INCREASING")
            previous_sequence = sequence
            previous_timestamp = timestamp
            sessions.add(session)
        integer_fields = (
            "captured_at_ns", "control_at_ns", "controller_dt_ns",
            "observation_age_ns", "backend_completed_at_ns", "issued_dx",
            "issued_dy", "backend_completed_dx", "backend_completed_dy",
            "aim_actual_history_dx", "aim_actual_history_dy",
            "plant_input_dx", "plant_input_dy",
            "physical_dispatch_count")
        if any(not isinstance(row[field], int) or isinstance(row[field], bool)
               for field in integer_fields):
            invalid.append("ROW_INTEGER_FIELD_INVALID")
        elif row["controller_dt_ns"] <= 0 or \
                row["control_at_ns"] < row["captured_at_ns"] or \
                row["backend_completed_at_ns"] < row["control_at_ns"]:
            invalid.append("ROW_TIME_CONTRACT_INVALID")
        if not _is_numeric_vector(row["matched_observation_box"], 4) or \
                not _is_numeric_vector(row["base_point"], 2) or \
                not _is_numeric_vector(row["delay_compensated_point"], 2) or \
                not _is_numeric_vector(row["prediction_point"], 2) or \
                not _is_numeric_vector(row["control_center"], 2) or \
                not _is_numeric_vector(row["plant_prefix"], 2) or \
                not _is_numeric_vector(row["camera_visible_effect"], 2) or \
                not _is_numeric_vector(row["observed_box"], 4):
            invalid.append("ROW_GEOMETRY_INVALID")
        elif row["matched_observation_box"][0] > \
                row["matched_observation_box"][2] or \
                row["matched_observation_box"][1] > \
                row["matched_observation_box"][3] or \
                row["observed_box"][0] > row["observed_box"][2] or \
                row["observed_box"][1] > row["observed_box"][3]:
            invalid.append("ROW_BOX_ORDER_INVALID")
        if any(not _is_number(row[field]) for field in (
                "world_delta_x", "world_delta_y", "box_width", "box_height")):
            invalid.append("ROW_NONFINITE_VALUE")
        elif row["box_width"] <= 0 or row["box_height"] <= 0:
            invalid.append("ROW_BOX_SIZE_INVALID")
        if not isinstance(row["controller_x"], dict) or any(
                not _is_number(row["controller_x"].get(field)) for field in (
                    "proportional_counts", "feedforward_counts",
                    "desired_counts", "filtered_counts", "shaped_counts")):
            invalid.append("ROW_CONTROLLER_X_INVALID")
        if not isinstance(row["plant_due_queue"], list) or \
                not isinstance(row["completion_status"], str) or \
                not isinstance(row["completion_zero_reason"], str) or \
                not isinstance(row["source_relative_path"], str) or \
                not isinstance(row["pose"], str) or \
                not isinstance(row["aim_status"], str) or \
                not isinstance(row["limit_signature"], str):
            invalid.append("ROW_ENUM_OR_COLLECTION_INVALID")
        if any(not isinstance(row[field], bool) for field in (
                "visible", "quantization_zero_x", "quantization_zero_y",
                "lock_active", "backend_failure")):
            invalid.append("ROW_BOOLEAN_FIELD_INVALID")
        if row["physical_dispatch_count"] != 0:
            invalid.append("OUTPUT_OFF_SAFETY_VIOLATION")
        completed = (
            row["backend_completed_dx"], row["backend_completed_dy"])
        if (row["aim_actual_history_dx"],
                row["aim_actual_history_dy"]) != completed or \
                (row["plant_input_dx"], row["plant_input_dy"]) != completed:
            invalid.append("REQUESTED_OR_ACK_LEAKED_INTO_PLANT")
        if not isinstance(row["protocol_acknowledged"], bool):
            invalid.append("PROTOCOL_ACK_TYPE_INVALID")
        if row["reset"] and completed == (0, 0) and \
                row["plant_due_queue"] == [] and \
                row["plant_prefix"] == [0.0, 0.0] and \
                row["camera_visible_effect"] != [0.0, 0.0]:
            invalid.append("REQUESTED_OR_ACK_LEAKED_INTO_PLANT")
        if not _is_sha256(row["source_sha256"]) or \
                not _is_sha256(row["configuration_sha256"]) or \
                not isinstance(row["extractor"], dict) or \
                not isinstance(row["extractor"].get("name"), str) or \
                not isinstance(row["extractor"].get("version"), int) or \
                not _is_sha256(row["extractor"].get("sha256")):
            invalid.append("ROW_ASSET_IDENTITY_INVALID")
        elif isinstance(row["source_relative_path"], str):
            source_assets.add((
                row["source_relative_path"], row["source_sha256"].lower()))
    if len(sessions) != 1:
        invalid.append("SOURCE_CLOCK_SESSION_MIXED")
    for relative_path, claimed_sha256 in source_assets:
        try:
            source_path = _resolve_owned_file(root, relative_path)
            if not source_path.is_file() or \
                    _file_sha256(source_path) != claimed_sha256:
                invalid.append("SOURCE_ASSET_SHA256_MISMATCH")
        except (OSError, ValueError):
            invalid.append("SOURCE_ASSET_INVALID")
    return incomplete, list(dict.fromkeys(invalid))


def _validate_measured_reference(
        root: pathlib.Path, reference: Any,
        metrics: dict[str, Any]) -> list[str]:
    if not isinstance(reference, dict):
        return ["MISSING_MEASURED_REFERENCE"]
    uncertainty = reference.get("measurement_uncertainty")
    if not isinstance(uncertainty, dict):
        return ["MISSING_MEASUREMENT_UNCERTAINTY"]
    try:
        source_path = _resolve_owned_file(
            root, uncertainty.get("source_relative_path"))
        claimed_hash = uncertainty.get("source_sha256")
        if not source_path.is_file() or not _is_sha256(claimed_hash) or \
                _file_sha256(source_path) != claimed_hash.lower():
            return ["MEASURED_REFERENCE_SOURCE_SHA256_MISMATCH"]
    except (OSError, ValueError):
        return ["MEASURED_REFERENCE_SOURCE_INVALID"]
    failures: list[str] = []
    if reference.get("source_identity_sha256") != \
            metrics["source_identity_sha256"]:
        failures.append("FIDELITY_SOURCE_IDENTITY_MISMATCH")
    if reference.get("outside_sequence_sha256") != \
            metrics["outside_sequence_sha256"]:
        failures.append("FIDELITY_OUTSIDE_SEQUENCE_MISMATCH")
    expected = reference.get("metrics")
    absolute = uncertainty.get("absolute")
    if not isinstance(expected, dict) or not isinstance(absolute, dict):
        return failures + ["MEASURED_REFERENCE_METRICS_INVALID"]
    for field in _INTEGER_METRICS:
        if expected.get(field) != metrics[field]:
            failures.append(f"FIDELITY_{field.upper()}_MISMATCH")
    for field in _FLOAT_METRICS:
        tolerance = absolute.get(field)
        expected_value = expected.get(field)
        if not _is_number(tolerance) or float(tolerance) < 0.0 or \
                not _is_number(expected_value):
            failures.append("MEASURED_REFERENCE_METRICS_INVALID")
        elif abs(float(expected_value) - float(metrics[field])) > \
                float(tolerance):
            failures.append(f"FIDELITY_{field.upper()}_MISMATCH")
    return list(dict.fromkeys(failures))


def _measured_reference_differences(
        reference: Any, metrics: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(reference, dict) or \
            not isinstance(reference.get("metrics"), dict):
        return {}
    differences: dict[str, Any] = {}
    for field in (*_INTEGER_METRICS, *_FLOAT_METRICS):
        measured = reference["metrics"].get(field)
        replay = metrics[field]
        if measured != replay:
            differences[field] = {
                "measured": measured,
                "replay": replay,
            }
    if reference.get("source_identity_sha256") != \
            metrics["source_identity_sha256"]:
        differences["source_identity_sha256"] = {
            "measured": reference.get("source_identity_sha256"),
            "replay": metrics["source_identity_sha256"],
        }
    if reference.get("outside_sequence_sha256") != \
            metrics["outside_sequence_sha256"]:
        differences["outside_sequence_sha256"] = {
            "measured": reference.get("outside_sequence_sha256"),
            "replay": metrics["outside_sequence_sha256"],
        }
    return differences


def _float_tolerance(a: float, b: float, sample_count: int) -> float:
    return 64.0 * math.ulp(1.0) * max(1.0, abs(a), abs(b)) * max(
        1, sample_count)


def _paired_inputs_equal(
        baseline_rows: list[dict[str, Any]],
        candidate_rows: list[dict[str, Any]]) -> bool:
    if len(baseline_rows) != len(candidate_rows):
        return False
    return all(
        all(baseline.get(field) == candidate.get(field)
            for field in _PAIRED_INPUT_FIELDS)
        for baseline, candidate in zip(baseline_rows, candidate_rows))


def _y_signature(row: dict[str, Any]) -> tuple[Any, ...]:
    return (
        row["aim_status"],
        row["matched_observation_box"][1],
        row["matched_observation_box"][3],
        row["base_point"][1],
        row["delay_compensated_point"][1],
        row["prediction_point"][1],
        row["control_center"][1],
        row["issued_dy"],
        row["backend_completed_dy"],
        row["aim_actual_history_dy"],
        row["plant_input_dy"],
        row["plant_prefix"][1],
        row["camera_visible_effect"][1],
        row["observed_box"][1],
        row["observed_box"][3],
        row["quantization_zero_y"],
        row["limit_signature"],
    )


def _y_contract_equal(
        baseline_rows: list[dict[str, Any]],
        candidate_rows: list[dict[str, Any]]) -> bool:
    return len(baseline_rows) == len(candidate_rows) and all(
        _y_signature(baseline) == _y_signature(candidate)
        for baseline, candidate in zip(baseline_rows, candidate_rows))


def _tail_comparison(
        baseline: dict[str, Any], candidate: dict[str, Any],
        sample_count: int, require_longest_reduction: bool) -> tuple[bool, list[str]]:
    failures: list[str] = []
    baseline_area = float(baseline["outside_area_px_ns"])
    candidate_area = float(candidate["outside_area_px_ns"])
    if baseline_area == 0.0:
        zero_fields = (
            "outside_samples", "outside_duration_ns",
            "longest_outside_samples", "longest_outside_ns",
            "outside_area_px_ns", "max_excess_x_px")
        if any(candidate[field] != 0 for field in zero_fields):
            failures.append("ZERO_BASELINE_BLOCK_REGRESSED")
    elif baseline_area - candidate_area <= _float_tolerance(
            baseline_area, candidate_area, sample_count):
        failures.append("OUTSIDE_AREA_NOT_STRICTLY_IMPROVED")

    for field in _TAIL_NON_REGRESSION_METRICS:
        baseline_value = baseline[field]
        candidate_value = candidate[field]
        if isinstance(baseline_value, int) and isinstance(candidate_value, int):
            regressed = candidate_value > baseline_value
        else:
            regressed = float(candidate_value) > float(baseline_value) + \
                _float_tolerance(
                    float(baseline_value), float(candidate_value), sample_count)
        if regressed:
            failures.append(f"{field.upper()}_REGRESSED")
    if require_longest_reduction and \
            candidate["longest_outside_samples"] > \
            baseline["longest_outside_samples"] - 1:
        failures.append("GLOBAL_LONGEST_NOT_REDUCED")
    return not failures, failures


def _evaluate_manifest(manifest_path: pathlib.Path | str) -> dict[str, Any]:
    """读取 manifest/trace 并返回 fail-closed、无物理能力的裁决报告。"""

    path = pathlib.Path(manifest_path).resolve()
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return _base_report("RED_INVALID", ["MANIFEST_UNREADABLE"])

    if not isinstance(manifest, dict) or \
            manifest.get("red_schema") != RED_SCHEMA_VERSION or \
            manifest.get("evidence_type") != EVIDENCE_TYPE:
        return _base_report("RED_INVALID", ["MANIFEST_SCHEMA_INVALID"])
    generator = manifest.get("generator")
    if not isinstance(manifest.get("asset_id"), str) or \
            not isinstance(generator, dict) or \
            not isinstance(generator.get("name"), str) or \
            not isinstance(generator.get("version"), int) or \
            not _is_sha256(generator.get("sha256")) or \
            not _is_sha256(manifest.get("configuration_sha256")):
        return _base_report("RED_INVALID", ["MANIFEST_IDENTITY_INVALID"])
    if manifest.get("physical_output_capability") is not False or \
            manifest.get("physical_dispatch_count") != 0:
        return _base_report(
            "RED_INVALID", ["OUTPUT_OFF_SAFETY_VIOLATION"])
    if not isinstance(manifest.get("candidate_uses_f1"), bool):
        return _base_report("RED_INVALID", ["F1_USAGE_DECLARATION_INVALID"])
    production_profile = manifest.get("production_plant_profile_id")
    mandatory_pairs_value = manifest.get("mandatory_block_profiles")
    if not isinstance(production_profile, str) or \
            not isinstance(mandatory_pairs_value, list) or \
            not mandatory_pairs_value:
        return _base_report(
            "RED_INPUT_INCOMPLETE", ["MANDATORY_BLOCK_PROFILES_MISSING"])
    try:
        mandatory_pairs = {
            (str(value["block_id"]), str(value["plant_profile_id"]))
            for value in mandatory_pairs_value
            if isinstance(value, dict)
        }
    except (KeyError, TypeError):
        return _base_report(
            "RED_INVALID", ["MANDATORY_BLOCK_PROFILES_INVALID"])
    if len(mandatory_pairs) != len(mandatory_pairs_value) or \
            production_profile not in {profile for _, profile in mandatory_pairs}:
        return _base_report(
            "RED_INVALID", ["MANDATORY_BLOCK_PROFILES_INVALID"])

    traces = manifest.get("traces")
    if not isinstance(traces, list) or not traces:
        return _base_report("RED_INPUT_INCOMPLETE", ["MISSING_TRACES"])

    incomplete: list[str] = []
    invalid: list[str] = []
    loaded: list[tuple[dict[str, Any], list[dict[str, Any]]]] = []
    for trace in traces:
        if not isinstance(trace, dict):
            invalid.append("TRACE_DESCRIPTOR_INVALID")
            continue
        if any(field not in trace for field in
               _REQUIRED_TRACE_DESCRIPTOR_FIELDS):
            incomplete.append("MISSING_TRACE_DESCRIPTOR_FIELDS")
            continue
        try:
            trace_path = _resolve_owned_file(
                path.parent, trace.get("relative_path"))
            if not trace_path.is_file():
                raise ValueError("TRACE_FILE_MISSING")
            claimed_sha256 = trace.get("sha256")
            trace_payload = trace_path.read_bytes()
            if not isinstance(claimed_sha256, str) or \
                    len(claimed_sha256) != 64 or \
                    hashlib.sha256(trace_payload).hexdigest() != \
                    claimed_sha256.lower():
                raise ValueError("TRACE_SHA256_MISMATCH")
            rows = _load_json_lines(trace_payload)
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
            invalid.append(str(error))
            continue

        if trace.get("sample_count") != len(rows):
            invalid.append("TRACE_SAMPLE_COUNT_MISMATCH")
        row_incomplete, row_invalid = _validate_trace_rows(
            path.parent, manifest, trace, rows)
        incomplete.extend(row_incomplete)
        invalid.extend(row_invalid)
        loaded.append((trace, rows))

    if invalid:
        return _base_report("RED_INVALID", invalid)
    if incomplete:
        return _base_report("RED_INPUT_INCOMPLETE", incomplete)

    actual_baseline_pairs = {
        (str(trace.get("block_id")), str(trace.get("plant_profile_id")))
        for trace, _ in loaded if trace.get("variant") == "B0"
    }
    if mandatory_pairs - actual_baseline_pairs:
        return _base_report(
            "RED_INPUT_INCOMPLETE", ["MANDATORY_BASELINE_TRACE_MISSING"])
    if actual_baseline_pairs - mandatory_pairs:
        return _base_report(
            "RED_INVALID", ["UNDECLARED_BASELINE_TRACE"])

    report = _base_report("BASELINE_RED_NOT_ESTABLISHED", [])
    report["loaded_trace_count"] = len(loaded)
    fidelity_failures: list[str] = []
    any_red = False
    result_by_key: dict[tuple[str, str, str], dict[str, Any]] = {}
    rows_by_key: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    trace_by_key: dict[tuple[str, str, str], dict[str, Any]] = {}
    for trace, rows in loaded:
        metrics = _score_trace(
            rows, int(trace["score_begin"]), int(trace["score_end"]))
        variant = trace.get("variant")
        if variant not in ("B0", "C", "C-F1-OFF", "C-D0"):
            return _base_report("RED_INVALID", ["TRACE_VARIANT_INVALID"])
        reference_failures = []
        if variant == "B0":
            reference_failures = _validate_measured_reference(
                path.parent, trace.get("measured_reference"), metrics)
            fidelity_failures.extend(reference_failures)
            any_red = any_red or (
                trace.get("plant_profile_id") == production_profile and
                metrics["outside_area_px_ns"] > 0.0)
        result = {
            "trace_id": trace.get("trace_id"),
            "block_id": trace.get("block_id"),
            "role": trace.get("role"),
            "variant": variant,
            "plant_profile_id": trace.get("plant_profile_id"),
            "metrics": metrics,
            "baseline_replay_fidelity_pass": (
                not reference_failures if variant == "B0" else None),
            "fidelity_failure_codes": reference_failures,
            "fidelity_differences": (
                _measured_reference_differences(
                    trace.get("measured_reference"), metrics)
                if variant == "B0" else {}),
        }
        report["trace_results"].append(result)
        key = (str(trace.get("block_id")),
               str(trace.get("plant_profile_id")), str(variant))
        if key in result_by_key:
            return _base_report("RED_INVALID", ["DUPLICATE_TRACE_VARIANT"])
        result_by_key[key] = result
        rows_by_key[key] = rows
        trace_by_key[key] = trace
    if fidelity_failures:
        if any(code.startswith("MISSING_") for code in fidelity_failures):
            report["status"] = "RED_INPUT_INCOMPLETE"
        elif any(code in (
                "MEASURED_REFERENCE_SOURCE_SHA256_MISMATCH",
                "MEASURED_REFERENCE_SOURCE_INVALID",
                "MEASURED_REFERENCE_METRICS_INVALID")
                 for code in fidelity_failures):
            report["status"] = "RED_INVALID"
        else:
            report["status"] = "BASELINE_REPLAY_FIDELITY_INVALID"
        report["failure_codes"] = list(dict.fromkeys(fidelity_failures))
        return report
    if not any_red:
        return report

    baseline_keys = [key for key in result_by_key if key[2] == "B0"]
    candidate_keys = [key for key in result_by_key if key[2] == "C"]
    auxiliary_keys = [key for key in result_by_key
                      if key[2] in ("C-F1-OFF", "C-D0")]
    report["status"] = "BASELINE_RED_LOCKED"
    if not candidate_keys:
        if auxiliary_keys:
            report["status"] = "RED_INVALID"
            report["failure_codes"] = ["ORPHAN_CANDIDATE_AUXILIARY_TRACE"]
        return report
    expected_candidate_keys = {(block, profile, "C")
                               for block, profile, _ in baseline_keys}
    if set(candidate_keys) != expected_candidate_keys:
        report["status"] = "RED_INPUT_INCOMPLETE"
        report["failure_codes"] = ["CANDIDATE_BLOCK_COVERAGE_INCOMPLETE"]
        return report

    for block, profile, _ in baseline_keys:
        baseline_rows = rows_by_key[(block, profile, "B0")]
        candidate_rows = rows_by_key[(block, profile, "C")]
        baseline_descriptor = trace_by_key[(block, profile, "B0")]
        candidate_descriptor = trace_by_key[(block, profile, "C")]
        if any(baseline_descriptor.get(field) !=
               candidate_descriptor.get(field)
               for field in ("role", "block_kind", "sample_count",
                             "score_begin", "score_end")):
            report["status"] = "RED_INVALID"
            report["failure_codes"] = ["VARIANT_DESCRIPTOR_MISMATCH"]
            return report
        if not _paired_inputs_equal(baseline_rows, candidate_rows):
            report["status"] = "RED_INVALID"
            report["failure_codes"] = ["VARIANT_INPUT_MISMATCH"]
            return report

    aggregate = {
        "B0": {"outside_area_px_ns": 0.0},
        "C": {"outside_area_px_ns": 0.0},
    }
    for variant in ("B0", "C"):
        aggregate[variant]["outside_area_px_ns"] = sum(
            float(result["metrics"]["outside_area_px_ns"])
            for key, result in result_by_key.items() if key[2] == variant)
    report["aggregate_diagnostics"] = aggregate

    global_longest = max(
        int(result_by_key[key]["metrics"]["longest_outside_samples"])
        for key in baseline_keys)
    comparisons: list[dict[str, Any]] = []
    any_tail_failure = False
    for block, profile, _ in baseline_keys:
        baseline = result_by_key[(block, profile, "B0")]["metrics"]
        candidate = result_by_key[(block, profile, "C")]["metrics"]
        tail_pass, tail_failures = _tail_comparison(
            baseline, candidate,
            len(rows_by_key[(block, profile, "B0")]),
            int(baseline["longest_outside_samples"]) == global_longest and
            global_longest > 0)
        any_tail_failure = any_tail_failure or not tail_pass
        comparisons.append({
            "block_id": block,
            "plant_profile_id": profile,
            "tail_pass": tail_pass,
            "failure_codes": tail_failures,
        })
    report["block_comparisons"] = comparisons
    if any_tail_failure:
        report["status"] = "CANDIDATE_DELETED"
        profile_ids = {item["plant_profile_id"] for item in comparisons}
        if len(profile_ids) > 1:
            report["failure_codes"] = ["MODEL_MISMATCH_RED"]
        elif aggregate["C"]["outside_area_px_ns"] < \
                aggregate["B0"]["outside_area_px_ns"]:
            report["failure_codes"] = [
                "AGGREGATE_MASKED_BLOCK_REGRESSION"]
        else:
            report["failure_codes"] = ["WORST_TAIL_RED"]
        return report

    y_mismatches: list[dict[str, str]] = []
    for block, profile, _ in baseline_keys:
        if not _y_contract_equal(
                rows_by_key[(block, profile, "B0")],
                rows_by_key[(block, profile, "C")]):
            y_mismatches.append({
                "block_id": block,
                "plant_profile_id": profile,
            })
    report["y_contract_mismatches"] = y_mismatches
    if y_mismatches:
        report["status"] = "CANDIDATE_DELETED"
        report["failure_codes"] = ["X_ONLY_CONTRACT_BROKEN"]
        return report

    reversal_comparisons: list[dict[str, Any]] = []
    reversal_failed = False
    for block, profile, _ in baseline_keys:
        baseline_descriptor = trace_by_key[(block, profile, "B0")]
        change_points = baseline_descriptor.get("change_points")
        if change_points is None:
            continue
        candidate_descriptor = trace_by_key[(block, profile, "C")]
        absolute_limit = baseline_descriptor.get(
            "max_nonzero_completed_command_reversals")
        if not isinstance(change_points, list) or \
                not isinstance(absolute_limit, int) or \
                candidate_descriptor.get("change_points") != change_points or \
                candidate_descriptor.get(
                    "max_nonzero_completed_command_reversals") != absolute_limit:
            report["status"] = "RED_INVALID"
            report["failure_codes"] = ["REVERSAL_CONTRACT_INVALID"]
            return report
        baseline_reversals = result_by_key[
            (block, profile, "B0")]["metrics"][
                "nonzero_completed_command_reversals"]
        candidate_reversals = result_by_key[
            (block, profile, "C")]["metrics"][
                "nonzero_completed_command_reversals"]
        passed = candidate_reversals <= absolute_limit and \
            candidate_reversals <= baseline_reversals
        reversal_failed = reversal_failed or not passed
        reversal_comparisons.append({
            "block_id": block,
            "plant_profile_id": profile,
            "expected_true_reversals": len(change_points),
            "baseline_reversals": baseline_reversals,
            "candidate_reversals": candidate_reversals,
            "absolute_limit": absolute_limit,
            "pass": passed,
        })
    report["reversal_comparisons"] = reversal_comparisons
    if reversal_failed:
        report["status"] = "CANDIDATE_DELETED"
        report["failure_codes"] = ["NATURAL_REVERSAL_RED"]
        return report

    static_comparisons: list[dict[str, Any]] = []
    static_failed = False
    static_metrics = (
        "nonzero_completed_x_samples",
        "nonzero_completed_y_samples",
        "nonzero_completed_command_reversals",
        "outside_samples",
        "outside_duration_ns",
        "longest_outside_samples",
        "longest_outside_ns",
        "outside_area_px_ns",
        "visible_effect_abs_area_px_ns",
        "quantization_zero_outside_samples",
    )
    for block, profile, _ in baseline_keys:
        baseline_descriptor = trace_by_key[(block, profile, "B0")]
        if baseline_descriptor.get("block_kind") != "static":
            continue
        candidate_descriptor = trace_by_key[(block, profile, "C")]
        if candidate_descriptor.get("block_kind") != "static":
            report["status"] = "RED_INVALID"
            report["failure_codes"] = ["BLOCK_KIND_MISMATCH"]
            return report
        baseline = result_by_key[(block, profile, "B0")]["metrics"]
        candidate = result_by_key[(block, profile, "C")]["metrics"]
        regressed = [field for field in static_metrics
                     if candidate[field] > baseline[field]]
        static_failed = static_failed or bool(regressed)
        static_comparisons.append({
            "block_id": block,
            "plant_profile_id": profile,
            "pass": not regressed,
            "regressed_metrics": regressed,
        })
    report["static_comparisons"] = static_comparisons
    if static_failed:
        report["status"] = "CANDIDATE_DELETED"
        report["failure_codes"] = ["STATIC_REGRESSION"]
        return report

    f1_off_keys = [key for key in result_by_key if key[2] == "C-F1-OFF"]
    if not manifest["candidate_uses_f1"] and f1_off_keys:
        report["status"] = "RED_INVALID"
        report["failure_codes"] = ["UNDECLARED_F1_DELETION_TRACE"]
        return report
    if manifest["candidate_uses_f1"]:
        expected_f1_off_keys = {(block, profile, "C-F1-OFF")
                                for block, profile, _ in baseline_keys}
        if set(f1_off_keys) != expected_f1_off_keys:
            report["status"] = "RED_INPUT_INCOMPLETE"
            report["failure_codes"] = ["F1_DELETION_TRACE_MISSING"]
            return report
        f1_comparisons: list[dict[str, Any]] = []
        leverage = False
        regression = False
        for block, profile, _ in baseline_keys:
            baseline_rows = rows_by_key[(block, profile, "B0")]
            f1_off_rows = rows_by_key[(block, profile, "C-F1-OFF")]
            if not _paired_inputs_equal(baseline_rows, f1_off_rows):
                report["status"] = "RED_INVALID"
                report["failure_codes"] = ["VARIANT_INPUT_MISMATCH"]
                return report
            baseline_metrics = result_by_key[
                (block, profile, "B0")]["metrics"]
            candidate_metrics = result_by_key[
                (block, profile, "C")]["metrics"]
            f1_off_metrics = result_by_key[
                (block, profile, "C-F1-OFF")]["metrics"]
            regressed_fields: list[str] = []
            for field in ("outside_area_px_ns",
                          *_TAIL_NON_REGRESSION_METRICS):
                candidate_value = candidate_metrics[field]
                f1_off_value = f1_off_metrics[field]
                if isinstance(candidate_value, int) and \
                        isinstance(f1_off_value, int):
                    field_regressed = candidate_value > f1_off_value
                else:
                    field_regressed = float(candidate_value) > \
                        float(f1_off_value) + _float_tolerance(
                            float(candidate_value), float(f1_off_value),
                            len(baseline_rows))
                if field_regressed:
                    regressed_fields.append(field)
            baseline_red = baseline_metrics["outside_area_px_ns"] > 0.0
            block_leverage = baseline_red and \
                float(f1_off_metrics["outside_area_px_ns"]) - \
                float(candidate_metrics["outside_area_px_ns"]) > \
                _float_tolerance(
                    float(f1_off_metrics["outside_area_px_ns"]),
                    float(candidate_metrics["outside_area_px_ns"]),
                    len(baseline_rows))
            leverage = leverage or block_leverage
            regression = regression or bool(regressed_fields)
            f1_comparisons.append({
                "block_id": block,
                "plant_profile_id": profile,
                "has_strict_leverage": block_leverage,
                "regressed_metrics": regressed_fields,
            })
        report["f1_deletion_comparisons"] = f1_comparisons
        if regression:
            report["status"] = "CANDIDATE_DELETED"
            report["failure_codes"] = ["F1_DELETION_REGRESSION"]
            return report
        if not leverage:
            report["status"] = "CANDIDATE_DELETED"
            report["failure_codes"] = ["F1_DELETION_NO_LEVERAGE"]
            return report

    candidate_off_keys = [key for key in result_by_key if key[2] == "C-D0"]
    expected_candidate_off_keys = {(block, profile, "C-D0")
                                   for block, profile, _ in baseline_keys}
    if set(candidate_off_keys) != expected_candidate_off_keys:
        report["status"] = "RED_INPUT_INCOMPLETE"
        report["failure_codes"] = ["CANDIDATE_DELETION_TRACE_MISSING"]
        return report
    deletion_mismatches: list[dict[str, str]] = []
    for block, profile, _ in baseline_keys:
        baseline_rows = rows_by_key[(block, profile, "B0")]
        candidate_off_rows = rows_by_key[(block, profile, "C-D0")]
        if baseline_rows != candidate_off_rows:
            deletion_mismatches.append({
                "block_id": block,
                "plant_profile_id": profile,
            })
    report["candidate_deletion_mismatches"] = deletion_mismatches
    if deletion_mismatches:
        report["status"] = "CANDIDATE_DELETED"
        report["failure_codes"] = ["CANDIDATE_DELETION_NOT_BASELINE"]
        return report
    report["status"] = "CANDIDATE_DEVELOPMENT_GREEN"
    return report


def evaluate_manifest(manifest_path: pathlib.Path | str) -> dict[str, Any]:
    """公开 evaluator seam；报告绑定输入、schema 与 evaluator 文件身份。"""

    path = pathlib.Path(manifest_path).resolve()
    before_sha256 = _file_sha256(path) if path.is_file() else None
    report = _evaluate_manifest(path)
    after_sha256 = _file_sha256(path) if path.is_file() else None
    if before_sha256 != after_sha256:
        report = _base_report(
            "RED_INVALID", ["MANIFEST_CHANGED_DURING_EVALUATION"])
        report["manifest_sha256_before"] = before_sha256
        report["manifest_sha256_after"] = after_sha256
    report["manifest_file_sha256"] = after_sha256
    report["schema_contract_semantic_sha256"] = \
        red_schema_contract()["contract_semantic_sha256"]
    report["evaluator_file_sha256"] = _file_sha256(
        pathlib.Path(__file__).resolve())
    report["evaluation_semantic_sha256"] = _canonical_sha256(report)
    return report


def _write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"拒绝覆盖既有 evaluator report: {path}")
    temporary = path.with_name(f".{path.name}.incoming")
    if temporary.exists():
        raise FileExistsError(f"输出临时文件已存在: {temporary}")
    try:
        temporary.write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="评估 hash-bound Aim production red trace")
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args(argv)
    if arguments.manifest.resolve() == arguments.output.resolve():
        parser.error("output 不得覆盖 manifest")
    report = evaluate_manifest(arguments.manifest)
    _write_json_atomic(arguments.output.resolve(), report)
    print(f"Aim production red evaluator: status={report['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
