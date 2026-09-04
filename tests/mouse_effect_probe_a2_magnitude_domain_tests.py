#!/usr/bin/env python3
"""A2 已录制幅度域与冻结 Physical B F1 的删除合同专项。"""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys

import numpy as np


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_module(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(
        "analyze_mouse_effect_probe_a2_magnitude_domain", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载分析器: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def make_block(
        block_id: str,
        sign: int,
        gains: tuple[float, float],
        *,
        delay: int = 4,
        perturbation: float = 0.0) -> dict:
    position = []
    current = 0
    for magnitude in range(1, 17):
        current = sign * magnitude
        position.extend([current] * 4)
    position.extend([sign * 16] * 64)
    for magnitude in range(15, -1, -1):
        current = sign * magnitude
        position.extend([current] * 4)
    expect(len(position) == 192 and position[-1] == 0,
           "合成 challenge 必须覆盖 192 samples 并回零")
    delayed = np.concatenate((
        np.zeros(delay, dtype=np.float64),
        np.asarray(position[:-delay], dtype=np.float64),
    ))
    outputs = {}
    for witness, gain in zip(("left", "right"), gains):
        shape = perturbation * np.sin(
            np.arange(192, dtype=np.float64) * 0.17)
        outputs[witness] = (gain * delayed + shape).tolist()
    return {
        "block_id": block_id,
        "position_x_counts": position,
        "witness_dx_px": outputs,
    }


def test_contract(analyzer) -> None:
    contract = analyzer.analysis_contract()
    expect(contract["schema_version"] == 1 and
           contract["physical_output_capability"] is False and
           contract["production_aim_changed"] is False,
           "幅度域分析必须是 output-off evidence-only 合同")
    expect(contract["measurement"]["left_roi"] == [16, 48, 96, 224] and
           contract["measurement"]["right_roi"] == [208, 48, 96, 224] and
           contract["model"]["delay_samples"] == 4 and
           contract["model"]["output_feedback_used"] is False,
           "必须复用 F1 witness/d4 且禁止输出反馈")
    expect(contract["split"] == {
        "fit": "primary_whole_blocks_only",
        "evaluation": "validation_whole_blocks_only",
        "random_frame_split_allowed": False,
    }, "Primary/Validation whole-block 角色必须冻结")
    expect(contract["contract_semantic_sha256"] ==
           analyzer.canonical_semantic_sha256(
               contract, "contract_semantic_sha256"),
           "分析合同 semantic SHA 必须可复算")


def test_primary_fit_rejects_f1_on_independent_validation(analyzer) -> None:
    primary = [
        make_block("PRIMARY-POSITIVE", 1, (-0.50, -0.51),
                   perturbation=0.01),
        make_block("PRIMARY-NEGATIVE", -1, (-0.50, -0.51),
                   perturbation=0.01),
    ]
    validation = [
        make_block("VALIDATION-NEGATIVE", -1, (-0.505, -0.515),
                   perturbation=0.01),
        make_block("VALIDATION-POSITIVE", 1, (-0.505, -0.515),
                   perturbation=0.01),
    ]
    result = analyzer.evaluate_domain(primary, validation)
    json.dumps(result, allow_nan=False)
    expect(result["status"] == "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" and
           result["f1_deleted_for_magnitude_domain"] is True and
           result["new_production_gain_claimed"] is False,
           "独立 validation 全 block 支持时只能删除 F1 幅度域外推，不能产出生产增益")
    expect(abs(result["primary_model"]["left_gain"] + 0.50) < 0.002 and
           abs(result["primary_model"]["right_gain"] + 0.51) < 0.002,
           "Primary 只能拟合自身 whole blocks")
    for witness in ("left", "right"):
        for block in result["validation"][witness]["blocks"]:
            expect(block["primary_model_rmse_px"] <
                   block["frozen_f1_rmse_px"] and
                   block["primary_model_max_abs_error_px"] <
                   block["frozen_f1_max_abs_error_px"],
                   "F1 删除必须在每个 validation whole block 的 RMSE/max 同时成立")


def test_aggregate_cannot_hide_one_f1_winning_block(analyzer) -> None:
    primary = [
        make_block("PRIMARY-POSITIVE", 1, (-0.50, -0.50)),
        make_block("PRIMARY-NEGATIVE", -1, (-0.50, -0.50)),
    ]
    validation = [
        make_block("VALIDATION-NEGATIVE", -1, (-0.50, -0.50)),
        make_block(
            "VALIDATION-POSITIVE", 1,
            (analyzer.FROZEN_F1_GAINS["left"],
             analyzer.FROZEN_F1_GAINS["right"])),
    ]
    result = analyzer.evaluate_domain(primary, validation)
    expect(result["status"] == "A2_MAGNITUDE_DOMAIN_UNRESOLVED" and
           result["f1_deleted_for_magnitude_domain"] is False,
           "aggregate 改善不得掩盖任一 whole block 的 F1 胜出")


def test_wrong_delay_and_incomplete_coverage_fail_closed(analyzer) -> None:
    primary = [
        make_block("PRIMARY-POSITIVE", 1, (-0.50, -0.50), delay=5),
        make_block("PRIMARY-NEGATIVE", -1, (-0.50, -0.50), delay=5),
    ]
    validation = [
        make_block("VALIDATION-NEGATIVE", -1, (-0.50, -0.50), delay=5),
        make_block("VALIDATION-POSITIVE", 1, (-0.50, -0.50), delay=5),
    ]
    wrong_delay = analyzer.evaluate_domain(primary, validation)
    expect(wrong_delay["status"] == "A2_MAGNITUDE_DOMAIN_UNRESOLVED" and
           "DELAY_4_NOT_BEST_ON_PRIMARY" in wrong_delay["invalid_reasons"],
           "真实 d5 不得被 d4 幅度拟合吞掉")

    incomplete = [dict(primary[0]), dict(primary[1])]
    incomplete[1]["position_x_counts"] = \
        incomplete[1]["position_x_counts"][:-1]
    rejected = False
    try:
        analyzer.evaluate_domain(incomplete, validation)
    except ValueError:
        rejected = True
    expect(rejected, "缺 sample 或不完整幅度覆盖必须 fail closed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    analyzer = load_module(arguments.analyzer.resolve())
    test_contract(analyzer)
    test_primary_fit_rejects_f1_on_independent_validation(analyzer)
    test_aggregate_cannot_hide_one_f1_winning_block(analyzer)
    test_wrong_delay_and_incomplete_coverage_fail_closed(analyzer)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
