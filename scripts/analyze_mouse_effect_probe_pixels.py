#!/usr/bin/env python3
"""分析 Mouse Effect Probe 的左右背景 witness 基线。

本脚本只读取已经发布的 sidecar/command/sequence 证据，不具备 Mouse 输出能力。
phaseCorrelate 的 shift 表示第二帧相对第一帧的平移；response 只按连续质量观测
记录，不在缺少本 Run baseline 时提升为固定通过阈值。
"""

from __future__ import annotations

import argparse
import csv
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
    shift, response = cv2.phaseCorrelate(first, second, window)
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
        description="分析 output-off Mouse Effect Probe 左右背景 witness 基线"
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
            "response_semantic": "continuous_normalized_peak_quality_not_fixed_gate",
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


def main() -> int:
    arguments = _parse_arguments()
    try:
        if arguments.output.exists() or arguments.pairs_csv.exists():
            raise ValueError("分析输出已存在，拒绝覆盖")
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
