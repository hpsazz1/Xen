import argparse
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
SPEC = importlib.util.spec_from_file_location("probe_a2_physical_analysis", MODULE_PATH)
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
    rng = np.random.default_rng(20260902)
    base = rng.integers(0, 256, (128, 256, 3), dtype=np.uint8)
    sequence_sha256 = "d" * 64
    binding_sha256 = "e" * 64
    source_start_ns = 20_000_000_000
    source_period_ns = 4_000_000
    first_directions = [1, -1, -1, 1]
    samples: list[dict] = []
    blocks: list[dict] = []

    for _ in range(4):
        samples.append(
            {
                "sample_index": len(samples),
                "block_id": 0,
                "phase": "baseline",
                "dx_counts": 0,
                "dy_counts": 0,
            }
        )
    for block_id, first_direction in enumerate(first_directions, start=1):
        first_sample_index = len(samples)
        block_samples: list[tuple[str, int]] = (
            [("guard", 0)] * 2
            + [("pulse", first_direction)]
            + [("response", 0)] * 3
            + [("pulse", -first_direction)]
            + [("response", 0)] * 3
            + [("guard", 0)] * 2
        )
        for phase, dx_counts in block_samples:
            samples.append(
                {
                    "sample_index": len(samples),
                    "block_id": block_id,
                    "phase": phase,
                    "dx_counts": dx_counts,
                    "dy_counts": 0,
                }
            )
        blocks.append(
            {
                "block_id": block_id,
                "first_sample_index": first_sample_index,
                "sample_count": len(block_samples),
                "first_pulse_dx_counts": first_direction,
                "second_pulse_dx_counts": -first_direction,
            }
        )

    events: list[dict] = []
    frames: list[dict] = []
    cumulative_x = 0
    for index, sample in enumerate(samples):
        dx_counts = int(sample["dx_counts"])
        bgr = np.roll(base, shift=-cumulative_x, axis=1)
        frame_path = frame_root / f"{index:06d}.png"
        expect(cv2.imwrite(str(frame_path), bgr), "测试 PNG 写入失败")
        source_time_ns = source_start_ns + index * source_period_ns
        source_timestamp = 2_000_000 + index
        issued_ns = source_time_ns + 1_000_000 if dx_counts else 0
        if dx_counts:
            cumulative_x += dx_counts
        events.append(
            {
                "run_uuid": "a2-fixture-run",
                "activation_epoch": 2,
                "block_id": int(sample["block_id"]),
                "sequence_sha256": sequence_sha256,
                "sample_index": index,
                "source_frame_sequence": index,
                "source_timestamp": source_timestamp,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_clock_status": "VALID",
                "source_clock_session_id": "a2-fixture-clock",
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
                "png_sha256": hashlib.sha256(frame_path.read_bytes()).hexdigest(),
                "source_timestamp": source_timestamp,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_time_timing_valid": True,
                "source_clock_status": "VALID",
                "source_clock_session_id": "a2-fixture-clock",
                "source_clock_uncertainty_ms": 0.05,
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
            }
        )

    sequence = {
        "schema": 2,
        "profile": "dependency_calibration_a2_p_cal",
        "request": {
            "baseline_sample_count": 4,
            "response_sample_count": 3,
            "guard_sample_count": 2,
            "block_count": 4,
            "run_role": "p_cal",
        },
        "blocks": blocks,
        "samples": samples,
        "summary": {"net_x_counts": 0, "max_abs_prefix_x_counts": 1},
        "sequence_sha256": sequence_sha256,
    }
    report = {
        "schema": 1,
        "evidence_type": "mouse_effect_probe_command_report",
        "profile": sequence["profile"],
        "run_uuid": "a2-fixture-run",
        "activation_epoch": 2,
        "dispatch_mode": "physical_a",
        "sequence_sha256": sequence_sha256,
        "binding": {
            "probe_binding_sha256": binding_sha256,
            "sidecar_run_uuid": "a2-fixture-run",
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
        "report_sha256": "f" * 64,
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
        output=root / "a2-physical-response.json",
        pairs_csv=root / "a2-physical-response.csv",
    )


def _remove_manifest_frame(arguments: argparse.Namespace, sample_index: int) -> None:
    manifest = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    source_timestamp = 2_000_000 + sample_index
    frames = [
        frame for frame in manifest["frames"]
        if int(frame["source_timestamp"]) != source_timestamp
    ]
    expect(len(frames) + 1 == len(manifest["frames"]),
           "测试必须精确移除一个 manifest frame")
    for index, frame in enumerate(frames):
        frame["index"] = index
    manifest["frames"] = frames
    manifest["recorded_frame_count"] = len(frames)
    manifest["requested_frame_count"] = len(frames)
    _write_json(arguments.manifest, manifest)


def test_a2_physical_response_preserves_blocks_without_claiming_dependency_green() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-probe-a2-physical-") as directory:
        arguments = _make_fixture(pathlib.Path(directory))
        result, rows = MODULE.analyze_physical(arguments)
        expect(result["status"] == "VALID", "A2 原始响应证据必须有效")
        expect(result["profile"] == "dependency_calibration_a2_p_cal",
               "A2 profile 必须原样绑定")
        expect(result["run_role"] == "p-cal", "A2 run role 必须显式记录")
        expect(len(result["pulse_responses"]) == 8 and len(rows) > 8,
               "四个完整 block 必须产生八个 transition 响应")
        expect(len(result["paired_closure"]) == 4,
               "A2 每个 block 必须独立核对成对回零")
        expect(all(pair["exact_witness_return"] for pair in result["paired_closure"]),
               "每个 A2 block 都必须回到自己的 anchor")
        expect(result["a2_dependency_gate_claimed"] is False,
               "单个原始 Physical Run 不得冒充五项依赖已绿")
        expect(result["source_mapping_uncertainty_ms"]["max"] == 0.05,
               "A2 必须保留 source mapping uncertainty，而不是乘固定速度")


def test_a2_allows_only_unmatched_zero_baseline_event() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-probe-a2-baseline-gap-") as directory:
        arguments = _make_fixture(pathlib.Path(directory))
        _remove_manifest_frame(arguments, 1)
        result, rows = MODULE.analyze_physical(arguments)
        expect(result["status"] == "VALID" and len(rows) > 8,
               "单个零命令 baseline 非交集帧不得废弃完整 pulse/response 证据")
        expect(result["matched_event_frame_count"] == 51,
               "raw analyzer 必须显式报告 event/frame 交集数量")
        expect(result["source_timestamp_unmatched_baseline_event_count"] == 1,
               "raw analyzer 必须显式报告 baseline 非交集数量")
        expect(result["source_timestamp_unmatched_baseline_event_limit"] == 1,
               "raw analyzer 必须固化单个 interior baseline 缺口上限")
        expect(result["zero_input_baseline"]["expected_event_count"] == 4 and
               result["zero_input_baseline"]["matched_frame_count"] == 3,
               "baseline 统计必须区分设计事件数与实际交集帧数")
        expect(len(result["pulse_responses"]) == 8,
               "baseline 缺口不得改变八个已完整匹配的 pulse 响应")

    for missing_phase, sample_index in (
        ("baseline-anchor", 0),
        ("pulse", 6),
        ("response", 7),
    ):
        with tempfile.TemporaryDirectory(
            prefix=f"xen-probe-a2-{missing_phase}-gap-"
        ) as directory:
            arguments = _make_fixture(pathlib.Path(directory))
            _remove_manifest_frame(arguments, sample_index)
            try:
                MODULE.analyze_physical(arguments)
            except ValueError:
                pass
            else:
                raise AssertionError(
                    f"{missing_phase} event 无同 timestamp sidecar frame 必须拒绝"
                )

    with tempfile.TemporaryDirectory(
        prefix="xen-probe-a2-multiple-baseline-gaps-"
    ) as directory:
        arguments = _make_fixture(pathlib.Path(directory))
        _remove_manifest_frame(arguments, 1)
        _remove_manifest_frame(arguments, 2)
        try:
            MODULE.analyze_physical(arguments)
        except ValueError:
            pass
        else:
            raise AssertionError("多个 interior baseline 非交集事件必须拒绝")


if __name__ == "__main__":
    test_a2_physical_response_preserves_blocks_without_claiming_dependency_green()
    test_a2_allows_only_unmatched_zero_baseline_event()
    print("Mouse Effect Probe A2 Physical pixel analysis 测试全部通过。")
