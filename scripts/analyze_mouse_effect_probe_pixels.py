#!/usr/bin/env python3
"""分析 Mouse Effect Probe 的左右背景 witness 基线。

本脚本只读取已经发布的 sidecar/command/sequence 证据，不具备 Mouse 输出能力。
phaseCorrelate 的 shift 表示第二帧相对第一帧的平移；response 只按连续质量观测
记录，不在缺少本 Run baseline 时提升为固定通过阈值。
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import pathlib
import sys
from typing import Iterable, Sequence

import cv2
import numpy as np


Roi = tuple[int, int, int, int]


def parse_roi(value: str) -> Roi:
    parts = value.split(",")
    if len(parts) != 4:
        raise ValueError("ROI 必须是 x,y,width,height")
    try:
        x, y, width, height = (int(part) for part in parts)
    except ValueError as exception:
        raise ValueError("ROI 只接受整数") from exception
    if x < 0 or y < 0 or width <= 1 or height <= 1:
        raise ValueError("ROI 原点必须非负且宽高必须大于 1")
    return x, y, width, height


def _overlap(first: Roi, second: Roi) -> bool:
    first_x, first_y, first_width, first_height = first
    second_x, second_y, second_width, second_height = second
    return not (
        first_x + first_width <= second_x
        or second_x + second_width <= first_x
        or first_y + first_height <= second_y
        or second_y + second_height <= first_y
    )


def validate_roi_pair(left: Roi, right: Roi, width: int, height: int) -> None:
    if width <= 0 or height <= 0:
        raise ValueError("图像几何非法")
    for name, roi in (("left", left), ("right", right)):
        x, y, roi_width, roi_height = roi
        if x + roi_width > width or y + roi_height > height:
            raise ValueError(f"{name} witness ROI 超出图像边界")
    if _overlap(left, right):
        raise ValueError("左右 witness ROI 不得重叠")
    if left[0] >= right[0]:
        raise ValueError("left witness 必须位于 right witness 左侧")


def distribution_summary(values: Iterable[float]) -> dict[str, float | int]:
    array = np.asarray(list(values), dtype=np.float64)
    if array.size == 0 or not np.all(np.isfinite(array)):
        raise ValueError("分布样本不能为空且必须全部有限")
    return {
        "count": int(array.size),
        "mean": float(np.mean(array)),
        "min": float(np.min(array)),
        "p50": float(np.percentile(array, 50)),
        "p95": float(np.percentile(array, 95)),
        "p99": float(np.percentile(array, 99)),
        "max": float(np.max(array)),
    }


def measure_translation(
    first: np.ndarray,
    second: np.ndarray,
    window: np.ndarray,
) -> dict[str, float]:
    if (
        first.dtype != np.float32
        or second.dtype != np.float32
        or first.ndim != 2
        or second.ndim != 2
        or first.shape != second.shape
        or window.dtype != np.float32
        or window.shape != first.shape
    ):
        raise ValueError("phaseCorrelate 输入必须是同尺寸 float32 单通道 ROI")
    # OpenCV 在无需 DFT padding 时可能把 window 直接乘入输入 Mat；重复使用
    # pre-command reference 时必须给它新鲜副本，不能让前一次 lag 污染后一次。
    shift, response = cv2.phaseCorrelate(first.copy(), second.copy(), window)
    dx, dy = float(shift[0]), float(shift[1])
    response_value = float(response)
    if not all(math.isfinite(value) for value in (dx, dy, response_value)):
        raise ValueError("phaseCorrelate 返回非有限结果")
    return {"dx_px": dx, "dy_px": dy, "response": response_value}


def _load_json(path: pathlib.Path, description: str) -> dict:
    if not path.is_file():
        raise ValueError(f"{description} 不是普通文件：{path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exception:
        raise ValueError(f"{description} 不是有效 UTF-8 JSON：{path}") from exception
    if not isinstance(value, dict):
        raise ValueError(f"{description} 根值必须是对象")
    return value


def _roi_image(gray: np.ndarray, roi: Roi) -> np.ndarray:
    x, y, width, height = roi
    return np.ascontiguousarray(gray[y : y + height, x : x + width])


def _texture_metrics(image: np.ndarray) -> tuple[float, float]:
    stddev = float(np.std(image, dtype=np.float64))
    gradient_x = cv2.Sobel(image, cv2.CV_32F, 1, 0, ksize=3)
    gradient_y = cv2.Sobel(image, cv2.CV_32F, 0, 1, ksize=3)
    gradient_rms = float(
        np.sqrt(np.mean(gradient_x * gradient_x + gradient_y * gradient_y))
    )
    if not math.isfinite(stddev) or not math.isfinite(gradient_rms):
        raise ValueError("witness texture metric 非有限")
    return stddev, gradient_rms


def _correlation_summary(
    left_values: Sequence[float], right_values: Sequence[float]
) -> dict[str, float | str | None]:
    left = np.asarray(left_values, dtype=np.float64)
    right = np.asarray(right_values, dtype=np.float64)
    if left.size != right.size or left.size == 0:
        raise ValueError("左右 witness correlation 样本不一致")
    left_std = float(np.std(left))
    right_std = float(np.std(right))
    if left_std <= np.finfo(np.float64).eps or right_std <= np.finfo(np.float64).eps:
        return {
            "pearson": None,
            "reason": "ZERO_VARIANCE_BASELINE",
            "left_std": left_std,
            "right_std": right_std,
        }
    return {
        "pearson": float(np.corrcoef(left, right)[0, 1]),
        "reason": "AVAILABLE",
        "left_std": left_std,
        "right_std": right_std,
    }


def _geometry_summary(roi: Roi, width: int, height: int) -> dict[str, int]:
    x, y, roi_width, roi_height = roi
    return {
        "x": x,
        "y": y,
        "width": roi_width,
        "height": roi_height,
        "left_margin_px": x,
        "right_margin_px": width - (x + roi_width),
        "top_margin_px": y,
        "bottom_margin_px": height - (y + roi_height),
    }


def _atomic_write_text(path: pathlib.Path, content: str) -> None:
    if path.exists():
        raise ValueError(f"输出已存在，拒绝覆盖：{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(f"{path.name}.pending-{os.getpid()}")
    if pending.exists():
        raise ValueError(f"临时输出已存在，拒绝覆盖：{pending}")
    try:
        pending.write_text(content, encoding="utf-8", newline="\n")
        os.rename(pending, path)
    finally:
        if pending.exists():
            pending.unlink()


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="分析 Mouse Effect Probe 左右背景 witness 基线或 Physical 响应"
    )
    parser.add_argument(
        "--analysis-mode",
        choices=("output-off-baseline", "physical-response"),
        default="output-off-baseline",
    )
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--command-report", type=pathlib.Path, required=True)
    parser.add_argument("--sequence", type=pathlib.Path, required=True)
    parser.add_argument("--left-roi", type=parse_roi, required=True)
    parser.add_argument("--right-roi", type=parse_roi, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--pairs-csv", type=pathlib.Path, required=True)
    return parser.parse_args()


def analyze(arguments: argparse.Namespace) -> dict:
    manifest = _load_json(arguments.manifest, "sidecar manifest")
    report = _load_json(arguments.command_report, "command report")
    sequence = _load_json(arguments.sequence, "probe sequence")
    if (
        manifest.get("evidence_type") != "output_off_capture"
        or manifest.get("physical_output_capability") is not False
        or report.get("dispatch_mode") != "output_off_rehearsal"
        or report.get("result", {}).get("state") != "completed"
        or report.get("result", {}).get("complete") is not True
        or sequence.get("profile") != "sparse_pulse_a"
        or report.get("sequence_sha256") != sequence.get("sequence_sha256")
    ):
        raise ValueError("输入证据不是同一完整 output-off sparse-pulse A Run")
    frames = manifest.get("frames")
    events = report.get("result", {}).get("events")
    samples = sequence.get("samples")
    if (
        not isinstance(frames, list)
        or not isinstance(events, list)
        or not isinstance(samples, list)
        or len(events) != len(samples)
        or len(events) < 2
    ):
        raise ValueError("sidecar/event/sequence 数组容量不一致")

    frame_by_timestamp: dict[int, dict] = {}
    for frame in frames:
        if not isinstance(frame, dict) or frame.get("source_timestamp_valid") is not True:
            raise ValueError("sidecar frame 缺少 source timestamp")
        timestamp = int(frame["source_timestamp"])
        if timestamp in frame_by_timestamp:
            raise ValueError("sidecar source timestamp 不唯一")
        frame_by_timestamp[timestamp] = frame

    matched: list[tuple[dict, dict, dict]] = []
    for index, (event, sample) in enumerate(zip(events, samples, strict=True)):
        if (
            int(event.get("sample_index", -1)) != index
            or int(sample.get("sample_index", -1)) != index
            or int(event.get("nominal_dx_counts", 0)) != int(sample.get("dx_counts", 0))
            or int(event.get("nominal_dy_counts", 0)) != int(sample.get("dy_counts", 0))
            or event.get("dispatch_attempted") is not False
            or int(event.get("requested_dx_counts", 0)) != 0
            or int(event.get("requested_dy_counts", 0)) != 0
            or event.get("backend_succeeded") is not False
        ):
            raise ValueError("command event 与 output-off nominal/actual 契约不一致")
        timestamp = int(event["source_timestamp"])
        frame = frame_by_timestamp.get(timestamp)
        if frame is None:
            raise ValueError("command event 无同 timestamp sidecar frame")
        matched.append((event, sample, frame))

    pixel_root = arguments.manifest.resolve().parent
    first_path = (pixel_root / str(matched[0][2]["file"])).resolve()
    if not first_path.is_relative_to(pixel_root) or not first_path.is_file():
        raise ValueError("首帧路径逃逸或不存在")
    first_bgr = cv2.imread(str(first_path), cv2.IMREAD_COLOR)
    if first_bgr is None or first_bgr.ndim != 3:
        raise ValueError("无法解码首帧 BGR")
    image_height, image_width = first_bgr.shape[:2]
    validate_roi_pair(arguments.left_roi, arguments.right_roi, image_width, image_height)
    left_window = cv2.createHanningWindow(
        (arguments.left_roi[2], arguments.left_roi[3]), cv2.CV_32F
    )
    right_window = cv2.createHanningWindow(
        (arguments.right_roi[2], arguments.right_roi[3]), cv2.CV_32F
    )

    left_stddevs: list[float] = []
    right_stddevs: list[float] = []
    left_gradient_rms: list[float] = []
    right_gradient_rms: list[float] = []
    pairs: list[dict] = []
    previous_left: np.ndarray | None = None
    previous_right: np.ndarray | None = None
    previous_event: dict | None = None
    previous_sample: dict | None = None

    for event, sample, frame in matched:
        frame_path = (pixel_root / str(frame["file"])).resolve()
        if not frame_path.is_relative_to(pixel_root) or not frame_path.is_file():
            raise ValueError("sidecar frame 路径逃逸或不存在")
        bgr = cv2.imread(str(frame_path), cv2.IMREAD_COLOR)
        if bgr is None or bgr.shape[:2] != (image_height, image_width):
            raise ValueError("sidecar frame 解码失败或几何改变")
        gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
        left_image = _roi_image(gray, arguments.left_roi)
        right_image = _roi_image(gray, arguments.right_roi)
        left_stddev, left_gradient = _texture_metrics(left_image)
        right_stddev, right_gradient = _texture_metrics(right_image)
        left_stddevs.append(left_stddev)
        right_stddevs.append(right_stddev)
        left_gradient_rms.append(left_gradient)
        right_gradient_rms.append(right_gradient)

        if previous_left is not None and previous_right is not None:
            left_motion = measure_translation(previous_left, left_image, left_window)
            right_motion = measure_translation(previous_right, right_image, right_window)
            pairs.append(
                {
                    "pair_index": len(pairs),
                    "from_sample_index": int(previous_event["sample_index"]),
                    "to_sample_index": int(event["sample_index"]),
                    "from_source_timestamp": int(previous_event["source_timestamp"]),
                    "to_source_timestamp": int(event["source_timestamp"]),
                    "to_phase": str(sample["phase"]),
                    "to_block_id": int(sample["block_id"]),
                    "to_nominal_dx_counts": int(sample["dx_counts"]),
                    "left_dx_px": left_motion["dx_px"],
                    "left_dy_px": left_motion["dy_px"],
                    "left_response": left_motion["response"],
                    "right_dx_px": right_motion["dx_px"],
                    "right_dy_px": right_motion["dy_px"],
                    "right_response": right_motion["response"],
                    "abs_left_right_dx_difference_px": abs(
                        left_motion["dx_px"] - right_motion["dx_px"]
                    ),
                    "abs_left_right_dy_difference_px": abs(
                        left_motion["dy_px"] - right_motion["dy_px"]
                    ),
                }
            )
        previous_left = left_image
        previous_right = right_image
        previous_event = event
        previous_sample = sample

    baseline_sample_count = int(sequence.get("request", {}).get("baseline_sample_count", 0))
    if baseline_sample_count < 2 or baseline_sample_count > len(samples):
        raise ValueError("sequence baseline_sample_count 非法")
    if any(
        str(sample.get("phase")) != "baseline"
        or int(sample.get("dx_counts", 0)) != 0
        or int(sample.get("dy_counts", 0)) != 0
        for sample in samples[:baseline_sample_count]
    ):
        raise ValueError("sequence baseline 不是严格零输入")
    baseline_pairs = pairs[: baseline_sample_count - 1]

    def motion_summary(rows: Sequence[dict]) -> dict:
        left_dx = [float(row["left_dx_px"]) for row in rows]
        right_dx = [float(row["right_dx_px"]) for row in rows]
        return {
            "pair_count": len(rows),
            "left_dx_px": distribution_summary(left_dx),
            "right_dx_px": distribution_summary(right_dx),
            "left_abs_dx_px": distribution_summary(abs(value) for value in left_dx),
            "right_abs_dx_px": distribution_summary(abs(value) for value in right_dx),
            "left_response": distribution_summary(
                float(row["left_response"]) for row in rows
            ),
            "right_response": distribution_summary(
                float(row["right_response"]) for row in rows
            ),
            "abs_left_right_dx_difference_px": distribution_summary(
                float(row["abs_left_right_dx_difference_px"]) for row in rows
            ),
            "dx_correlation": _correlation_summary(left_dx, right_dx),
        }

    invalid_reasons: list[str] = []
    if min(left_stddevs) <= 0.0 or min(right_stddevs) <= 0.0:
        invalid_reasons.append("CONSTANT_WITNESS_ROI")
    if min(left_gradient_rms) <= 0.0 or min(right_gradient_rms) <= 0.0:
        invalid_reasons.append("NO_WITNESS_GRADIENT")
    if len(baseline_pairs) != baseline_sample_count - 1:
        invalid_reasons.append("INCOMPLETE_BASELINE")

    result = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_background_witness_baseline",
        "status": "VALID" if not invalid_reasons else "INVALID",
        "invalid_reasons": invalid_reasons,
        "physical_output_capability": False,
        "visible_effect_analyzed": False,
        "method": {
            "name": "opencv_phase_correlate",
            "input": "grayscale_float32",
            "window": "opencv_createHanningWindow_CV_32F",
            "shift_semantic": "current_frame_relative_to_previous_frame",
            "response_semantic": "continuous_peak_quality_may_exceed_one_not_probability_or_gain",
            "opencv_version": cv2.__version__,
        },
        "run_binding": {
            "run_uuid": report["run_uuid"],
            "activation_epoch": report["activation_epoch"],
            "sequence_sha256": report["sequence_sha256"],
            "probe_binding_sha256": report["binding"]["probe_binding_sha256"],
            "command_report_sha256": report["report_sha256"],
            "capture_source_name": manifest["capture_source_name"],
            "sidecar_source_binding_sha256": manifest["source_binding"]["sha256"],
        },
        "geometry": {
            "image_width": image_width,
            "image_height": image_height,
            "left_roi": _geometry_summary(arguments.left_roi, image_width, image_height),
            "right_roi": _geometry_summary(arguments.right_roi, image_width, image_height),
            "horizontal_gap_px": arguments.right_roi[0]
            - (arguments.left_roi[0] + arguments.left_roi[2]),
        },
        "matched_event_frame_count": len(matched),
        "texture": {
            "left_gray_stddev": distribution_summary(left_stddevs),
            "right_gray_stddev": distribution_summary(right_stddevs),
            "left_sobel_gradient_rms": distribution_summary(left_gradient_rms),
            "right_sobel_gradient_rms": distribution_summary(right_gradient_rms),
        },
        "zero_input_baseline": motion_summary(baseline_pairs),
        "whole_output_off_sequence": motion_summary(pairs),
        "pairs_csv": str(arguments.pairs_csv.resolve()),
    }
    return result, pairs


def _file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _array_sha256(image: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(image).tobytes()).hexdigest()


def _load_bgr_frame(
    pixel_root: pathlib.Path,
    frame: dict,
    expected_geometry: tuple[int, int] | None = None,
) -> np.ndarray:
    frame_path = (pixel_root / str(frame["file"])).resolve()
    if not frame_path.is_relative_to(pixel_root) or not frame_path.is_file():
        raise ValueError("sidecar frame 路径逃逸或不存在")
    bgr = cv2.imread(str(frame_path), cv2.IMREAD_COLOR)
    if bgr is None or bgr.ndim != 3:
        raise ValueError("sidecar frame 无法解码为 BGR")
    if expected_geometry is not None and bgr.shape[:2] != expected_geometry:
        raise ValueError("sidecar frame 几何在 Run 内发生变化")
    return bgr


def _witness_images(
    bgr: np.ndarray,
    left_roi: Roi,
    right_roi: Roi,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)
    left_gray = _roi_image(gray, left_roi)
    right_gray = _roi_image(gray, right_roi)
    left_x, left_y, left_width, left_height = left_roi
    right_x, right_y, right_width, right_height = right_roi
    left_bgr = np.ascontiguousarray(
        bgr[left_y : left_y + left_height, left_x : left_x + left_width]
    )
    right_bgr = np.ascontiguousarray(
        bgr[right_y : right_y + right_height, right_x : right_x + right_width]
    )
    return left_gray, right_gray, left_bgr, right_bgr


def _milliseconds(nanoseconds: int) -> float:
    return float(nanoseconds) / 1_000_000.0


def _physical_frame_index(frames: Sequence[dict]) -> dict[int, tuple[int, dict]]:
    frame_by_timestamp: dict[int, tuple[int, dict]] = {}
    for index, frame in enumerate(frames):
        if (
            not isinstance(frame, dict)
            or int(frame.get("index", -1)) != index
            or frame.get("source_timestamp_valid") is not True
            or frame.get("source_time_timing_valid") is not True
            or frame.get("source_clock_status") != "VALID"
            or frame.get("source_time_basis") != "NDI_SDK_SUBMISSION"
            or int(frame.get("source_dropped_frames", -1)) != 0
            or int(frame.get("transport_dropped_frames", -1)) != 0
            or int(frame.get("transport_invalid_packets", -1)) != 0
        ):
            raise ValueError("Physical sidecar frame timing/drop 契约不完整")
        timestamp = int(frame["source_timestamp"])
        if timestamp in frame_by_timestamp:
            raise ValueError("Physical sidecar source timestamp 不唯一")
        frame_by_timestamp[timestamp] = (index, frame)
    return frame_by_timestamp


def _match_physical_events(
    report: dict,
    sequence: dict,
    frame_by_timestamp: dict[int, tuple[int, dict]],
) -> tuple[list[tuple[dict, dict, dict, int] | None], list[int]]:
    result = report["result"]
    events = result.get("events")
    samples = sequence.get("samples")
    if (
        not isinstance(events, list)
        or not isinstance(samples, list)
        or len(events) != len(samples)
        or len(events) < 2
        or int(result.get("consumed_sample_count", -1)) != len(samples)
    ):
        raise ValueError("Physical command event 与 sequence 容量不一致")

    matched: list[tuple[dict, dict, dict, int] | None] = []
    pulse_positions: list[int] = []
    requested_x = 0
    backend_x = 0
    previous_manifest_index = -1
    baseline_sample_count = int(
        sequence.get("request", {}).get("baseline_sample_count", 0)
    )
    for index, (event, sample) in enumerate(zip(events, samples, strict=True)):
        nominal_x = int(sample.get("dx_counts", 0))
        nominal_y = int(sample.get("dy_counts", 0))
        if (
            int(sample.get("sample_index", -1)) != index
            or int(event.get("sample_index", -1)) != index
            or int(event.get("block_id", -1)) != int(sample.get("block_id", -2))
            or event.get("run_uuid") != report.get("run_uuid")
            or int(event.get("activation_epoch", -1))
            != int(report.get("activation_epoch", -2))
            or event.get("sequence_sha256") != report.get("sequence_sha256")
            or int(event.get("nominal_dx_counts", 0)) != nominal_x
            or int(event.get("nominal_dy_counts", 0)) != nominal_y
            or nominal_y != 0
            or int(event.get("requested_dy_counts", 0)) != 0
            or event.get("source_timestamp_valid") is not True
            or event.get("source_clock_status") != "VALID"
            or event.get("source_time_basis") != "NDI_SDK_SUBMISSION"
            or int(event.get("source_dropped_frames", -1)) != 0
            or int(event.get("transport_dropped_frames", -1)) != 0
            or int(event.get("transport_invalid_packets", -1)) != 0
            or event.get("safety_allowed") is not True
            or event.get("mouse_status") != "READY"
            or event.get("stop_reason") != "none"
        ):
            raise ValueError("Physical command event 与 sample/timing/safety 契约不一致")

        if nominal_x == 0:
            if (
                event.get("dispatch_attempted") is not False
                or int(event.get("requested_dx_counts", 0)) != 0
                or event.get("backend_succeeded") is not False
                or event.get("protocol_ack_received") is not False
                or int(event.get("issued_at_steady_ns", 0)) != 0
                or int(event.get("backend_completed_at_steady_ns", 0)) != 0
                or int(event.get("protocol_ack_received_at_steady_ns", 0)) != 0
                or int(event.get("returned_at_steady_ns", 0)) != 0
            ):
                raise ValueError("Physical 零输入 sample 发生了 dispatch/backend/ACK")
        else:
            issued_at = int(event.get("issued_at_steady_ns", 0))
            backend_at = int(event.get("backend_completed_at_steady_ns", 0))
            ack_at = int(event.get("protocol_ack_received_at_steady_ns", 0))
            returned_at = int(event.get("returned_at_steady_ns", 0))
            if (
                abs(nominal_x) != 1
                or event.get("dispatch_attempted") is not True
                or int(event.get("requested_dx_counts", 0)) != nominal_x
                or event.get("backend_succeeded") is not True
                or event.get("protocol_ack_received") is not True
                or issued_at <= 0
                or backend_at < issued_at
                or ack_at < issued_at
                or returned_at < max(backend_at, ack_at)
            ):
                raise ValueError("Physical pulse 缺少 ±1 dispatch/backend/ACK 完整证据")
            requested_x += nominal_x
            backend_x += nominal_x
        if (
            int(event.get("cumulative_requested_x_counts", 0)) != requested_x
            or int(event.get("cumulative_backend_completed_x_counts", 0)) != backend_x
        ):
            raise ValueError("Physical cumulative requested/backend count 不守恒")

        frame_match = frame_by_timestamp.get(int(event["source_timestamp"]))
        if frame_match is None:
            unmatched_baseline_allowed = (
                nominal_x == 0
                and str(sample.get("phase")) == "baseline"
                and 0 < index < baseline_sample_count - 1
            )
            if not unmatched_baseline_allowed:
                raise ValueError(
                    "Physical command event 缺少必须的同 timestamp sidecar frame"
                )
            matched.append(None)
            continue
        manifest_index, frame = frame_match
        if (
            manifest_index <= previous_manifest_index
            or str(frame.get("source_clock_session_id"))
            != str(event.get("source_clock_session_id"))
        ):
            raise ValueError("Physical event/frame 顺序或 source-clock session 不一致")
        previous_manifest_index = manifest_index
        if nominal_x != 0:
            pulse_positions.append(index)
        matched.append((event, sample, frame, manifest_index))

    unmatched_baseline_event_count = sum(item is None for item in matched)
    if unmatched_baseline_event_count > 1:
        raise ValueError("Physical baseline 的未观测零事件超过预注册上限")

    pulse_signs = [int(samples[position]["dx_counts"]) for position in pulse_positions]
    profile = str(sequence.get("profile", ""))
    if profile == "sparse_pulse_a":
        sequence_shape_valid = pulse_signs == [1, -1, -1, 1]
    else:
        blocks = sequence.get("blocks")
        request = sequence.get("request", {})
        run_role = str(request.get("run_role", ""))
        expected_profile = {
            "p_cal": "dependency_calibration_a2_p_cal",
            "p_holdout": "dependency_calibration_a2_p_holdout",
        }.get(run_role)
        block_count = int(request.get("block_count", 0))
        expected_pattern = [1, -1, -1, 1] if run_role == "p_cal" else [-1, 1, 1, -1]
        sequence_shape_valid = (
            expected_profile == profile
            and isinstance(blocks, list)
            and block_count >= 4
            and block_count % 4 == 0
            and len(blocks) == block_count
            and len(pulse_positions) == block_count * 2
        )
        if sequence_shape_valid:
            for block_index, block in enumerate(blocks):
                first_position = pulse_positions[block_index * 2]
                second_position = pulse_positions[block_index * 2 + 1]
                first_sample = samples[first_position]
                second_sample = samples[second_position]
                expected_first = expected_pattern[block_index % 4]
                if (
                    int(block.get("block_id", -1)) != block_index + 1
                    or int(first_sample.get("block_id", -1)) != block_index + 1
                    or int(second_sample.get("block_id", -1)) != block_index + 1
                    or int(first_sample.get("dx_counts", 0)) != expected_first
                    or int(second_sample.get("dx_counts", 0)) != -expected_first
                    or int(block.get("first_pulse_dx_counts", 0)) != expected_first
                    or int(block.get("second_pulse_dx_counts", 0)) != -expected_first
                ):
                    sequence_shape_valid = False
                    break
    if (
        not sequence_shape_valid
        or int(sequence.get("summary", {}).get("net_x_counts", 1)) != 0
        or int(sequence.get("summary", {}).get("max_abs_prefix_x_counts", 0)) != 1
        or requested_x != 0
        or backend_x != 0
        or int(result.get("cumulative_requested_x_counts", 1)) != 0
        or int(result.get("cumulative_backend_completed_x_counts", 1)) != 0
    ):
        raise ValueError("Physical sparse-pulse 序列不是按 profile 预注册的 ±1 成对净零序列")
    return matched, pulse_positions


def _baseline_motion(
    matched: Sequence[tuple[dict, dict, dict, int] | None],
    baseline_sample_count: int,
    pixel_root: pathlib.Path,
    left_roi: Roi,
    right_roi: Roi,
    geometry: tuple[int, int],
    left_window: np.ndarray,
    right_window: np.ndarray,
) -> dict:
    if baseline_sample_count < 2 or baseline_sample_count > len(matched):
        raise ValueError("Physical sequence baseline_sample_count 非法")
    baseline_matches = [
        item for item in matched[:baseline_sample_count] if item is not None
    ]
    if len(baseline_matches) < 2:
        raise ValueError("Physical sequence baseline 同 timestamp sidecar frame 不足")
    if any(
        str(sample.get("phase")) != "baseline"
        or int(sample.get("dx_counts", 0)) != 0
        or int(sample.get("dy_counts", 0)) != 0
        for _, sample, _, _ in baseline_matches
    ):
        raise ValueError("Physical sequence baseline 不是严格零输入")

    left_dx: list[float] = []
    right_dx: list[float] = []
    left_abs_dx: list[float] = []
    right_abs_dx: list[float] = []
    left_states: set[str] = set()
    right_states: set[str] = set()
    previous_left: np.ndarray | None = None
    previous_right: np.ndarray | None = None
    for _, _, frame, _ in baseline_matches:
        bgr = _load_bgr_frame(pixel_root, frame, geometry)
        left, right, left_bgr, right_bgr = _witness_images(bgr, left_roi, right_roi)
        left_states.add(_array_sha256(left_bgr))
        right_states.add(_array_sha256(right_bgr))
        if previous_left is not None and previous_right is not None:
            left_motion = measure_translation(previous_left, left, left_window)
            right_motion = measure_translation(previous_right, right, right_window)
            left_dx.append(left_motion["dx_px"])
            right_dx.append(right_motion["dx_px"])
            left_abs_dx.append(abs(left_motion["dx_px"]))
            right_abs_dx.append(abs(right_motion["dx_px"]))
        previous_left = left
        previous_right = right
    return {
        "sample_count": len(baseline_matches),
        "expected_event_count": baseline_sample_count,
        "matched_frame_count": len(baseline_matches),
        "unmatched_event_count": baseline_sample_count - len(baseline_matches),
        "pair_count": len(baseline_matches) - 1,
        "left_exact_state_count": len(left_states),
        "right_exact_state_count": len(right_states),
        "left_dx_px": distribution_summary(left_dx),
        "right_dx_px": distribution_summary(right_dx),
        "left_abs_dx_px": distribution_summary(left_abs_dx),
        "right_abs_dx_px": distribution_summary(right_abs_dx),
    }


def analyze_physical(arguments: argparse.Namespace) -> tuple[dict, list[dict]]:
    manifest = _load_json(arguments.manifest, "sidecar manifest")
    report = _load_json(arguments.command_report, "command report")
    sequence = _load_json(arguments.sequence, "probe sequence")
    result = report.get("result", {})
    binding = report.get("binding", {})
    source_binding = manifest.get("source_binding", {})
    profile = str(sequence.get("profile", ""))
    is_a1 = int(sequence.get("schema", 0)) == 1 and profile == "sparse_pulse_a"
    a2_role = {
        "dependency_calibration_a2_p_cal": "p-cal",
        "dependency_calibration_a2_p_holdout": "p-holdout",
    }.get(profile)
    is_a2 = int(sequence.get("schema", 0)) == 2 and a2_role is not None
    if (
        manifest.get("evidence_type") != "output_off_capture"
        or manifest.get("physical_output_capability") is not False
        or report.get("dispatch_mode") != "physical_a"
        or report.get("profile") != profile
        or not (is_a1 or is_a2)
        or result.get("state") != "completed"
        or result.get("stop_reason") != "normal_completion"
        or result.get("complete") is not True
        or report.get("sequence_sha256") != sequence.get("sequence_sha256")
        or binding.get("probe_binding_sha256") != source_binding.get("sha256")
        or binding.get("sidecar_run_uuid") != report.get("run_uuid")
        or binding.get("capture_source_name") != manifest.get("capture_source_name")
        or report.get("executor_timebase", {}).get("name")
        != "steady_clock_nanoseconds_since_epoch"
        or int(report.get("executor_timebase", {}).get("ticks_per_second", 0))
        != 1_000_000_000
    ):
        raise ValueError("输入证据不是同一完整 Physical sparse-pulse A/A2 Run")

    frames = manifest.get("frames")
    if (
        not isinstance(frames, list)
        or len(frames) < 2
        or int(manifest.get("recorded_frame_count", -1)) != len(frames)
        or int(manifest.get("requested_frame_count", -1)) != len(frames)
    ):
        raise ValueError("Physical sidecar manifest frame 容量不完整")
    frame_by_timestamp = _physical_frame_index(frames)
    source_mapping_uncertainty_ms: list[float] = []
    if is_a2:
        for frame in frames:
            uncertainty = float(frame.get("source_clock_uncertainty_ms", -1.0))
            if not math.isfinite(uncertainty) or uncertainty < 0.0:
                raise ValueError("Physical A2 frame 缺少有效 source mapping uncertainty")
            source_mapping_uncertainty_ms.append(uncertainty)
    matched, pulse_positions = _match_physical_events(
        report, sequence, frame_by_timestamp
    )

    pixel_root = arguments.manifest.resolve().parent
    matched_items = [item for item in matched if item is not None]
    if not matched_items:
        raise ValueError("Physical event/frame 没有任何同 timestamp 交集")
    first_bgr = _load_bgr_frame(pixel_root, matched_items[0][2])
    image_height, image_width = first_bgr.shape[:2]
    geometry = (image_height, image_width)
    validate_roi_pair(arguments.left_roi, arguments.right_roi, image_width, image_height)
    left_window = cv2.createHanningWindow(
        (arguments.left_roi[2], arguments.left_roi[3]), cv2.CV_32F
    )
    right_window = cv2.createHanningWindow(
        (arguments.right_roi[2], arguments.right_roi[3]), cv2.CV_32F
    )
    matched_event_frame_count = len(matched_items)

    baseline_sample_count = int(sequence.get("request", {}).get(
        "baseline_sample_count", 0
    ))
    baseline = _baseline_motion(
        matched,
        baseline_sample_count,
        pixel_root,
        arguments.left_roi,
        arguments.right_roi,
        geometry,
        left_window,
        right_window,
    )

    pulse_responses: list[dict] = []
    rows: list[dict] = []
    internal_states: list[dict] = []
    reference_texture_left: list[float] = []
    reference_texture_right: list[float] = []
    reference_gradient_left: list[float] = []
    reference_gradient_right: list[float] = []
    for pulse_index, matched_position in enumerate(pulse_positions):
        reference_match = matched[matched_position]
        if reference_match is None:
            raise ValueError("Physical pulse reference frame 缺失")
        event, sample, reference_frame, reference_manifest_index = reference_match
        next_pulse_position = (
            pulse_positions[pulse_index + 1]
            if pulse_index + 1 < len(pulse_positions)
            else len(matched)
        )
        if next_pulse_position <= matched_position + 1:
            raise ValueError("Physical pulse 后没有可观察 source frame")
        reference_bgr = _load_bgr_frame(pixel_root, reference_frame, geometry)
        (
            reference_left,
            reference_right,
            reference_left_bgr,
            reference_right_bgr,
        ) = _witness_images(
            reference_bgr, arguments.left_roi, arguments.right_roi
        )
        left_stddev, left_gradient = _texture_metrics(reference_left)
        right_stddev, right_gradient = _texture_metrics(reference_right)
        reference_texture_left.append(left_stddev)
        reference_texture_right.append(right_stddev)
        reference_gradient_left.append(left_gradient)
        reference_gradient_right.append(right_gradient)
        reference_left_sha = _array_sha256(reference_left_bgr)
        reference_right_sha = _array_sha256(reference_right_bgr)
        pulse_rows: list[dict] = []

        for current_position in range(matched_position + 1, next_pulse_position):
            current_match = matched[current_position]
            if current_match is None:
                continue
            frame_lag = current_position - matched_position
            current_event, current_sample, current_frame, current_manifest_index = (
                current_match
            )
            current_bgr = _load_bgr_frame(pixel_root, current_frame, geometry)
            current_left, current_right, current_left_bgr, current_right_bgr = (
                _witness_images(current_bgr, arguments.left_roi, arguments.right_roi)
            )
            current_left_sha = _array_sha256(current_left_bgr)
            current_right_sha = _array_sha256(current_right_bgr)
            left_motion = measure_translation(
                reference_left, current_left, left_window
            )
            right_motion = measure_translation(
                reference_right, current_right, right_window
            )
            current_source_ns = int(current_event["source_time_at_steady_ns"])
            row = {
                "pulse_index": pulse_index,
                "block_id": int(sample["block_id"]),
                "command_dx_counts": int(sample["dx_counts"]),
                "reference_sample_index": int(sample["sample_index"]),
                "sample_index": int(current_sample["sample_index"]),
                "frame_lag": frame_lag,
                "manifest_index": current_manifest_index,
                "source_timestamp": int(current_frame["source_timestamp"]),
                "source_clock_session_id": str(
                    current_frame["source_clock_session_id"]
                ),
                "source_clock_uncertainty_ms": (
                    float(current_frame.get("source_clock_uncertainty_ms"))
                    if is_a2
                    else None
                ),
                "png_sha256": str(current_frame.get("png_sha256", "")),
                "bgr_sha256": str(current_frame.get("bgr_sha256", "")),
                "phase": str(current_sample["phase"]),
                "source_submission_after_pulse_ms": _milliseconds(
                    current_source_ns - int(event["source_time_at_steady_ns"])
                ),
                "source_submission_after_command_issue_ms": _milliseconds(
                    current_source_ns - int(event["issued_at_steady_ns"])
                ),
                "source_submission_after_protocol_ack_ms": _milliseconds(
                    current_source_ns
                    - int(event["protocol_ack_received_at_steady_ns"])
                ),
                "left_exact_changed": current_left_sha != reference_left_sha,
                "right_exact_changed": current_right_sha != reference_right_sha,
                "left_state_sha256": current_left_sha,
                "right_state_sha256": current_right_sha,
                "left_dx_px": left_motion["dx_px"],
                "left_dy_px": left_motion["dy_px"],
                "left_response": left_motion["response"],
                "right_dx_px": right_motion["dx_px"],
                "right_dy_px": right_motion["dy_px"],
                "right_response": right_motion["response"],
                "mean_dx_px": 0.5
                * (left_motion["dx_px"] + right_motion["dx_px"]),
                "abs_left_right_dx_difference_px": abs(
                    left_motion["dx_px"] - right_motion["dx_px"]
                ),
            }
            pulse_rows.append(row)
            rows.append(row)

        left_onset = next(
            (row for row in pulse_rows if row["left_exact_changed"]), None
        )
        right_onset = next(
            (row for row in pulse_rows if row["right_exact_changed"]), None
        )
        joint_onset = next(
            (
                row
                for row in pulse_rows
                if row["left_exact_changed"] and row["right_exact_changed"]
            ),
            None,
        )
        last_joint_unchanged = None
        if joint_onset is not None:
            for row in pulse_rows:
                if row is joint_onset:
                    break
                if not row["left_exact_changed"] and not row["right_exact_changed"]:
                    last_joint_unchanged = row
        final_row = pulse_rows[-1]
        final_left_sha = str(final_row["left_state_sha256"])
        final_right_sha = str(final_row["right_state_sha256"])
        post_onset_rows = (
            pulse_rows[pulse_rows.index(joint_onset) :]
            if joint_onset is not None
            else []
        )
        unique_post_onset_states = {
            (str(row["left_state_sha256"]), str(row["right_state_sha256"]))
            for row in post_onset_rows
        }
        mean_dx_px = float(final_row["mean_dx_px"])
        command_dx = int(sample["dx_counts"])
        if mean_dx_px < 0.0:
            background_direction = "left"
        elif mean_dx_px > 0.0:
            background_direction = "right"
        else:
            background_direction = "none"
        reference_source_ns = int(event["source_time_at_steady_ns"])
        issued_ns = int(event["issued_at_steady_ns"])
        last_unchanged_match = (
            matched[last_joint_unchanged["sample_index"]]
            if last_joint_unchanged is not None
            else None
        )
        last_unchanged_source_ns = (
            int(last_unchanged_match[0]["source_time_at_steady_ns"])
            if last_unchanged_match is not None
            else reference_source_ns
        )
        onset = {
            "left_first_changed_frame_lag": (
                int(left_onset["frame_lag"]) if left_onset is not None else None
            ),
            "right_first_changed_frame_lag": (
                int(right_onset["frame_lag"]) if right_onset is not None else None
            ),
            "last_joint_unchanged_frame_lag": (
                int(last_joint_unchanged["frame_lag"])
                if last_joint_unchanged is not None
                else 0
            ),
            "first_changed_frame_lag": (
                int(joint_onset["frame_lag"]) if joint_onset is not None else None
            ),
            "last_joint_unchanged_source_submission_after_pulse_ms": _milliseconds(
                last_unchanged_source_ns - reference_source_ns
            ),
            "last_joint_unchanged_source_submission_after_command_issue_ms": (
                _milliseconds(last_unchanged_source_ns - issued_ns)
            ),
            "first_changed_source_submission_after_pulse_ms": (
                float(joint_onset["source_submission_after_pulse_ms"])
                if joint_onset is not None
                else None
            ),
            "first_changed_source_submission_after_command_issue_ms": (
                float(joint_onset["source_submission_after_command_issue_ms"])
                if joint_onset is not None
                else None
            ),
            "timestamp_basis": "NDI_SDK_SUBMISSION_MAPPED_TO_STEADY_CLOCK_NOT_EXPOSURE",
        }
        pulse_response = {
            "pulse_index": pulse_index,
            "block_id": int(sample["block_id"]),
            "sample_index": int(sample["sample_index"]),
            "manifest_index": reference_manifest_index,
            "command_dx_counts": command_dx,
            "source_to_command_issue_ms": _milliseconds(
                issued_ns - reference_source_ns
            ),
            "command_issue_to_protocol_ack_ms": _milliseconds(
                int(event["protocol_ack_received_at_steady_ns"]) - issued_ns
            ),
            "command_issue_to_backend_complete_ms": _milliseconds(
                int(event["backend_completed_at_steady_ns"]) - issued_ns
            ),
            "onset": onset,
            "observed_frame_count": len(pulse_rows),
            "left_dx_px": float(final_row["left_dx_px"]),
            "right_dx_px": float(final_row["right_dx_px"]),
            "mean_background_dx_px": mean_dx_px,
            "abs_left_right_dx_difference_px": float(
                final_row["abs_left_right_dx_difference_px"]
            ),
            "left_phase_response": float(final_row["left_response"]),
            "right_phase_response": float(final_row["right_response"]),
            "background_direction": background_direction,
            "direction_contract_matches": mean_dx_px * command_dx < 0.0,
            "x_px_per_count": -mean_dx_px / float(command_dx),
            "joint_exact_change_observed": joint_onset is not None,
            "post_onset_exact_state_count": len(unique_post_onset_states),
            "stable_step_observed": joint_onset is not None
            and len(unique_post_onset_states) == 1,
            "reference_state": {
                "left_sha256": reference_left_sha,
                "right_sha256": reference_right_sha,
            },
            "final_state": {
                "left_sha256": final_left_sha,
                "right_sha256": final_right_sha,
            },
        }
        pulse_responses.append(pulse_response)
        internal_states.append(
            {
                "reference": (reference_left_sha, reference_right_sha),
                "final": (final_left_sha, final_right_sha),
            }
        )

    paired_closure: list[dict] = []
    for first_index in range(0, len(pulse_responses), 2):
        second_index = first_index + 1
        first = pulse_responses[first_index]
        second = pulse_responses[second_index]
        if int(first["block_id"]) != int(second["block_id"]):
            raise ValueError("Physical pulse pair 跨越了 block 边界")
        paired_closure.append(
            {
                "block_id": int(first["block_id"]),
                "pulse_indices": [first_index, second_index],
                "background_dx_sum_px": float(first["mean_background_dx_px"])
                + float(second["mean_background_dx_px"]),
                "abs_x_px_per_count_difference": abs(
                    float(first["x_px_per_count"])
                    - float(second["x_px_per_count"])
                ),
                "exact_witness_return": internal_states[second_index]["final"]
                == internal_states[first_index]["reference"],
            }
        )
    whole_sequence_closure = {
        "net_requested_x_counts": int(
            result["cumulative_requested_x_counts"]
        ),
        "net_backend_completed_x_counts": int(
            result["cumulative_backend_completed_x_counts"]
        ),
        "exact_witness_return": internal_states[-1]["final"]
        == internal_states[0]["reference"],
    }
    unique_witness_states = {
        state
        for pulse_state in internal_states
        for state in (pulse_state["reference"], pulse_state["final"])
    }

    invalid_reasons: list[str] = []
    if min(reference_texture_left) <= 0.0 or min(reference_texture_right) <= 0.0:
        invalid_reasons.append("CONSTANT_WITNESS_ROI")
    if min(reference_gradient_left) <= 0.0 or min(reference_gradient_right) <= 0.0:
        invalid_reasons.append("NO_WITNESS_GRADIENT")
    physical_result = {
        "schema_version": 2 if is_a2 else 1,
        "evidence_type": (
            "mouse_effect_probe_a2_physical_background_response"
            if is_a2
            else "mouse_effect_probe_physical_background_response"
        ),
        "status": "VALID" if not invalid_reasons else "INVALID",
        "invalid_reasons": invalid_reasons,
        "physical_output_capability": False,
        "visible_effect_analyzed": True,
        "machine_visible_effect_observed": all(
            bool(pulse["joint_exact_change_observed"])
            for pulse in pulse_responses
        ),
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "profile": profile,
        "run_role": a2_role,
        "a2_dependency_gate_claimed": False,
        "method": {
            "name": "exact_witness_change_plus_opencv_phase_correlate",
            "onset_semantic": "first_exact_left_and_right_witness_pixel_change",
            "shift_semantic": "current_frame_relative_to_pre_command_source_frame",
            "step_semantic": "one_count_changes_persistent_pointer_state_until_inverse_count",
            "response_semantic": "continuous_peak_quality_may_exceed_one_not_probability_or_gain",
            "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
            "opencv_version": cv2.__version__,
        },
        "run_binding": {
            "run_uuid": report["run_uuid"],
            "activation_epoch": report["activation_epoch"],
            "sequence_sha256": report["sequence_sha256"],
            "probe_binding_sha256": binding["probe_binding_sha256"],
            "command_report_semantic_sha256": report["report_sha256"],
            "command_report_file_sha256": _file_sha256(arguments.command_report),
            "sequence_file_sha256": _file_sha256(arguments.sequence),
            "sidecar_manifest_file_sha256": _file_sha256(arguments.manifest),
            "analyzer_file_sha256": _file_sha256(pathlib.Path(__file__).resolve()),
            "capture_source_name": manifest["capture_source_name"],
        },
        "geometry": {
            "image_width": image_width,
            "image_height": image_height,
            "left_roi": _geometry_summary(
                arguments.left_roi, image_width, image_height
            ),
            "right_roi": _geometry_summary(
                arguments.right_roi, image_width, image_height
            ),
        },
        "sidecar_frame_count": len(frames),
        "matched_event_frame_count": matched_event_frame_count,
        "source_timestamp_unmatched_baseline_event_count": (
            len(matched) - matched_event_frame_count
        ),
        "source_timestamp_unmatched_baseline_event_limit": 1,
        "event_minus_manifest_source_time_mapping_ns": distribution_summary(
            int(event["source_time_at_steady_ns"])
            - int(frame["source_time_at_steady_ns"])
            for event, _, frame, _ in matched_items
        ),
        "source_mapping_uncertainty_ms": (
            distribution_summary(source_mapping_uncertainty_ms)
            if is_a2
            else None
        ),
        "source_mapping_uncertainty_px": (
            "PENDING_CANDIDATE_FRAME_HULL_NO_TIME_TIMES_FIXED_SPEED"
            if is_a2
            else None
        ),
        "backend_completed_pulse_count": len(pulse_positions),
        "texture": {
            "left_gray_stddev": distribution_summary(reference_texture_left),
            "right_gray_stddev": distribution_summary(reference_texture_right),
            "left_sobel_gradient_rms": distribution_summary(
                reference_gradient_left
            ),
            "right_sobel_gradient_rms": distribution_summary(
                reference_gradient_right
            ),
        },
        "zero_input_baseline": baseline,
        "pulse_responses": pulse_responses,
        "witness_state_summary": {
            "transition_observation_count": len(pulse_responses),
            "unique_exact_state_count": len(unique_witness_states),
            "distinct_excursion_count": max(0, len(unique_witness_states) - 1),
            "statistical_independence_claimed": False,
        },
        "x_px_per_count": distribution_summary(
            float(pulse["x_px_per_count"]) for pulse in pulse_responses
        ),
        "x_px_per_count_scope": "run_local_transition_displacement_not_general_plant_gain",
        "paired_closure": paired_closure,
        "whole_sequence_closure": whole_sequence_closure,
        "pairs_csv": str(arguments.pairs_csv.resolve()),
    }
    return physical_result, rows


def main() -> int:
    arguments = _parse_arguments()
    try:
        if arguments.output.exists() or arguments.pairs_csv.exists():
            raise ValueError("分析输出已存在，拒绝覆盖")
        if arguments.analysis_mode == "physical-response":
            result, pairs = analyze_physical(arguments)
        else:
            result, pairs = analyze(arguments)
        fieldnames = list(pairs[0].keys())
        pending_csv = arguments.pairs_csv.with_name(
            f"{arguments.pairs_csv.name}.pending-{os.getpid()}"
        )
        arguments.pairs_csv.parent.mkdir(parents=True, exist_ok=True)
        if pending_csv.exists():
            raise ValueError("pairs CSV 临时输出已存在")
        try:
            with pending_csv.open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=fieldnames)
                writer.writeheader()
                writer.writerows(pairs)
            os.rename(pending_csv, arguments.pairs_csv)
        finally:
            if pending_csv.exists():
                pending_csv.unlink()
        _atomic_write_text(
            arguments.output,
            json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        )
        if arguments.analysis_mode == "physical-response":
            print(
                "physical background response "
                f"{result['status']}: matched={result['matched_event_frame_count']}, "
                f"pulses={result['backend_completed_pulse_count']}, "
                f"visible={result['machine_visible_effect_observed']}, "
                f"output={arguments.output}"
            )
        else:
            print(
                "background witness baseline "
                f"{result['status']}: matched={result['matched_event_frame_count']}, "
                f"baseline_pairs={result['zero_input_baseline']['pair_count']}, "
                f"output={arguments.output}"
            )
        return 0 if result["status"] == "VALID" else 1
    except (OSError, ValueError, KeyError, TypeError, cv2.error) as exception:
        print(f"background witness 分析失败: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
