#!/usr/bin/env python3
"""验证 composite-phase 原始 acquisition 到 sealed ledger 的生产链。"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

import cv2
import numpy as np


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(
        value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def seal(value: dict[str, Any], field: str) -> None:
    value.pop(field, None)
    value[field] = canonical_sha256(value)


def seal_report_native(value: dict[str, Any], path: pathlib.Path,
                       verifier: pathlib.Path) -> None:
    value.pop("report_sha256", None)
    write_json(path, value)
    calculated = subprocess.run([
        str(verifier), "--report-semantic-sha256", str(path),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    digest = calculated.stdout.strip()
    expect(calculated.returncode == 0 and len(digest) == 64,
           f"原生 report semantic 计算失败: {calculated.stderr}")
    value["report_sha256"] = digest
    write_json(path, value)


def load_fixture(path: pathlib.Path) -> Any:
    spec = importlib.util.spec_from_file_location("binder_fixture", path)
    if spec is None or spec.loader is None:
        raise AssertionError("无法加载 binder fixture")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_raw(root: pathlib.Path, fixture: Any,
              binder: pathlib.Path, evaluator: pathlib.Path,
              producer: pathlib.Path,
              verifier: pathlib.Path) -> dict[str, pathlib.Path]:
    plan, _, _ = fixture.build_inputs(binder, evaluator)
    plan["seal"]["producer_file_sha256"] = file_sha256(producer)
    plan["seal"]["report_verifier_file_sha256"] = file_sha256(verifier)
    policy = plan["capture_policy"]
    policy.update({
        "source_geometry": [64, 32],
        "roi_geometry": [0, 0, 64, 32],
        "left_witness_roi": [0, 0, 24, 32],
        "right_witness_roi": [40, 0, 24, 32],
    })
    seal(policy, "semantic_sha256")
    seal(plan, "plan_semantic_sha256")
    plan_path = root / "plan.json"
    write_json(plan_path, plan)

    source_session = "77"
    report_events: list[dict[str, Any]] = []

    def report_event(sample_index: int, source_time: int, dx: int,
                     dispatched: bool, completion: int = 0) -> dict[str, Any]:
        return {
            "run_uuid": plan["run_uuid"],
            "activation_epoch": plan["activation_epoch"],
            "block_id": 0 if sample_index == 0 else (sample_index - 1) // 7 + 1,
            "sequence_sha256": plan["sequence_binding"][
                "sequence_semantic_sha256"],
            "sample_index": sample_index,
            "source_frame_sequence": sample_index + 1,
            "source_timestamp": source_time // 100,
            "source_timestamp_valid": True,
            "source_time_at_steady_ns": source_time,
            "source_time_basis": "NDI_SDK_SUBMISSION",
            "source_clock_status": "VALID",
            "source_clock_session_id": source_session,
            "source_clock_uncertainty_ms": 0.01,
            "source_clock_rtt_ms": 0.004,
            "source_clock_rate": 1.0,
            "source_clock_mapping_age_ms": 1.0,
            "source_clock_sample_count": 8,
            "source_dropped_frames": 0,
            "transport_dropped_frames": 0,
            "transport_invalid_packets": 0,
            "scheduled_at_steady_ns": source_time,
            "issued_at_steady_ns": completion - 100 if dispatched else 0,
            "nominal_dx_counts": dx,
            "nominal_dy_counts": 0,
            "requested_dx_counts": dx if dispatched else 0,
            "requested_dy_counts": 0,
            "safety_allowed": True,
            "dispatch_attempted": dispatched,
            "backend_succeeded": dispatched,
            "backend_completed_at_steady_ns": completion,
            "protocol_ack_received": dispatched,
            "protocol_ack_received_at_steady_ns": (
                completion - 50 if dispatched else 0),
            "returned_at_steady_ns": completion + 100 if dispatched else 0,
            "mouse_status": "connected",
            "cumulative_requested_x_counts": 0,
            "cumulative_backend_completed_x_counts": 0,
            "stop_reason": "none",
        }

    report_events.append(report_event(0, 9_900_000_000, 0, False))
    manifest_frames: list[dict[str, Any]] = []
    schedule_events: list[dict[str, Any]] = []
    frame_root = root / "frames"
    frame_root.mkdir()
    pulse_by_id = {pulse["pulse_id"]: pulse for pulse in plan["pulses"]}
    control_by_id = {
        control["window_id"]: control
        for control in plan["negative_controls"]
    }
    phase_numerator = {"P1_8": 1, "P3_8": 3, "P5_8": 5, "P7_8": 7}
    rng = np.random.default_rng(20260904)
    cumulative_x = 0
    manifest_index = 0
    for ordinal, window_id in enumerate(plan["window_order"]):
        predictor_index = 1 + ordinal * 7
        base_ns = 10_000_000_000 + ordinal * 100_000_000
        pulse = pulse_by_id.get(window_id)
        control = control_by_id.get(window_id)
        phase_cell = pulse["phase_cell"] if pulse is not None else control["phase_cell"]
        dx = pulse["command_dx_counts"] if pulse is not None else 0
        completion_ns = base_ns + phase_numerator[phase_cell] * 1_000_000
        dispatched = pulse is not None
        predictor = report_event(
            predictor_index, base_ns - 8_000_000, dx, dispatched,
            completion_ns if dispatched else 0)
        if dispatched:
            cumulative_x += dx
            predictor["cumulative_requested_x_counts"] = cumulative_x
            predictor["cumulative_backend_completed_x_counts"] = cumulative_x
        report_events.append(predictor)

        marker_ns = completion_ns
        schedule_events.append({
            "window_ordinal": ordinal,
            "window_id": window_id,
            "phase_cell": phase_cell,
            "negative_control": not dispatched,
            "predictor_sample_index": predictor_index,
            "report_event_index": predictor_index,
            "marker_time_steady_ns": marker_ns,
            "marker_width_ns": 100,
            "status": "PHASE_CONFIRMED",
        })

        base_image = rng.integers(0, 256, size=(32, 64, 3), dtype=np.uint8)
        changed = base_image.copy()
        if dispatched:
            shift = 2 if dx > 0 else -2
            changed[:, :24] = np.roll(base_image[:, :24], shift, axis=1)
            changed[:, 40:] = np.roll(base_image[:, 40:], shift, axis=1)
        onset = (3 if phase_numerator[phase_cell] in (1, 3) else 4)
        for sample in range(6):
            sample_index = predictor_index + 1 + sample
            source_time = base_ns + sample * 8_000_000
            response = report_event(sample_index, source_time, 0, False)
            response["cumulative_requested_x_counts"] = cumulative_x
            response["cumulative_backend_completed_x_counts"] = cumulative_x
            report_events.append(response)
            image = changed if dispatched and sample >= onset else base_image
            png_path = frame_root / f"frame-{manifest_index:04d}.png"
            expect(cv2.imwrite(str(png_path), image), "无法写入 fixture PNG")
            manifest_frames.append({
                "index": manifest_index,
                "file": f"frames/{png_path.name}",
                "png_sha256": file_sha256(png_path),
                "bgr_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
                "source_timestamp": response["source_timestamp"],
                "source_timestamp_valid": True,
                "source_clock_session_id": int(source_session),
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_clock_status": "VALID",
                "source_time_timing_valid": True,
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
                "storage": "CPU_BGR",
                "width": 64,
                "height": 32,
                "roi_x": 0,
                "roi_y": 0,
                "source_width": 64,
                "source_height": 32,
            })
            manifest_index += 1

    expect(len(report_events) == 295 and len(manifest_frames) == 252,
           "fixture sample/frame 数量漂移")
    report: dict[str, Any] = {
        "schema": 1,
        "evidence_type":
            "backend_completed_command_to_visible_background_response",
        "profile": "physical_b_composite_phase_calibration",
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "dispatch_mode": "physical_b",
        "sequence_sha256": plan["sequence_binding"][
            "sequence_semantic_sha256"],
        "binding": {
            "probe_binding_sha256": "1" * 64,
            "sidecar_run_uuid": plan["run_uuid"],
            "capture_source_name": policy["source_name"],
        },
        "executor_timebase": {
            "name": "steady_clock_nanoseconds_since_epoch",
            "ticks_per_second": 1_000_000_000,
        },
        "result": {
            "state": "completed",
            "stop_reason": "normal_completion",
            "complete": True,
            "consumed_sample_count": 295,
            "cumulative_requested_x_counts": 0,
            "cumulative_backend_completed_x_counts": 0,
            "events": report_events,
        },
    }
    report_path = root / "command-report.json"
    seal_report_native(report, report_path, verifier)

    safety = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_safety_monitor_ledger",
        "physical_output_capability": False,
        "run_uuid": plan["run_uuid"],
        "input_backend": "kmbox_net",
        "probe_stop_reason": "normal_completion",
        "terminal_decision": "ready",
        "recording_failed": False,
        "dropped_observation_count": 0,
        "observations": [],
        "monitor_packet_recording_failed": False,
        "dropped_monitor_packet_count": 0,
        "monitor_packets": [],
    }
    safety_path = root / "safety-ledger.json"
    write_json(safety_path, safety)
    schedule: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_raw_schedule_ledger",
        "status": "ACQUISITION_COMPLETE",
        "ledger_physical_output_capability": False,
        "ledger_physical_dispatch_count": 0,
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "composite_plan_file_sha256": file_sha256(plan_path),
        "probe_binding_sha256": "1" * 64,
        "sequence_semantic_sha256": plan["sequence_binding"][
            "sequence_semantic_sha256"],
        "command_report_file_sha256": file_sha256(report_path),
        "safety_ledger_file_sha256": file_sha256(safety_path),
        "scheduler_clock": {
            "clock_kind": "WINDOWS_QPC",
            "clock_session_id": f"{plan['run_uuid']}-qpc",
            "frequency_hz": 10_000_000,
            "producer_process_id": 4242,
        },
        "timer_mode": "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL",
        "plan_accepted_at_qpc": 10_000_000,
        "acquisition_started_at_qpc": 20_000_000,
        "acquisition_finished_at_qpc": 30_000_000,
        "revealed_at_qpc": 30_000_000,
        "active_wait_total_ns": 1_000_000,
        "source_dispatch_count": 38,
        "events": schedule_events,
    }
    seal(schedule, "ledger_semantic_sha256")
    schedule_path = root / "composite-schedule-ledger.json"
    write_json(schedule_path, schedule)
    manifest = {
        "schema_version": 1,
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_backend": "NDI",
        "capture_source_name": policy["source_name"],
        "capture_config": {
            "frame_layout": "BGRX",
            "source_width": 64,
            "source_height": 32,
            "roi_width": 64,
            "roi_height": 32,
            "center_roi": False,
            "roi_x": 0,
            "roi_y": 0,
        },
        "requested_frame_count": 252,
        "recorded_frame_count": 252,
        "frames": manifest_frames,
    }
    manifest_path = root / "manifest.json"
    write_json(manifest_path, manifest)
    assessment = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_human_assessment",
        "status": "CONFIRMED",
        "run_uuid": plan["run_uuid"],
        "activation_epoch": plan["activation_epoch"],
        "scope_id": plan["scope_id"],
        "manual_mouse_or_wasd": False,
        "scene_cut_or_occlusion": False,
        "abnormal_or_emergency_stop": False,
        "observation_file_sha256": "2" * 64,
    }
    seal(assessment, "assessment_semantic_sha256")
    assessment_path = root / "human-assessment.json"
    write_json(assessment_path, assessment)
    return {
        "plan": plan_path,
        "report": report_path,
        "schedule": schedule_path,
        "safety": safety_path,
        "manifest": manifest_path,
        "assessment": assessment_path,
        "verifier": verifier,
    }


def invoke(producer: pathlib.Path, inputs: dict[str, pathlib.Path],
           root: pathlib.Path, stem: str) -> tuple[
               subprocess.CompletedProcess[str], pathlib.Path, pathlib.Path]:
    capture = root / f"{stem}-capture.json"
    commands = root / f"{stem}-commands.json"
    completed = subprocess.run([
        sys.executable, str(producer),
        "--plan", str(inputs["plan"]),
        "--report", str(inputs["report"]),
        "--schedule", str(inputs["schedule"]),
        "--safety", str(inputs["safety"]),
        "--manifest", str(inputs["manifest"]),
        "--human-assessment", str(inputs["assessment"]),
        "--report-verifier", str(inputs["verifier"]),
        "--capture-output", str(capture),
        "--command-output", str(commands),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    return completed, capture, commands


def update_schedule_for_files(inputs: dict[str, pathlib.Path]) -> None:
    schedule = json.loads(inputs["schedule"].read_text(encoding="utf-8"))
    schedule["command_report_file_sha256"] = file_sha256(inputs["report"])
    schedule["safety_ledger_file_sha256"] = file_sha256(inputs["safety"])
    seal(schedule, "ledger_semantic_sha256")
    write_json(inputs["schedule"], schedule)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--binder", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--report-verifier", required=True,
                        type=pathlib.Path)
    options = parser.parse_args()
    producer = options.producer.resolve()
    binder = options.binder.resolve()
    evaluator = options.evaluator.resolve()
    verifier = options.report_verifier.resolve()
    expect(all(path.is_file() for path in
               (producer, binder, evaluator, verifier)),
           "producer/binder/evaluator/report verifier 缺失")
    fixture = load_fixture(pathlib.Path(__file__).with_name(
        "mouse_effect_probe_b_composite_phase_binder_tests.py"))

    with tempfile.TemporaryDirectory(
            prefix="xen-composite-phase-producer-") as temporary:
        root = pathlib.Path(temporary)
        inputs = build_raw(
            root, fixture, binder, evaluator, producer, verifier)
        completed, capture, commands = invoke(
            producer, inputs, root, "positive")
        expect(completed.returncode == 0 and capture.is_file() and
               commands.is_file(), f"producer 正向链失败: {completed.stderr}")
        evidence = root / "evidence.json"
        bound = subprocess.run([
            sys.executable, str(binder), "--plan", str(inputs["plan"]),
            "--capture", str(capture), "--commands", str(commands),
            "--output", str(evidence),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(bound.returncode == 0 and evidence.is_file(),
               f"producer 输出无法由 binder 消费: {bound.stderr}")
        evaluation = root / "evaluation.json"
        evaluated = subprocess.run([
            sys.executable, str(evaluator), "--evidence", str(evidence),
            "--output", str(evaluation),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        verdict = json.loads(evaluation.read_text(encoding="utf-8"))
        expect(evaluated.returncode == 0 and
               verdict["status"] == "READY_FOR_SEALED_PHASE_VALIDATION",
               f"完整合成链未形成预期 verdict: {evaluated.stderr} {verdict}")

        report = json.loads(inputs["report"].read_text(encoding="utf-8"))
        report["binding"]["probe_binding_sha256"] = "3" * 64
        write_json(inputs["report"], report)
        update_schedule_for_files(inputs)
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "report-seal-drift")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "report 内部 semantic seal 漂移必须 fail closed")

        # 恢复 report 后只篡改 sidecar capture geometry；文件链本身仍自洽。
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)

        report["executor_timebase"]["ticks_per_second"] = 1_000_001
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "executor-timebase-drift")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "report completion steady-clock 合同漂移必须 fail closed")
        report["executor_timebase"]["ticks_per_second"] = 1_000_000_000
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)

        report["result"]["events"][0]["source_timestamp_valid"] = False
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "invalid-source-timestamp")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "report source timestamp validity 漂移必须 fail closed")
        report["result"]["events"][0]["source_timestamp_valid"] = True
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)

        report["result"]["events"][0]["source_clock_rate"] = 1.02
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "source-clock-rate-outside-contract")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "source clock rate 超出 ClockSync 映射合同必须 fail closed")
        report["result"]["events"][0]["source_clock_rate"] = 1.0
        seal_report_native(report, inputs["report"], verifier)
        update_schedule_for_files(inputs)

        manifest = json.loads(inputs["manifest"].read_text(encoding="utf-8"))
        manifest["capture_config"]["roi_width"] = 63
        write_json(inputs["manifest"], manifest)
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "manifest-geometry-drift")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "NDI manifest geometry 与冻结 capture policy 漂移必须拒绝")

        manifest["capture_config"]["roi_width"] = 64
        write_json(inputs["manifest"], manifest)
        first_png = root / manifest["frames"][0]["file"]
        first_png.write_bytes(first_png.read_bytes() + b"drift")
        rejected, bad_capture, bad_commands = invoke(
            producer, inputs, root, "png-drift")
        expect(rejected.returncode == 2 and not bad_capture.exists() and
               not bad_commands.exists(),
               "PNG 文件 SHA 漂移必须拒绝且不发布半份 ledger")

        collision_capture = root / "collision-capture.json"
        collision_capture.write_text("keep\n", encoding="utf-8")
        collision_commands = root / "collision-commands.json"
        collision = subprocess.run([
            sys.executable, str(producer),
            "--plan", str(inputs["plan"]),
            "--report", str(inputs["report"]),
            "--schedule", str(inputs["schedule"]),
            "--safety", str(inputs["safety"]),
            "--manifest", str(inputs["manifest"]),
            "--human-assessment", str(inputs["assessment"]),
            "--report-verifier", str(inputs["verifier"]),
            "--capture-output", str(collision_capture),
            "--command-output", str(collision_commands),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(collision.returncode == 2 and
               collision_capture.read_text(encoding="utf-8") == "keep\n" and
               not collision_commands.exists(),
               "输出碰撞不得覆盖或发布另一半 ledger")

    print("mouse effect probe B composite phase producer tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
