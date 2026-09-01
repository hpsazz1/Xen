import importlib.util
import pathlib
import sys

import cv2
import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "analyze_mouse_effect_probe_pixels.py"
SPEC = importlib.util.spec_from_file_location("probe_pixel_analysis", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 mouse effect probe pixel analyzer")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_phase_translation_direction_and_quality() -> None:
    rng = np.random.default_rng(20260901)
    first = rng.normal(127.0, 35.0, (96, 128)).astype(np.float32)
    second = np.roll(first, shift=(-2, 3), axis=(0, 1))
    first_before = first.copy()
    second_before = second.copy()
    window = cv2.createHanningWindow((128, 96), cv2.CV_32F)
    measurement = MODULE.measure_translation(first, second, window)
    expect(abs(measurement["dx_px"] - 3.0) < 0.05,
           "src2 向右平移时 dx 必须为正")
    expect(abs(measurement["dy_px"] + 2.0) < 0.05,
           "src2 向上平移时 dy 必须为负")
    expect(np.isfinite(measurement["response"]) and
           measurement["response"] > 0.0,
           "phase response 必须作为连续质量值保留，不能截断成概率")
    expect(np.array_equal(first, first_before) and
           np.array_equal(second, second_before),
           "phaseCorrelate 的 Hann 加窗不得修改调用方持有的 witness 输入")


def test_roi_contract_and_distribution_summary() -> None:
    left = MODULE.parse_roi("8,4,32,48")
    right = MODULE.parse_roi("56,4,32,48")
    MODULE.validate_roi_pair(left, right, width=96, height=64)
    try:
        MODULE.validate_roi_pair(left, (24, 4, 32, 48), width=96, height=64)
    except ValueError:
        pass
    else:
        raise AssertionError("左右 witness ROI 重叠必须拒绝")
    summary = MODULE.distribution_summary([0.0, 1.0, 2.0, 3.0])
    expect(summary["count"] == 4 and summary["p50"] == 1.5 and
           summary["max"] == 3.0,
           "分布摘要必须保持样本数、中位数和最大值")


if __name__ == "__main__":
    test_phase_translation_direction_and_quality()
    test_roi_contract_and_distribution_summary()
    print("Mouse Effect Probe pixel analysis 测试全部通过。")
