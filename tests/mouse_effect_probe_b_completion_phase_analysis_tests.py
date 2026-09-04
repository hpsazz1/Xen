#!/usr/bin/env python3
"""验证完成时刻归因只删除 source-sample-only 假设，不产生 Aim 候选。"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import tempfile
from typing import Any


AMPLITUDES = (1, 1, 4, 4, 13, 13, 2, 2, 8, 8)
ROLES = ("estimation",) * 6 + ("confirmation",) * 4
POLARITIES = ("normal", "inverted") * 5
RESPONSE_ROWS = 49
FRAME_COUNT = 1684
SOURCE_SESSION = "5808209070696154636"


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: dict[str, Any]) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def load_module(path: pathlib.Path) -> Any:
    spec = importlib.util.spec_from_file_location(
        "mouse_effect_probe_b_completion_phase_analysis", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载 completion-phase analyzer")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def metric(observed: list[float], predicted: list[float]) -> dict[str, float]:
    residual = [actual - expected
                for actual, expected in zip(observed, predicted)]
    return {
        "rmse_px": (sum(value * value for value in residual) /
                    len(residual)) ** 0.5,
        "mae_px": sum(abs(value) for value in residual) / len(residual),
        "max_abs_error_px": max(abs(value) for value in residual),
    }


def pulse_layout() -> list[dict[str, int | str]]:
    pulses: list[dict[str, int | str]] = []
    for block_index, amplitude in enumerate(AMPLITUDES):
        first = 64 + block_index * 162
        sign = 1 if POLARITIES[block_index] == "normal" else -1
        for pulse_ordinal, (sample_index, command) in enumerate((
                (first + 32, sign * amplitude),
                (first + 32 + RESPONSE_ROWS, -sign * amplitude)), 1):
            pulses.append({
                "block_id": block_index + 1,
                "pair_index": block_index // 2 + 1,
                "role": ROLES[block_index],
                "polarity": POLARITIES[block_index],
                "amplitude_counts": amplitude,
                "pulse_ordinal": pulse_ordinal,
                "command_dx_counts": command,
                "sample_index": sample_index,
            })
    return pulses


def build_fixture(root: pathlib.Path, *, uniform_onset: bool = False,
                  delayed_completion_ms: float = 10.0) -> pathlib.Path:
    layouts = pulse_layout()
    onsets: dict[tuple[int, int], int] = {
        (int(row["block_id"]), int(row["pulse_ordinal"])): 4
        for row in layouts
    }
    if not uniform_onset:
        onsets[(6, 1)] = 3
        onsets[(9, 1)] = 6

    state_hashes = [
        hashlib.sha256(f"state-{index}".encode("ascii")).hexdigest()
        for index in range(len(layouts) + 1)
    ]
    hashes: list[str] = []
    cursor = 0
    state = 0
    for row in layouts:
        transition = int(row["sample_index"]) + onsets[
            (int(row["block_id"]), int(row["pulse_ordinal"]))]
        hashes.extend([state_hashes[state]] * (transition - cursor))
        cursor = transition
        state += 1
    hashes.extend([state_hashes[state]] * (FRAME_COUNT - cursor))
    expect(len(hashes) == FRAME_COUNT, "合成 frame hash 时间线容量错误")

    base_ns = 10_000_000_000
    cadence_ns = 4_000_000
    frames: list[dict[str, Any]] = []
    for index, bgr_sha256 in enumerate(hashes):
        frames.append({
            "index": index,
            "bgr_sha256": bgr_sha256,
            "source_timestamp": 1_000_000 + index,
            "source_timestamp_valid": True,
            "source_time_at_steady_ns": base_ns + index * cadence_ns,
            "source_time_basis": "NDI_SDK_SUBMISSION",
            "source_time_timing_valid": True,
            "source_clock_status": "VALID",
            "source_clock_session_id": int(SOURCE_SESSION),
            "source_clock_uncertainty_ms": 0.15,
        })

    events: list[dict[str, Any]] = []
    pulse_measurements: list[dict[str, Any]] = []
    fit_curve = [0.0 if offset < 3 else -0.5
                 for offset in range(RESPONSE_ROWS)]
    f1_gain = -0.4
    confirmation: list[dict[str, Any]] = []
    for row in layouts:
        block_id = int(row["block_id"])
        pulse_ordinal = int(row["pulse_ordinal"])
        command = int(row["command_dx_counts"])
        sample_index = int(row["sample_index"])
        onset = onsets[(block_id, pulse_ordinal)]
        source_ns = int(frames[sample_index]["source_time_at_steady_ns"])
        completion_ms = 2.0
        if not uniform_onset and (block_id, pulse_ordinal) == (6, 1):
            completion_ms = 1.5
        if not uniform_onset and (block_id, pulse_ordinal) == (9, 1):
            completion_ms = delayed_completion_ms
        completed_ns = source_ns + int(completion_ms * 1_000_000)
        scheduled_ns = completed_ns - 500_000
        issued_ns = completed_ns - 400_000
        ack_ns = completed_ns - 100_000
        events.append({
            "run_uuid": "12345678-1234-4234-8234-123456789abc",
            "block_id": block_id,
            "sample_index": sample_index,
            "source_timestamp": int(frames[sample_index]["source_timestamp"]),
            "source_timestamp_valid": True,
            "source_time_at_steady_ns": source_ns,
            "source_time_basis": "NDI_SDK_SUBMISSION",
            "source_clock_status": "VALID",
            "source_clock_session_id": SOURCE_SESSION,
            "source_clock_uncertainty_ms": 0.15,
            "scheduled_at_steady_ns": scheduled_ns,
            "issued_at_steady_ns": issued_ns,
            "nominal_dx_counts": command,
            "nominal_dy_counts": 0,
            "requested_dx_counts": command,
            "requested_dy_counts": 0,
            "safety_allowed": True,
            "dispatch_attempted": True,
            "backend_succeeded": True,
            "protocol_ack_received": True,
            "protocol_ack_received_at_steady_ns": ack_ns,
            "backend_completed_at_steady_ns": completed_ns,
            "returned_at_steady_ns": completed_ns + 100_000,
        })
        observed = [0.0 if offset < onset else f1_gain * command
                    for offset in range(RESPONSE_ROWS)]
        measurement: dict[str, Any] = {
            key: row[key] for key in (
                "block_id", "pair_index", "role", "polarity",
                "amplitude_counts", "pulse_ordinal", "command_dx_counts")
        }
        measurement.update({
            "source_timestamp": int(frames[sample_index]["source_timestamp"]),
            "manifest_first_index": sample_index,
            "manifest_last_index": sample_index + RESPONSE_ROWS - 1,
            "left_dx_px": observed,
            "right_dx_px": observed,
        })
        pulse_measurements.append(measurement)
        if row["role"] == "confirmation":
            model_prediction = [value * command for value in fit_curve]
            f1_prediction = [0.0 if offset < 4 else f1_gain * command
                             for offset in range(RESPONSE_ROWS)]
            model_metric = metric(observed, model_prediction)
            f1_metric = metric(observed, f1_prediction)
            witness = {
                "model": model_metric,
                "frozen_f1": f1_metric,
                "model_strictly_beats_f1_rmse":
                    model_metric["rmse_px"] < f1_metric["rmse_px"],
                "model_strictly_beats_f1_max":
                    model_metric["max_abs_error_px"] <
                    f1_metric["max_abs_error_px"],
                "opposite_direction": True,
            }
            confirmation.append({
                key: measurement[key] for key in (
                    "block_id", "pair_index", "role", "polarity",
                    "amplitude_counts", "pulse_ordinal",
                    "command_dx_counts")
            } | {
                "witnesses": {
                    "left": copy.deepcopy(witness),
                    "right": copy.deepcopy(witness),
                },
                "model_strictly_beats_f1_rmse":
                    witness["model_strictly_beats_f1_rmse"],
                "model_strictly_beats_f1_max":
                    witness["model_strictly_beats_f1_max"],
                "opposite_direction": True,
                "whole_pulse_pass": False,
            })

    report = {
        "run_uuid": "12345678-1234-4234-8234-123456789abc",
        "dispatch_mode": "physical_b",
        "profile": "physical_b_command_magnitude_primary",
        "result": {
            "complete": True,
            "stop_reason": "normal_completion",
            "events": events,
        },
    }
    manifest = {
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "recorded_frame_count": FRAME_COUNT,
        "frames": frames,
    }
    report_path = root / "command-report.json"
    manifest_path = root / "pixel-evidence" / "manifest.json"
    write_json(report_path, report)
    write_json(manifest_path, manifest)
    analysis: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_command_magnitude_primary_analysis",
        "status": "LINEAR_STEP_RESPONSE_DELETED",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "new_production_gain_claimed": False,
        "analysis_contract": {
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "production_aim_changed": False,
            "measurement": {
                "pulse_anchor":
                    "exact_event_frame_immediately_before_pulse",
                "source_join": "exact_int64_ndi_submission_timestamp",
                "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
                "response_rows_including_pulse": RESPONSE_ROWS,
            },
            "model": {
                "input": "backend_completed_relative_command_dx_counts",
                "fit":
                    "least_squares_through_origin_at_each_response_offset",
                "fit_amplitudes": [1, 4, 13],
                "within_run_confirmation_amplitudes": [2, 8],
                "confirmation_used_for_refit": False,
            },
            "deletion_tests": {
                "failure_status": "LINEAR_STEP_RESPONSE_DELETED",
            },
        },
        "primary": {
            "run_uuid": report["run_uuid"],
            "source_clock_session_id": SOURCE_SESSION,
            "command_report_sha256": file_sha256(report_path),
            "manifest_sha256": file_sha256(manifest_path),
            "physical_output_was_historical_input": True,
            "physical_output_dispatched_by_this_analysis": False,
        },
        "evaluation": {
            "status": "LINEAR_STEP_RESPONSE_DELETED",
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "production_aim_changed": False,
            "new_production_gain_claimed": False,
            "fit": {
                "fit_role": "estimation_whole_pulses_only",
                "confirmation_used_for_refit": False,
                "left_step_response_px_per_count": fit_curve,
                "right_step_response_px_per_count": fit_curve,
            },
            "frozen_f1": {
                "delay_samples": 4,
                "gains": {"left": f1_gain, "right": f1_gain},
            },
            "confirmation": confirmation,
            "all_confirmation_whole_pulses_pass": False,
            "cross_run_holdout_required_before_candidate": True,
        },
        "pulse_measurements": pulse_measurements,
    }
    analysis["analysis_semantic_sha256"] = canonical_sha256(analysis)
    analysis_path = root / "command-magnitude-analysis.json"
    write_json(analysis_path, analysis)
    return analysis_path


def rebind_analysis(root: pathlib.Path, analysis_path: pathlib.Path) -> None:
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    analysis["primary"]["command_report_sha256"] = file_sha256(
        root / "command-report.json")
    analysis["primary"]["manifest_sha256"] = file_sha256(
        root / "pixel-evidence" / "manifest.json")
    analysis.pop("analysis_semantic_sha256", None)
    analysis["analysis_semantic_sha256"] = canonical_sha256(analysis)
    write_json(analysis_path, analysis)


def expect_cli_failure(module: Any, arguments: list[str], message: str) -> None:
    with contextlib.redirect_stderr(io.StringIO()):
        result = module.main(arguments)
    expect(result == 2, message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=pathlib.Path)
    options = parser.parse_args()
    module = load_module(options.analyzer.resolve())

    with tempfile.TemporaryDirectory(
            prefix="xen-completion-phase-analysis-") as temporary:
        root = pathlib.Path(temporary)
        analysis_path = build_fixture(root)
        output_path = root / "completion-phase.json"
        expect(module.main([
                   "--run-directory", str(root.resolve()),
                   "--command-magnitude-analysis", str(analysis_path.resolve()),
                   "--output", str(output_path.resolve()),
               ]) == 0 and output_path.is_file(),
               "完整 hash-bound fixture 应发布只读归因")
        output = json.loads(output_path.read_text(encoding="utf-8"))
        evaluation = output["evaluation"]
        expect(output["status"] == "SOURCE_SAMPLE_ALIGNMENT_CONFOUNDED" and
               output["physical_output_capability"] is False and
               output["physical_dispatch_count"] == 0 and
               output["production_aim_changed"] is False and
               output["new_production_gain_claimed"] is False and
               output["used_for_model_selection"] is True and
               output["validation_evidence"] is False,
               "归因只能删除 source-sample-only 假设，不能升级为候选或验证")
        mismatches = [row for row in evaluation["exact_command_pairs"]
                      if row["source_sample_onset_differs"]]
        expect(len(evaluation["pulse_transition_brackets"]) == 20 and
               len(evaluation["exact_command_pairs"]) == 10 and
               {(row["amplitude_counts"], row["command_dx_counts"])
                for row in mismatches} == {(13, -13), (8, 8)} and
               all(row["completion_relative_brackets_overlap"]
                   for row in evaluation["exact_command_pairs"]) and
               evaluation[
                   "all_failed_model_max_comparisons_precede_transition"],
               "必须以完整 pulse、exact signed-command pair 和失配位置归因")

        report_path = root / "command-report.json"
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["result"]["events"][0]["returned_at_steady_ns"] += 1
        write_json(report_path, report)
        expect_cli_failure(module, [
            "--run-directory", str(root.resolve()),
            "--command-magnitude-analysis", str(analysis_path.resolve()),
            "--output", str((root / "tampered.json").resolve()),
        ], "command report 身份漂移必须 fail closed")

    with tempfile.TemporaryDirectory(
            prefix="xen-completion-phase-nonoverlap-") as temporary:
        root = pathlib.Path(temporary)
        analysis_path = build_fixture(root, delayed_completion_ms=1.0)
        output_path = root / "nonoverlap.json"
        expect(module.main([
                   "--run-directory", str(root.resolve()),
                   "--command-magnitude-analysis", str(analysis_path.resolve()),
                   "--output", str(output_path.resolve()),
               ]) == 0 and
               json.loads(output_path.read_text(encoding="utf-8"))["status"] ==
                   "TIMING_ATTRIBUTION_UNRESOLVED",
               "exact-command pair 的完成时刻区间不重叠时不得归因")

    with tempfile.TemporaryDirectory(
            prefix="xen-completion-phase-uniform-") as temporary:
        root = pathlib.Path(temporary)
        analysis_path = build_fixture(root, uniform_onset=True)
        output_path = root / "uniform.json"
        expect(module.main([
                   "--run-directory", str(root.resolve()),
                   "--command-magnitude-analysis", str(analysis_path.resolve()),
                   "--output", str(output_path.resolve()),
               ]) == 0 and
               json.loads(output_path.read_text(encoding="utf-8"))["status"] ==
                   "TIMING_ATTRIBUTION_UNRESOLVED",
               "没有 source-sample onset 反例时不得制造 timing 归因")

    negative_cases = (
        ("mixed-session", "report_session"),
        ("exposure-semantic", "frame_basis"),
        ("event-timestamp-shift", "event_timestamp"),
        ("duplicate-frame", "frame_index"),
    )
    for prefix, mutation in negative_cases:
        with tempfile.TemporaryDirectory(
                prefix=f"xen-completion-phase-{prefix}-") as temporary:
            root = pathlib.Path(temporary)
            analysis_path = build_fixture(root)
            report_path = root / "command-report.json"
            manifest_path = root / "pixel-evidence" / "manifest.json"
            report = json.loads(report_path.read_text(encoding="utf-8"))
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            if mutation == "report_session":
                report["result"]["events"][0]["source_clock_session_id"] = \
                    "different-session"
                write_json(report_path, report)
            elif mutation == "frame_basis":
                manifest["frames"][0]["source_time_basis"] = "EXPOSURE"
                write_json(manifest_path, manifest)
            elif mutation == "event_timestamp":
                report["result"]["events"][0]["source_timestamp"] += 999_999
                write_json(report_path, report)
            else:
                manifest["frames"][1]["index"] = 0
                write_json(manifest_path, manifest)
            rebind_analysis(root, analysis_path)
            expect_cli_failure(module, [
                "--run-directory", str(root.resolve()),
                "--command-magnitude-analysis", str(analysis_path.resolve()),
                "--output", str((root / "invalid.json").resolve()),
            ], f"{prefix} 阴性对照必须 fail closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
