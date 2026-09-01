import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile

import cv2
import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "analyze_mouse_effect_probe_pixels.py"
SPEC = importlib.util.spec_from_file_location("probe_physical_analysis", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 mouse effect probe pixel analyzer")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _write_json(path: pathlib.Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _make_fixture(root: pathlib.Path) -> argparse.Namespace:
    frame_root = root / "frames"
    frame_root.mkdir(parents=True)
    rng = np.random.default_rng(20260901)
    base = rng.integers(0, 256, (128, 256, 3), dtype=np.uint8)
    pulse_samples = {4: 1, 10: -1, 16: -1, 22: 1}
    shifts = [0] * 6 + [-1] * 6 + [0] * 6 + [1] * 6 + [0] * 2
    source_start_ns = 10_000_000_000
    source_period_ns = 4_000_000
    sequence_sha256 = "a" * 64
    binding_sha256 = "b" * 64
    samples: list[dict] = []
    events: list[dict] = []
    frames: list[dict] = []
    cumulative_x = 0

    for index, shift in enumerate(shifts):
        bgr = np.roll(base, shift=shift, axis=1)
        frame_path = frame_root / f"{index:06d}.png"
        expect(cv2.imwrite(str(frame_path), bgr), "测试 PNG 写入失败")
        source_time_ns = source_start_ns + index * source_period_ns
        source_timestamp = 1_000_000 + index
        dx_counts = pulse_samples.get(index, 0)
        if index < 4:
            phase = "baseline"
            block_id = 0
        elif 4 <= index <= 13:
            phase = "pulse" if index in pulse_samples else "response"
            block_id = 1
        elif 16 <= index <= 25:
            phase = "pulse" if index in pulse_samples else "response"
            block_id = 2
        else:
            phase = "guard"
            block_id = 0
        samples.append(
            {
                "sample_index": index,
                "block_id": block_id,
                "phase": phase,
                "dx_counts": dx_counts,
                "dy_counts": 0,
            }
        )
        issued_ns = source_time_ns + 1_000_000 if dx_counts else 0
        if dx_counts:
            cumulative_x += dx_counts
        events.append(
            {
                "run_uuid": "fixture-run",
                "activation_epoch": 1,
                "block_id": block_id,
                "sequence_sha256": sequence_sha256,
                "sample_index": index,
                "source_frame_sequence": index,
                "source_timestamp": source_timestamp,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_clock_status": "VALID",
                "source_clock_session_id": "fixture-clock",
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
                "scheduled_at_steady_ns": source_time_ns + 999_000,
                "issued_at_steady_ns": issued_ns,
                "nominal_dx_counts": dx_counts,
                "nominal_dy_counts": 0,
                "requested_dx_counts": dx_counts,
                "requested_dy_counts": 0,
                "safety_allowed": True,
                "dispatch_attempted": dx_counts != 0,
                "backend_succeeded": dx_counts != 0,
                "backend_completed_at_steady_ns": issued_ns + 300_000 if dx_counts else 0,
                "protocol_ack_received": dx_counts != 0,
                "protocol_ack_received_at_steady_ns": issued_ns + 299_000 if dx_counts else 0,
                "returned_at_steady_ns": issued_ns + 301_000 if dx_counts else 0,
                "mouse_status": "READY",
                "cumulative_requested_x_counts": cumulative_x,
                "cumulative_backend_completed_x_counts": cumulative_x,
                "stop_reason": "none",
            }
        )
        frames.append(
            {
                "index": index,
                "file": f"frames/{index:06d}.png",
                "width": 256,
                "height": 128,
                "bgr_sha256": hashlib.sha256(bgr.tobytes()).hexdigest(),
                "source_timestamp": source_timestamp,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_time_timing_valid": True,
                "source_clock_status": "VALID",
                "source_clock_session_id": "fixture-clock",
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
            }
        )

    sequence = {
        "schema": 1,
        "profile": "sparse_pulse_a",
        "request": {
            "baseline_sample_count": 4,
            "response_sample_count": 3,
            "guard_sample_count": 2,
        },
        "blocks": [
            {
                "block_id": 1,
                "first_sample_index": 4,
                "sample_count": 10,
                "first_pulse_dx_counts": 1,
                "second_pulse_dx_counts": -1,
            },
            {
                "block_id": 2,
                "first_sample_index": 16,
                "sample_count": 10,
                "first_pulse_dx_counts": -1,
                "second_pulse_dx_counts": 1,
            },
        ],
        "samples": samples,
        "summary": {"net_x_counts": 0, "max_abs_prefix_x_counts": 1},
        "sequence_sha256": sequence_sha256,
    }
    report = {
        "schema": 1,
        "evidence_type": "mouse_effect_probe_command_report",
        "profile": "sparse_pulse_a",
        "run_uuid": "fixture-run",
        "activation_epoch": 1,
        "dispatch_mode": "physical_a",
        "sequence_sha256": sequence_sha256,
        "binding": {
            "probe_binding_sha256": binding_sha256,
            "sidecar_run_uuid": "fixture-run",
            "capture_source_name": "fixture",
        },
        "executor_timebase": {
            "name": "steady_clock_nanoseconds_since_epoch",
            "ticks_per_second": 1_000_000_000,
        },
        "result": {
            "state": "completed",
            "stop_reason": "normal_completion",
            "complete": True,
            "consumed_sample_count": len(samples),
            "cumulative_requested_x_counts": 0,
            "cumulative_backend_completed_x_counts": 0,
            "events": events,
        },
        "report_sha256": "c" * 64,
    }
    manifest = {
        "schema_version": 1,
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_source_name": "fixture",
        "recorded_frame_count": len(frames),
        "requested_frame_count": len(frames),
        "source_binding": {"file": "source-binding.json", "sha256": binding_sha256},
        "frames": frames,
    }
    sequence_path = root / "sequence.json"
    report_path = root / "command-report.json"
    manifest_path = root / "manifest.json"
    _write_json(sequence_path, sequence)
    _write_json(report_path, report)
    _write_json(manifest_path, manifest)
    return argparse.Namespace(
        manifest=manifest_path,
        command_report=report_path,
        sequence=sequence_path,
        left_roi=(16, 16, 80, 96),
        right_roi=(160, 16, 80, 96),
        output=root / "physical-response-analysis.json",
        pairs_csv=root / "physical-response-pairs.csv",
    )


def test_physical_response_recovers_step_direction_and_exact_onset() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-probe-physical-") as directory:
        arguments = _make_fixture(pathlib.Path(directory))
        result, rows = MODULE.analyze_physical(arguments)
        expect(result["status"] == "VALID", "完整 Physical A 合成证据必须有效")
        expect(result["visible_effect_analyzed"] is True,
               "Physical 分析必须明确已经分析可见效果")
        pulses = result["pulse_responses"]
        expect(len(pulses) == 4 and len(rows) > 4,
               "必须恢复四个脉冲及逐 lag 证据")
        expect(all(pulse["onset"]["first_changed_frame_lag"] == 2 for pulse in pulses),
               "完全相同的 lag-1 帧不得被浮点残差误判为 onset")
        expect([pulse["background_direction"] for pulse in pulses] == [
            "left", "right", "right", "left"
        ], "±1 count 必须恢复为成对相反的背景方向")
        expect(all(pulse["stable_step_observed"] for pulse in pulses),
               "首次变化后完全相同的 witness 状态必须识别为稳定 step")
        expect(all(abs(pulse["x_px_per_count"] - 1.0) < 0.1 for pulse in pulses),
               "合成一像素 step 必须恢复为约一 px/count")
        expect(all(pair["exact_witness_return"] for pair in result["paired_closure"]),
               "成对反向脉冲必须回到各自初始 witness 状态")
        expect(result["whole_sequence_closure"]["exact_witness_return"] is True,
               "四脉冲净零序列必须精确回到首个 pulse 前状态")


def test_physical_contract_rejects_missing_ack_y_and_timestamp_mismatch() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-probe-contract-") as directory:
        arguments = _make_fixture(pathlib.Path(directory))
        original_report = json.loads(arguments.command_report.read_text(encoding="utf-8"))
        original_sequence = json.loads(arguments.sequence.read_text(encoding="utf-8"))

        missing_ack = copy.deepcopy(original_report)
        missing_ack["result"]["events"][4]["protocol_ack_received"] = False
        _write_json(arguments.command_report, missing_ack)
        try:
            MODULE.analyze_physical(arguments)
        except ValueError:
            pass
        else:
            raise AssertionError("Physical pulse 缺少协议 ACK 必须拒绝")

        _write_json(arguments.command_report, original_report)
        y_motion = copy.deepcopy(original_sequence)
        y_motion["samples"][4]["dy_counts"] = 1
        _write_json(arguments.sequence, y_motion)
        try:
            MODULE.analyze_physical(arguments)
        except ValueError:
            pass
        else:
            raise AssertionError("Physical A 出现 Y 输入必须拒绝")

        _write_json(arguments.sequence, original_sequence)
        timestamp_mismatch = copy.deepcopy(original_report)
        timestamp_mismatch["result"]["events"][4]["source_timestamp"] = 9_999_999
        _write_json(arguments.command_report, timestamp_mismatch)
        try:
            MODULE.analyze_physical(arguments)
        except ValueError:
            pass
        else:
            raise AssertionError("command event 无同 timestamp sidecar frame 必须拒绝")


if __name__ == "__main__":
    test_physical_response_recovers_step_direction_and_exact_onset()
    test_physical_contract_rejects_missing_ack_y_and_timestamp_mismatch()
    print("Mouse Effect Probe Physical pixel analysis 测试全部通过。")
