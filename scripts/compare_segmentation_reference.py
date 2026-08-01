"""将 Xen 实例分割诊断结果与 Ultralytics 参考后处理做数值对照。"""

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
    parser.add_argument("--minimum-mask-iou", type=float, default=0.96)
    parser.add_argument("--maximum-box-delta", type=float, default=1e-3)
    parser.add_argument("--maximum-confidence-delta", type=float, default=1e-5)
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

    # 显式使用调用方给出的参考实现，不从网络安装包，也不让工作目录中同名
    # 模块改变对照语义。onnxruntime 缺失时直接失败，由调用方准备验证环境。
    sys.path.insert(0, str(args.ultralytics_root))
    try:
        import cv2
        import numpy as np
        import onnxruntime  # noqa: F401
        from ultralytics import YOLO
        from ultralytics.utils.ops import scale_masks
    except ImportError as error:
        raise RuntimeError(
            "参考验证需要 Ultralytics、OpenCV、NumPy 与 onnxruntime"
        ) from error

    summary = json.loads(
        (args.xen_result / "summary.json").read_text(encoding="utf-8")
    )
    reference = YOLO(str(args.model), task="segment").predict(
        str(args.image),
        imgsz=320,
        conf=0.25,
        iou=0.45,
        max_det=300,
        device="cpu",
        verbose=False,
    )[0]
    if reference.masks is None:
        raise RuntimeError("Ultralytics 参考结果没有实例掩码")

    reference_masks = (
        scale_masks(reference.masks.data[None].float(), reference.orig_shape)[0]
        .cpu()
        .numpy()
        > 0.5
    )
    reference_boxes = reference.boxes.xyxy.cpu().numpy()
    reference_confidences = reference.boxes.conf.cpu().numpy()
    reference_classes = reference.boxes.cls.cpu().numpy().astype(int)
    instances = summary.get("instances", [])
    if len(instances) != len(reference_boxes) or not instances:
        raise RuntimeError(
            "Xen 与 Ultralytics 实例数不一致或结果为空："
            f"{len(instances)} != {len(reference_boxes)}"
        )

    mask_ious: list[float] = []
    maximum_box_delta = 0.0
    maximum_confidence_delta = 0.0
    for index, instance in enumerate(instances):
        if int(instance["class_id"]) != int(reference_classes[index]):
            raise RuntimeError(f"实例 {index} 类别不一致")
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
            float(np.max(np.abs(box - reference_boxes[index]))),
        )
        maximum_confidence_delta = max(
            maximum_confidence_delta,
            abs(
                float(instance["confidence"])
                - float(reference_confidences[index])
            ),
        )

        actual = cv2.imread(
            str(args.xen_result / instance["mask_file"]),
            cv2.IMREAD_GRAYSCALE,
        )
        if actual is None:
            raise RuntimeError(f"实例 {index} 的 Xen 掩码不可读")
        actual = np.squeeze(actual) > 0
        expected = np.squeeze(reference_masks[index])
        intersection = int(np.logical_and(actual, expected).sum())
        union = int(np.logical_or(actual, expected).sum())
        mask_ious.append(intersection / union if union else 1.0)

    result = {
        "instances": len(instances),
        "mask_iou": mask_ious,
        "minimum_mask_iou": min(mask_ious),
        "mean_mask_iou": sum(mask_ious) / len(mask_ious),
        "maximum_box_delta_pixels": maximum_box_delta,
        "maximum_confidence_delta": maximum_confidence_delta,
    }
    print(json.dumps(result, ensure_ascii=False))

    if min(mask_ious) < args.minimum_mask_iou:
        raise RuntimeError(
            f"最小掩码 IoU {min(mask_ious):.6f} 低于门槛 "
            f"{args.minimum_mask_iou:.6f}"
        )
    if maximum_box_delta > args.maximum_box_delta:
        raise RuntimeError(
            f"最大框偏差 {maximum_box_delta:.8f} 超过门槛 "
            f"{args.maximum_box_delta:.8f}"
        )
    if maximum_confidence_delta > args.maximum_confidence_delta:
        raise RuntimeError(
            f"最大置信度偏差 {maximum_confidence_delta:.8f} 超过门槛 "
            f"{args.maximum_confidence_delta:.8f}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - 命令行边界统一给出中文失败原因。
        print(f"实例分割参考对照失败：{error}", file=sys.stderr)
        raise SystemExit(1)
