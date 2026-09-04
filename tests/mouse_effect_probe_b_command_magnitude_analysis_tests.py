#!/usr/bin/env python3
"""验证多幅值 Primary 分析只拟合 estimation，并以 whole pulse 删除。"""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import math
import pathlib
import tempfile
from typing import Any


RESPONSE_ROWS = 49
SOURCE_SESSION = "5808209070696154636"


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_module(path: pathlib.Path) -> Any:
    spec = importlib.util.spec_from_file_location(
        "mouse_effect_probe_b_command_magnitude_analysis", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("无法加载 analyzer module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def curve(gain: float, command: int) -> list[float]:
    return [
        0.0 if offset < 4 else gain * command
        for offset in range(RESPONSE_ROWS)
    ]


def make_pulses(confirmation_gain: float = -0.5) -> list[dict[str, Any]]:
    pulses: list[dict[str, Any]] = []
    amplitudes = (1, 1, 4, 4, 13, 13, 2, 2, 8, 8)
    for block_index, amplitude in enumerate(amplitudes):
        polarity = 1 if block_index % 2 == 0 else -1
        role = "estimation" if block_index < 6 else "confirmation"
        gain = -0.5 if role == "estimation" else confirmation_gain
        for pulse_ordinal, command in enumerate(
                (polarity * amplitude, -polarity * amplitude), 1):
            pulses.append({
                "block_id": block_index + 1,
                "pair_index": block_index // 2 + 1,
                "role": role,
                "polarity": "normal" if polarity > 0 else "inverted",
                "amplitude_counts": amplitude,
                "pulse_ordinal": pulse_ordinal,
                "command_dx_counts": command,
                "left_dx_px": curve(gain, command),
                "right_dx_px": curve(gain, command),
            })
    return pulses


def make_sequence() -> dict[str, Any]:
    amplitudes = (1, 1, 4, 4, 13, 13, 2, 2, 8, 8)
    roles = ("estimation",) * 6 + ("confirmation",) * 4
    polarities = ("normal", "inverted") * 5
    samples: list[dict[str, Any]] = []

    def append(block_id: int, phase: str, dx: int) -> None:
        samples.append({
            "sample_index": len(samples),
            "block_id": block_id,
            "phase": phase,
            "dx_counts": dx,
            "dy_counts": 0,
        })

    for _ in range(64):
        append(0, "baseline", 0)
    blocks: list[dict[str, Any]] = []
    for block_index, amplitude in enumerate(amplitudes):
        block_id = block_index + 1
        sign = 1 if polarities[block_index] == "normal" else -1
        blocks.append({
            "block_id": block_id,
            "pair_index": block_index // 2 + 1,
            "role": roles[block_index],
            "polarity": polarities[block_index],
            "amplitude_counts": amplitude,
            "first_sample_index": len(samples),
            "sample_count": 162,
        })
        for _ in range(32):
            append(block_id, "guard", 0)
        append(block_id, "pulse", sign * amplitude)
        for _ in range(48):
            append(block_id, "response", 0)
        append(block_id, "pulse", -sign * amplitude)
        for _ in range(48):
            append(block_id, "response", 0)
        for _ in range(32):
            append(block_id, "guard", 0)
    return {
        "schema": 6,
        "profile": "physical_b_command_magnitude_primary",
        "sequence_sha256": "0" * 64,
        "request": {
            "baseline_sample_count": 64,
            "response_sample_count": 48,
            "guard_sample_count": 32,
            "run_role": "primary",
        },
        "summary": {"net_x_counts": 0, "max_abs_prefix_x_counts": 13},
        "samples": samples,
        "blocks": blocks,
    }


def make_recorded_run(module: Any, root: pathlib.Path) -> None:
    profile = "physical_b_command_magnitude_primary"
    run_uuid = "12345678-1234-4234-8234-123456789abc"
    activation_epoch = 123456789
    scope_id = "a" * 64
    source_session = SOURCE_SESSION
    sequence = make_sequence()
    sequence_path = root / "sequence.json"
    write_json(sequence_path, sequence)

    binding = {
        "schema_version": 4,
        "evidence_type": "mouse_effect_probe_binding",
        "experiment": profile,
        "run_uuid": run_uuid,
        "activation_epoch": activation_epoch,
        "dispatch_mode": "physical_b",
        "profile": profile,
        "run_role": "primary",
        "scope_id": scope_id,
        "sequence_sha256": sequence["sequence_sha256"],
        "sidecar_physical_output_capability": False,
        "normal_aim_output_required": False,
        "dy_counts_required": 0,
        "max_abs_pulse_counts": 13,
        "max_abs_prefix_x_counts": 13,
        "validation_used_for_refit": False,
        "new_production_gain_claimed": False,
    }
    binding_path = root / "probe-binding.json"
    write_json(binding_path, binding)

    a2 = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_magnitude_domain_analysis",
        "status": "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "analysis_contract": {
            "model": {"frozen_f1_gains": {"left": -0.39, "right": -0.41}}
        },
        "evaluation": {"new_production_gain_claimed": False},
    }
    a2_path = root / "inputs" / "a2-magnitude-analysis.json"
    write_json(a2_path, a2)
    b0 = {
        "status": "BASELINE_REPLAY_FIDELITY_INVALID",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
    }
    b0_path = root / "inputs" / "b0-fidelity-evaluation.json"
    write_json(b0_path, b0)

    safety_path = root / "safety-ledger.json"
    lifecycle_path = root / "sidecar-lifecycle.json"
    write_json(safety_path, {"terminal_decision": "continue"})
    write_json(lifecycle_path, {"status": "EXITED"})

    frames_root = root / "pixel-evidence" / "frames"
    frames_root.mkdir(parents=True)
    image_path = frames_root / "shared.png"
    random = module.np.random.default_rng(20260904)
    original = random.integers(
        0, 256, size=(320, 320, 3), dtype=module.np.uint8)
    if not module.cv2.imwrite(str(image_path), original):
        raise RuntimeError("failed to write synthetic PNG")
    decoded = module.cv2.imread(str(image_path), module.cv2.IMREAD_COLOR)
    if decoded is None:
        raise RuntimeError("failed to decode synthetic PNG")
    png_sha256 = file_sha256(image_path)
    bgr_sha256 = hashlib.sha256(
        module.np.ascontiguousarray(decoded).tobytes()).hexdigest()
    frame_count = 2200
    frames = []
    for index in range(frame_count):
        frames.append({
            "index": index,
            "file": "frames/shared.png",
            "png_sha256": png_sha256,
            "bgr_sha256": bgr_sha256,
            "source_timestamp": 100_000 + index,
            "source_timestamp_valid": True,
            "source_time_timing_valid": True,
            "source_clock_status": "VALID",
            "source_clock_session_id": source_session,
            "source_dropped_frames": 0,
            "transport_dropped_frames": 0,
            "transport_invalid_packets": 0,
        })
    manifest = {
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_source_name": "HPSAZZ (Xen-ROI-320)",
        "recorded_frame_count": frame_count,
        "source_binding": {"sha256": file_sha256(binding_path)},
        "frames": frames,
    }
    manifest_path = root / "pixel-evidence" / "manifest.json"
    write_json(manifest_path, manifest)

    cumulative = 0
    events = []
    for index, sample in enumerate(sequence["samples"]):
        command = int(sample["dx_counts"])
        cumulative += command
        nonzero = command != 0
        events.append({
            "sample_index": index,
            "nominal_dx_counts": command,
            "nominal_dy_counts": 0,
            "requested_dx_counts": command,
            "requested_dy_counts": 0,
            "safety_allowed": True,
            "dispatch_attempted": nonzero,
            "backend_succeeded": nonzero,
            "protocol_ack_received": nonzero,
            "cumulative_requested_x_counts": cumulative,
            "cumulative_backend_completed_x_counts": cumulative,
            "source_timestamp": 100_000 + index,
            "source_timestamp_valid": True,
            "source_time_at_steady_ns": 1_000_000_000 + index * 4_166_667,
            "source_clock_status": "VALID",
            "source_clock_session_id": source_session,
            "source_dropped_frames": 0,
            "transport_dropped_frames": 0,
            "transport_invalid_packets": 0,
        })
    report = {
        "run_uuid": run_uuid,
        "dispatch_mode": "physical_b",
        "profile": profile,
        "sequence_sha256": sequence["sequence_sha256"],
        "report_sha256": "b" * 64,
        "result": {
            "complete": True,
            "stop_reason": "normal_completion",
            "cumulative_requested_x_counts": 0,
            "cumulative_backend_completed_x_counts": 0,
            "events": events,
        },
    }
    report_path = root / "command-report.json"
    write_json(report_path, report)

    launch = {
        "schema_version": 6,
        "evidence_type": "mouse_effect_probe_b_command_magnitude_launch",
        "status": "RECORDED_UNANALYZED",
        "run_uuid": run_uuid,
        "activation_epoch": activation_epoch,
        "scope_id": scope_id,
        "run_role": "primary",
        "profile": profile,
        "sequence_sha256": sequence["sequence_sha256"],
        "command_event_count": 1684,
        "source_timestamp_matched_event_count": 1684,
        "source_timestamp_unmatched_baseline_event_count": 0,
        "sidecar_frame_count": frame_count,
        "backend_completed_pulse_count": 20,
        "requested_net_x_counts": 0,
        "backend_completed_net_x_counts": 0,
        "command_report_sha256": report["report_sha256"],
        "sidecar_manifest_sha256": file_sha256(manifest_path),
        "safety_ledger_sha256": file_sha256(safety_path),
        "sidecar_lifecycle_sha256": file_sha256(lifecycle_path),
        "png_hash_verified_count": frame_count,
        "visible_effect_analyzed": False,
        "human_observation_received": False,
        "validation_used_for_refit": False,
        "new_production_gain_claimed": False,
    }
    write_json(root / "launch-summary.json", launch)

    def identity(path: pathlib.Path) -> dict[str, Any]:
        return {
            "path": str(path.resolve()),
            "size": path.stat().st_size,
            "sha256": file_sha256(path),
        }

    task = {
        "schema_version": 8,
        "evidence_type": "mouse_effect_probe_b_command_magnitude_task",
        "status": "PREPARED",
        "run_directory": str(root.resolve()),
        "run_uuid": run_uuid,
        "activation_epoch": activation_epoch,
        "scope_id": scope_id,
        "dispatch_mode": "physical_b",
        "profile": profile,
        "run_role": "primary",
        "sequence_sha256": sequence["sequence_sha256"],
        "sequence_sample_count": 1684,
        "expected_nonzero_transition_count": 20,
        "max_abs_prefix_x_counts": 13,
        "physical_output_capability": True,
        "requires_user_frontend_launch": True,
        "physical_output_confirmation":
            "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT",
        "files": {
            "sequence": identity(sequence_path),
            "probe_binding": identity(binding_path),
            "a2_magnitude_analysis": identity(a2_path),
            "b0_fidelity_evaluation": identity(b0_path),
        },
        "capture": {
            "source_name": "HPSAZZ (Xen-ROI-320)",
            "roi_width": 320,
            "roi_height": 320,
        },
        "sidecar": {
            "frames": frame_count,
            "left_witness_roi": "16,48,96,224",
            "right_witness_roi": "208,48,96,224",
        },
        "dynamics_policy": {
            "policy_id": "b-command-magnitude-primary-v1",
            "input_definition":
                "backend_completed_relative_command_dx_counts",
            "primary_estimation_amplitudes": [1, 4, 13],
            "within_run_confirmation_amplitudes": [2, 8],
            "validation_used_for_refit": False,
            "new_production_gain_claimed": False,
            "cross_run_holdout_required_before_candidate": True,
            "fixed_pixel_speed_used_as_gate": False,
        },
        "safety": {
            "zero_y_required": True,
            "max_abs_pulse_counts": 13,
            "max_abs_prefix_x_counts": 13,
            "manual_mouse_motion_or_wasd_forbidden": True,
            "no_runtime_amplitude_or_repetition_change": True,
        },
    }
    write_json(root / "task.json", task)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    module = load_module(arguments.analyzer.resolve())

    sequence = make_sequence()
    module._validate_sequence_payload(sequence)
    hidden_command = copy.deepcopy(sequence)
    hidden_command["samples"][1]["dx_counts"] = 13
    try:
        module._validate_sequence_payload(hidden_command)
    except ValueError:
        pass
    else:
        raise AssertionError("摘要不变但 sample 漂移时必须 fail closed")

    with tempfile.TemporaryDirectory(
            prefix="xen-command-magnitude-analysis-") as temporary:
        recorded_root = pathlib.Path(temporary)
        make_recorded_run(module, recorded_root)
        measured, identity, measured_f1 = module._measure_primary(recorded_root)
        expect(len(measured) == 20 and
               [row["command_dx_counts"] for row in measured[:4]] ==
                   [1, -1, -1, 1] and
               identity["source_clock_session_id"] == SOURCE_SESSION and
               measured_f1 == {"left": -0.39, "right": -0.41},
               "完整 Run fixture 必须闭合 source/event/PNG/pulse 身份")
        output_path = recorded_root / "analysis.json"
        expect(module.main([
                   "--run-directory", str(recorded_root),
                   "--output", str(output_path),
               ]) == 0 and output_path.is_file(),
               "完整 Run fixture 必须可原子发布零物理能力分析")
        published = json.loads(output_path.read_text(encoding="utf-8"))
        expect(published["physical_output_capability"] is False and
               published["physical_dispatch_count"] == 0 and
               published["production_aim_changed"] is False and
               published["new_production_gain_claimed"] is False and
               len(published["pulse_measurements"]) == 20,
               "分析输出不得携带 Physical/生产能力且必须保留 whole pulses")
        manifest_path = recorded_root / "pixel-evidence" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for frame in manifest["frames"]:
            frame["source_clock_session_id"] = int(SOURCE_SESSION)
        write_json(manifest_path, manifest)
        launch_path = recorded_root / "launch-summary.json"
        launch = json.loads(launch_path.read_text(encoding="utf-8"))
        launch["sidecar_manifest_sha256"] = file_sha256(manifest_path)
        write_json(launch_path, launch)
        numeric_session_output = recorded_root / "numeric-session-analysis.json"
        expect(module.main([
                   "--run-directory", str(recorded_root),
                   "--output", str(numeric_session_output),
               ]) == 0 and numeric_session_output.is_file(),
               "同值 source clock session 的字符串/整数 JSON 编码必须闭合")
        numeric_session = json.loads(
            numeric_session_output.read_text(encoding="utf-8"))
        expect(numeric_session["primary"]["source_clock_session_id"] ==
                   SOURCE_SESSION,
               "分析身份必须规范化保存 source clock session")
        write_json(
            recorded_root / "safety-ledger.json",
            {"terminal_decision": "tampered"})
        try:
            module._measure_primary(recorded_root)
        except ValueError:
            pass
        else:
            raise AssertionError("Launch 后 safety ledger 漂移必须 fail closed")

    frozen_f1 = {"left": -0.39, "right": -0.41}
    green = module.evaluate_primary(make_pulses(), frozen_f1)
    expect(green["status"] == "READY_FOR_COMMAND_MAGNITUDE_HOLDOUT" and
           green["physical_output_capability"] is False and
           green["physical_dispatch_count"] == 0 and
           green["production_aim_changed"] is False and
           green["new_production_gain_claimed"] is False and
           green["fit"]["fit_role"] == "estimation_whole_pulses_only" and
           green["fit"]["confirmation_used_for_refit"] is False and
           green["fit"]["left_step_response_px_per_count"][4] == -0.5 and
           green["fit"]["right_step_response_px_per_count"][4] == -0.5,
           "线性合成响应应冻结 Primary model 并只允许进入独立 Holdout")
    expect(len(green["confirmation"]) == 8 and
           all(row["model_strictly_beats_f1_rmse"] and
               row["model_strictly_beats_f1_max"] and
               row["opposite_direction"]
               for row in green["confirmation"]),
           "2/8-count confirmation 必须逐 pulse/双 witness 严格优于 F1")
    json.dumps(green, allow_nan=False)

    failed_confirmation = module.evaluate_primary(
        make_pulses(confirmation_gain=-0.2), frozen_f1)
    expect(failed_confirmation["status"] ==
               "LINEAR_STEP_RESPONSE_DELETED" and
           failed_confirmation["fit"] == green["fit"],
           "confirmation 失败必须删除假设，且不得泄漏回 Primary fit")

    changed_estimation = make_pulses()
    for pulse in changed_estimation:
        if pulse["role"] == "estimation":
            pulse["left_dx_px"] = curve(-0.6, pulse["command_dx_counts"])
    changed = module.evaluate_primary(changed_estimation, frozen_f1)
    expect(math.isclose(
               changed["fit"]["left_step_response_px_per_count"][4], -0.6,
               rel_tol=0.0, abs_tol=1e-12) and
           math.isclose(
               changed["fit"]["right_step_response_px_per_count"][4], -0.5,
               rel_tol=0.0, abs_tol=1e-12),
           "只有 estimation 数据可以改变对应 witness model")

    invalid = make_pulses()[:-1]
    try:
        module.evaluate_primary(invalid, frozen_f1)
    except ValueError:
        pass
    else:
        raise AssertionError("缺失 whole pulse 时必须 fail closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
