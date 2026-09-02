#!/usr/bin/env python3
"""Physical B Primary F0/F1 因果 FIR 选择合同专项。"""

from __future__ import annotations

import importlib.util
import hashlib
import pathlib
import sys
import tempfile

import cv2
import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载测试模块: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ANALYZER = load_module(
    ROOT / "scripts" / "analyze_mouse_effect_probe_b.py",
    "analyze_mouse_effect_probe_b",
)
DESIGNER = load_module(
    ROOT / "scripts" / "design_mouse_effect_probe_prbs.py",
    "design_mouse_effect_probe_prbs_for_b_analysis_tests",
)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def frozen_sequence() -> dict:
    bits = DESIGNER.generate_maximum_length_period(
        order=6, feedback_mask=0x27, seed=1, phase=49)
    sequence = DESIGNER.build_candidate_sequence(
        period_bits=bits,
        input_definition="cumulative_position_counts",
        guard_sample_count=32,
        pair_repetitions=2,
        role="estimation",
    )
    DESIGNER._assign_pair_roles(
        sequence, ["estimation", "within_run_validation"])
    DESIGNER._bind_sequence_semantics(sequence)
    return sequence


def synthetic_measurements(sequence: dict) -> dict[str, list[float]]:
    samples = sequence["samples"]
    positions = np.asarray([
        sample["identification_input_x_counts"] for sample in samples
    ], dtype=np.float64)
    left = np.zeros(len(samples), dtype=np.float64)
    right = np.zeros(len(samples), dtype=np.float64)
    left_impulse = np.asarray(
        [-0.10, -0.16, -0.20, -0.17, -0.12, -0.08, -0.04, -0.02],
        dtype=np.float64,
    )
    right_impulse = left_impulse * 1.015
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        output_end = first + int(block["sample_count"]) + 32
        for index in range(first, output_end):
            row = np.asarray([
                positions[index - lag]
                for lag in range(1, len(left_impulse) + 1)
            ])
            fraction = (index - first) / max(output_end - first - 1, 1)
            nuisance = 0.015 * int(block["block_id"]) + 0.01 * fraction
            left[index] = nuisance + float(row @ left_impulse)
            right[index] = -0.5 * nuisance + float(row @ right_impulse)
    return {"left": left.tolist(), "right": right.tolist()}


def test_contract_freezes_predata_selection_and_validation_rules() -> None:
    contract = ANALYZER.physical_b_analysis_contract()
    expect(contract["schema_version"] == 1 and
           contract["model"]["family"] == "strictly_causal_fir" and
           contract["model"]["lag_origin"] == 1 and
           contract["model"]["identification_input"] ==
           "cumulative_position_counts" and
           contract["model"]["output_feedback_used"] is False,
           "F0 必须冻结 lag=1 的 position-to-pixel FIR，不能混入 measured output")
    expect(contract["candidate_horizons"] == [4, 8, 16, 32] and
           contract["deletion_control_horizons"] == [4] and
           contract["acceptance_eligible_horizons"] == [8, 16, 32] and
           contract["split"]["random_frame_split_allowed"] is False and
           contract["split"]["estimation_pair_index"] == 1 and
           contract["split"]["within_run_validation_pair_index"] == 2,
           "候选 H 与整 pair split 必须在任何 Physical B data 前冻结")
    expect(contract["validation"]["input_forced_required"] is True and
           contract["validation"]["output_free_run_required"] is True and
           contract["validation"]["output_free_run_equivalent_for_fir"] is True and
           contract["selection"]["metric_round_decimal_places"] == 12 and
           contract["selection"]["tie_break"][-1] == "lowest_horizon",
           "F0 必须冻结 forced/free-run 与确定性 tie-break")
    expect(contract["failure_semantics"]["any_invalid_block_is_red"] is True and
           contract["failure_semantics"]["missing_rows_may_be_dropped"] is False and
           contract["failure_semantics"]["holdout_used_for_tuning"] is False,
           "缺帧、坏 block 与 holdout 调参必须 fail closed")
    expect(contract["contract_semantic_sha256"] ==
           ANALYZER.canonical_semantic_sha256(
               contract, "contract_semantic_sha256"),
           "分析合同必须可规范复算 SHA-256")


def test_primary_selects_smallest_supported_h_and_beats_deletion_control() -> None:
    sequence = frozen_sequence()
    result = ANALYZER.fit_primary_models(
        sequence,
        synthetic_measurements(sequence),
        candidate_horizons=[4, 8, 16, 32],
        acceptance_eligible_horizons=[8, 16, 32],
    )
    expect(result["status"] == "PRIMARY_MODEL_SELECTED" and
           result["selected_horizon"] == 8 and
           result["primary_gate"]["ready_for_f1"] is True,
           "真实支撑到 H=8 时必须选择最小同分 eligible H，并允许形成 F1")
    selected = result["candidates"]["8"]
    deletion = result["candidates"]["4"]
    expect(selected["validation"]["worst_witness_rmse_px"] <
           deletion["validation"]["worst_witness_rmse_px"] and
           selected["validation"]["worst_witness_rmse_px"] <
           result["nuisance_only_validation"]["worst_witness_rmse_px"] and
           selected["validation"]["max_abs_residual_past_input_correlation"] <=
           deletion["validation"]["max_abs_residual_past_input_correlation"],
           "selected model 必须在整 validation pair 上同时胜过 H=4 与 nuisance-only")
    for witness in ("left", "right"):
        coefficients = selected["estimation"][witness]["fir_coefficients"]
        expect(len(coefficients) == 8 and
               all(np.isfinite(coefficients)),
               "F1 候选必须保留每个 witness 的有限 FIR 系数")
    expect(selected["validation"]["input_forced"] ==
           selected["validation"]["output_free_run"],
           "无 output feedback 的 FIR 必须显式证明 forced/free-run 等价")


def test_primary_fails_closed_when_eligible_models_do_not_beat_h4() -> None:
    sequence = frozen_sequence()
    flat = {"left": [0.0] * 416, "right": [0.0] * 416}
    result = ANALYZER.fit_primary_models(
        sequence,
        flat,
        candidate_horizons=[4, 8, 16, 32],
        acceptance_eligible_horizons=[8, 16, 32],
    )
    expect(result["status"] == "PRIMARY_RED" and
           result["primary_gate"]["ready_for_f1"] is False and
           "SELECTED_MODEL_DOES_NOT_BEAT_H4" in
           result["primary_gate"]["invalid_reasons"],
           "没有 input-forced 改善的静态输出不得生成 F1")


def test_png_witness_extraction_preserves_known_eight_sample_delay() -> None:
    sequence = frozen_sequence()
    positions = [
        int(sample["identification_input_x_counts"])
        for sample in sequence["samples"]
    ]
    left_roi = (16, 48, 96, 224)
    right_roi = (208, 48, 96, 224)
    rng = np.random.default_rng(20260903)
    base = rng.integers(0, 256, size=(320, 320, 3), dtype=np.uint8)

    with tempfile.TemporaryDirectory(prefix="xen-probe-b-png-") as root_text:
        pixel_root = pathlib.Path(root_text) / "pixel-evidence"
        frames_root = pixel_root / "frames"
        frames_root.mkdir(parents=True)
        frame_records: dict[int, dict] = {}
        for delayed_position in (-1, 0, 1):
            image = base.copy()
            for roi, multiplier in ((left_roi, 2), (right_roi, 3)):
                x, y, width, height = roi
                image[y:y + height, x:x + width] = np.roll(
                    base[y:y + height, x:x + width],
                    multiplier * delayed_position,
                    axis=1,
                )
            relative = pathlib.Path("frames") / \
                f"state-{delayed_position:+d}.png"
            path = pixel_root / relative
            expect(cv2.imwrite(str(path), image), "测试 PNG 写入失败")
            decoded = cv2.imread(str(path), cv2.IMREAD_COLOR)
            expect(decoded is not None, "测试 PNG 解码失败")
            frame_records[delayed_position] = {
                "file": relative.as_posix(),
                "png_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "bgr_sha256": hashlib.sha256(
                    np.ascontiguousarray(decoded).tobytes()).hexdigest(),
            }

        matched = []
        for index, sample in enumerate(sequence["samples"]):
            delayed = positions[index - 8] if index >= 8 else 0
            frame = {
                **frame_records[delayed],
                "source_clock_session_id": "test-clock-session",
            }
            event = {
                "source_timestamp": 100000 + index,
                "source_clock_session_id": "test-clock-session",
            }
            matched.append((event, sample, frame, index))

        loaded = {
            "task": {
                "sidecar": {
                    "roi_width": 320,
                    "roi_height": 320,
                    "left_witness_roi": "16,48,96,224",
                    "right_witness_roi": "208,48,96,224",
                }
            },
            "paths": {"manifest": pixel_root / "manifest.json"},
            "offline_sequence": sequence,
        }
        measurements, rows, geometry = ANALYZER._measure_primary_witnesses(
            loaded, matched)
        expect(len(rows) == 384 and
               geometry["decoded_matched_frame_count"] == 385,
               "像素路径必须覆盖首个 anchor 与 384 个连续完整 block rows: "
               f"rows={len(rows)}, geometry={geometry}")
        result = ANALYZER.fit_primary_models(
            sequence,
            measurements,
            candidate_horizons=[4, 8, 16, 32],
            acceptance_eligible_horizons=[8, 16, 32],
        )
        expect(result["status"] == "PRIMARY_MODEL_SELECTED" and
               result["selected_horizon"] == 8,
               "PNG witness 提取后必须保留已知的 8-sample 严格因果延迟")


if __name__ == "__main__":
    test_contract_freezes_predata_selection_and_validation_rules()
    test_primary_selects_smallest_supported_h_and_beats_deletion_control()
    test_primary_fails_closed_when_eligible_models_do_not_beat_h4()
    test_png_witness_extraction_preserves_known_eight_sample_delay()
    print("Mouse Effect Probe Physical B analysis 测试全部通过。")
