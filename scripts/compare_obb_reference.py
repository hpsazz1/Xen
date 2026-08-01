"""将 Xen OBB 诊断结果与 Ultralytics 参考后处理做数值对照。"""

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
    parser.add_argument(
        "--maximum-geometry-delta", type=float, default=1e-3
    )
    parser.add_argument("--maximum-angle-delta", type=float, default=1e-5)
    parser.add_argument(
        "--maximum-confidence-delta", type=float, default=1e-5
    )
    return parser.parse_args()


def pair_instances(instances, reference_data):
    """按同类旋转几何一一配对，允许不同 Provider 改变近似同分目标顺序。"""
    available_indices = set(range(len(instances)))
    pairs = []
    for reference_index, expected in enumerate(reference_data):
        class_id = int(expected[6])
        candidates = [
            index
            for index in available_indices
            if int(instances[index]["class_id"]) == class_id
        ]
        if not candidates:
            raise RuntimeError(
                f"参考实例 {reference_index} 的类别 {class_id} 没有可配对结果"
            )

        # OBB 输出按置信度排序；CUDA 等 Provider 的微小数值差异可能让
        # 两个近似同分目标交换次序。以 xywh 的最大绝对差配对，仍由后续
        # 独立门槛约束几何、角度与置信度，不能借重排隐藏错误检测结果。
        actual_index = min(
            candidates,
            key=lambda index: (
                max(
                    abs(float(instances[index][key]) - float(expected[offset]))
                    for offset, key in enumerate(
                        ("center_x", "center_y", "width", "height")
                    )
                ),
                index,
            ),
        )
        available_indices.remove(actual_index)
        pairs.append(
            (reference_index, actual_index, instances[actual_index], expected)
        )
    if available_indices:
        raise RuntimeError("Xen 结果存在未配对的 OBB 实例")
    return pairs


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
    reference = YOLO(str(args.model), task="obb").predict(
        str(args.image),
        imgsz=320,
        conf=0.25,
        iou=0.45,
        max_det=300,
        device="cpu",
        verbose=False,
    )[0]
    if reference.obb is None:
        raise RuntimeError("Ultralytics 参考结果没有旋转框")
    reference_data = reference.obb.data.cpu().numpy()
    instances = summary.get("instances", [])
    if len(instances) != len(reference_data) or not instances:
        raise RuntimeError(
            "Xen 与 Ultralytics OBB 实例数不一致或结果为空："
            f"{len(instances)} != {len(reference_data)}"
        )

    maximum_geometry_delta = 0.0
    maximum_angle_delta = 0.0
    maximum_confidence_delta = 0.0
    reordered_instances = 0
    for reference_index, actual_index, instance, expected in pair_instances(
        instances, reference_data
    ):
        if reference_index != actual_index:
            reordered_instances += 1
        actual_geometry = np.asarray(
            [
                instance["center_x"],
                instance["center_y"],
                instance["width"],
                instance["height"],
            ],
            dtype=np.float32,
        )
        maximum_geometry_delta = max(
            maximum_geometry_delta,
            float(np.max(np.abs(actual_geometry - expected[:4]))),
        )
        maximum_angle_delta = max(
            maximum_angle_delta,
            abs(float(instance["angle_radians"]) - float(expected[4])),
        )
        maximum_confidence_delta = max(
            maximum_confidence_delta,
            abs(float(instance["confidence"]) - float(expected[5])),
        )

    result = {
        "instances": len(instances),
        "reordered_instances": reordered_instances,
        "maximum_geometry_delta_pixels": maximum_geometry_delta,
        "maximum_angle_delta_radians": maximum_angle_delta,
        "maximum_confidence_delta": maximum_confidence_delta,
    }
    print(json.dumps(result, ensure_ascii=False))

    if maximum_geometry_delta > args.maximum_geometry_delta:
        raise RuntimeError(
            f"最大 OBB 几何偏差 {maximum_geometry_delta:.8f} 超过门槛 "
            f"{args.maximum_geometry_delta:.8f}"
        )
    if maximum_angle_delta > args.maximum_angle_delta:
        raise RuntimeError(
            f"最大角度偏差 {maximum_angle_delta:.8f} 超过门槛 "
            f"{args.maximum_angle_delta:.8f}"
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
    except Exception as error:  # noqa: BLE001 - 命令行边界统一输出中文原因。
        print(f"OBB 参考对照失败：{error}", file=sys.stderr)
        raise SystemExit(1)
