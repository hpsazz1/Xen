"""将 Xen 姿态诊断结果与 Ultralytics 参考后处理做数值对照。"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--xen-result", required=True, type=Path)
    parser.add_argument("--ultralytics-root", required=True, type=Path)
    parser.add_argument("--maximum-box-delta", type=float, default=1e-3)
    parser.add_argument(
        "--maximum-detection-confidence-delta", type=float, default=1e-5
    )
    parser.add_argument(
        "--maximum-keypoint-coordinate-delta", type=float, default=1e-3
    )
    parser.add_argument(
        "--maximum-keypoint-confidence-delta", type=float, default=1e-5
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    for path, description in (
        (args.model, "ONNX 模型"),
        (args.image, "测试图像"),
        (args.xen_result / "summary.json", "Xen 诊断摘要"),
        (args.ultralytics_root / "ultralytics", "Ultralytics 源码目录"),
    ):
        if not path.exists():
            raise RuntimeError(f"{description}不存在：{path}")

    # 显式使用调用方给出的参考源码，不从网络安装包；onnxruntime 缺失时
    # 直接失败，由调用方准备可复现的数值对照环境。
    sys.path.insert(0, str(args.ultralytics_root))
    try:
        import numpy as np
        import onnxruntime  # noqa: F401
        from ultralytics import YOLO
    except ImportError as error:
        raise RuntimeError(
            "参考验证需要 Ultralytics、NumPy 与 onnxruntime"
        ) from error

    summary = json.loads(
        (args.xen_result / "summary.json").read_text(encoding="utf-8")
    )
    reference = YOLO(str(args.model), task="pose").predict(
        str(args.image),
        imgsz=320,
        conf=0.25,
        iou=0.45,
        max_det=300,
        device="cpu",
        verbose=False,
    )[0]
    if reference.keypoints is None:
        raise RuntimeError("Ultralytics 参考结果没有姿态关键点")

    reference_boxes = reference.boxes.xyxy.cpu().numpy()
    reference_confidences = reference.boxes.conf.cpu().numpy()
    reference_classes = reference.boxes.cls.cpu().numpy().astype(int)
    reference_keypoints = reference.keypoints.data.cpu().numpy()
    instances = summary.get("instances", [])
    if len(instances) != len(reference_boxes) or not instances:
        raise RuntimeError(
            "Xen 与 Ultralytics 实例数不一致或结果为空："
            f"{len(instances)} != {len(reference_boxes)}"
        )
    if reference_keypoints.ndim != 3 or reference_keypoints.shape[2] != 3:
        raise RuntimeError(
            f"参考关键点 shape 不受支持：{reference_keypoints.shape}"
        )
    if int(summary.get("keypoints_per_detection", 0)) != int(
        reference_keypoints.shape[1]
    ) or int(summary.get("keypoint_dimensions", 0)) != 3:
        raise RuntimeError("Xen 与参考关键点 shape 不一致")

    maximum_box_delta = 0.0
    maximum_detection_confidence_delta = 0.0
    maximum_keypoint_coordinate_delta = 0.0
    maximum_keypoint_confidence_delta = 0.0
    compared_visible_keypoints = 0
    for instance_index, instance in enumerate(instances):
        if int(instance["class_id"]) != int(
            reference_classes[instance_index]
        ):
            raise RuntimeError(f"实例 {instance_index} 类别不一致")
        box = np.asarray(
            [
                instance["x1"],
                instance["y1"],
                instance["x2"],
                instance["y2"],
            ],
            dtype=np.float32,
        )
        maximum_box_delta = max(
            maximum_box_delta,
            float(np.max(np.abs(box - reference_boxes[instance_index]))),
        )
        maximum_detection_confidence_delta = max(
            maximum_detection_confidence_delta,
            abs(
                float(instance["confidence"])
                - float(reference_confidences[instance_index])
            ),
        )

        actual_keypoints = instance.get("keypoints", [])
        if len(actual_keypoints) != reference_keypoints.shape[1]:
            raise RuntimeError(f"实例 {instance_index} 关键点数量不一致")
        for keypoint_index, actual in enumerate(actual_keypoints):
            expected = reference_keypoints[instance_index, keypoint_index]
            maximum_keypoint_confidence_delta = max(
                maximum_keypoint_confidence_delta,
                abs(float(actual["confidence"]) - float(expected[2])),
            )
            # Ultralytics Results 会把 confidence<0.5 的坐标置零；Xen 保留
            # 原始解码坐标供调用方自行选择阈值，因此只对可见关键点核对坐标。
            if expected[2] >= 0.5:
                compared_visible_keypoints += 1
                coordinate_delta = max(
                    abs(float(actual["x"]) - float(expected[0])),
                    abs(float(actual["y"]) - float(expected[1])),
                )
                maximum_keypoint_coordinate_delta = max(
                    maximum_keypoint_coordinate_delta, coordinate_delta
                )

    if compared_visible_keypoints == 0:
        raise RuntimeError("参考结果没有可用于坐标对照的可见关键点")

    result = {
        "instances": len(instances),
        "keypoints_per_detection": int(reference_keypoints.shape[1]),
        "compared_visible_keypoints": compared_visible_keypoints,
        "maximum_box_delta_pixels": maximum_box_delta,
        "maximum_detection_confidence_delta": (
            maximum_detection_confidence_delta
        ),
        "maximum_keypoint_coordinate_delta_pixels": (
            maximum_keypoint_coordinate_delta
        ),
        "maximum_keypoint_confidence_delta": (
            maximum_keypoint_confidence_delta
        ),
    }
    print(json.dumps(result, ensure_ascii=False))

    if maximum_box_delta > args.maximum_box_delta:
        raise RuntimeError(
            f"最大框偏差 {maximum_box_delta:.8f} 超过门槛 "
            f"{args.maximum_box_delta:.8f}"
        )
    if (
        maximum_detection_confidence_delta
        > args.maximum_detection_confidence_delta
    ):
        raise RuntimeError(
            "最大检测置信度偏差 "
            f"{maximum_detection_confidence_delta:.8f} 超过门槛 "
            f"{args.maximum_detection_confidence_delta:.8f}"
        )
    if (
        maximum_keypoint_coordinate_delta
        > args.maximum_keypoint_coordinate_delta
    ):
        raise RuntimeError(
            "最大关键点坐标偏差 "
            f"{maximum_keypoint_coordinate_delta:.8f} 超过门槛 "
            f"{args.maximum_keypoint_coordinate_delta:.8f}"
        )
    if (
        maximum_keypoint_confidence_delta
        > args.maximum_keypoint_confidence_delta
    ):
        raise RuntimeError(
            "最大关键点置信度偏差 "
            f"{maximum_keypoint_confidence_delta:.8f} 超过门槛 "
            f"{args.maximum_keypoint_confidence_delta:.8f}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - 命令行边界统一输出中文原因。
        print(f"姿态参考对照失败：{error}", file=sys.stderr)
        raise SystemExit(1)
