#!/usr/bin/env python3
"""Physical B Primary F0/F1 v2 core/tail deletion 合同专项。"""

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
        pair_repetitions=3,
        role="estimation",
    )
    DESIGNER._assign_pair_roles(
        sequence, ["estimation", "selection", "confirmation"])
    DESIGNER._bind_sequence_semantics(sequence)
    return sequence


def phase_rotated_signals(
        sequence: dict, rotation: int) -> tuple[np.ndarray, np.ndarray]:
    positions = np.zeros(len(sequence["samples"]), dtype=np.float64)
    commands = np.zeros(len(sequence["samples"]), dtype=np.float64)
    original = np.asarray([
        sample["identification_input_x_counts"]
        for sample in sequence["samples"]
    ], dtype=np.float64)
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        levels = original[first:first + 63]
        rotated = np.roll(levels, -rotation)
        previous = 0.0
        for offset, level in enumerate(rotated):
            positions[first + offset] = level
            commands[first + offset] = level - previous
            previous = level
        commands[first + 63] = -previous
    return positions, commands


def synthetic_measurements(
        sequence: dict,
        *,
        delay: int = 4,
        gains: tuple[float, float] = (-0.45, -0.45),
        tail: tuple[float, ...] = (),
        input_shift: int = 0,
        phase_rotation: int = 0,
        response_only_drift: bool = False) -> dict[str, list[float]]:
    if phase_rotation:
        positions, commands = phase_rotated_signals(
            sequence, phase_rotation)
    else:
        positions = np.asarray([
            sample["identification_input_x_counts"]
            for sample in sequence["samples"]
        ], dtype=np.float64)
        commands = np.asarray([
            sample["command_dx_counts"] for sample in sequence["samples"]
        ], dtype=np.float64)
    outputs = [
        np.zeros(len(sequence["samples"]), dtype=np.float64),
        np.zeros(len(sequence["samples"]), dtype=np.float64),
    ]
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        for index in range(first - 32, first + 64 + 32):
            offset = index - (first - 1)
            nuisance = 0.012 * int(block["block_id"]) + 0.0003 * offset
            forced = []
            source = index - delay + input_shift
            for gain in gains:
                value = gain * positions[source]
                value += sum(
                    coefficient * commands[source - lag]
                    for lag, coefficient in enumerate(tail)
                )
                forced.append(value)
            if response_only_drift and index >= first:
                nuisance += 0.0002 * (offset ** 2)
            outputs[0][index] = nuisance + forced[0]
            outputs[1][index] = -0.4 * nuisance + forced[1]
    return {"left": outputs[0].tolist(), "right": outputs[1].tolist()}


def test_contract_freezes_predata_selection_and_validation_rules() -> None:
    contract = ANALYZER.physical_b_analysis_contract()
    expect(contract["schema_version"] == 2 and
           contract["model"]["family"] ==
           "delayed_static_gain_with_optional_relative_command_tail" and
           contract["model"]["core_delay_samples"] == 4 and
           contract["model"]["tail_lengths"] == [0, 1, 2, 4, 8] and
           contract["model"]["identification_input"] ==
           "cumulative_position_counts" and
           contract["model"]["tail_input"] ==
           "completed_command_dx_counts" and
           contract["model"]["output_feedback_used"] is False,
           "F0 v2 必须分开冻结 delay=4 static-gain core 与 relative-command tail")
    expect(contract["alternative_core_delays"] ==
           [1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16] and
           contract["split"]["random_frame_split_allowed"] is False and
           contract["split"]["pair_roles"] ==
           ["estimation", "selection", "confirmation"] and
           contract["split"]["nuisance_fit_rows"] ==
           "exact_dedicated_pre_guard_only" and
           contract["split"]["confirmation_used_for_refit"] is False,
           "F0 v2 必须冻结三段完整 pair 与 pre-guard-only nuisance")
    expect(contract["negative_controls"]["time_shifts"] ==
           [-16, -15, -14, -13, 13, 14, 15, 16] and
           contract["negative_controls"]["phase_rotations"] ==
           list(range(13, 51)) and
           contract["selection"]["metric_round_decimal_places"] == 12 and
           contract["selection"]["tie_break"][-1] == "lowest_tail_length",
           "F0 v2 必须冻结 time/phase controls 与最短 tail tie-break")
    expect(contract["failure_semantics"]["any_invalid_block_is_red"] is True and
           contract["failure_semantics"]["missing_rows_may_be_dropped"] is False and
           contract["failure_semantics"]["holdout_used_for_tuning"] is False,
           "缺帧、坏 block 与 holdout 调参必须 fail closed")
    expect(contract["contract_semantic_sha256"] ==
           ANALYZER.canonical_semantic_sha256(
               contract, "contract_semantic_sha256"),
           "分析合同必须可规范复算 SHA-256")


def test_primary_sequence_has_three_pairs_and_nonshared_guard_rows() -> None:
    sequence = frozen_sequence()
    expect(sequence["pair_roles"] ==
           ["estimation", "selection", "confirmation"] and
           sequence["summary"]["sample_count"] == 800 and
           len(sequence["blocks"]) == 6,
           "F0 v2 Primary 必须是三组完整 pair 与 800 个固定 sample")
    output_rows: set[int] = set()
    pre_guard_rows: set[int] = set()
    for block in sequence["blocks"]:
        first = int(block["first_sample_index"])
        current_pre = set(range(first - 32, first))
        current_output = set(range(first, first + 64 + 32))
        expect(current_pre.isdisjoint(current_output),
               "单 block 的 pre-guard 不能进入 response/post-guard score")
        expect(output_rows.isdisjoint(current_pre) and
               pre_guard_rows.isdisjoint(current_output),
               "不同 block 不得共享 nuisance 与 response/post-guard 模型行")
        pre_guard_rows.update(current_pre)
        output_rows.update(current_output)


def test_pure_core_freezes_f1_without_forcing_a_tail() -> None:
    sequence = frozen_sequence()
    result = ANALYZER.fit_primary_models(
        sequence, synthetic_measurements(sequence))
    expect(result["status"] == "PRIMARY_CORE_ONLY" and
           result["f1_kind"] == "F1_CORE" and
           result["selected_tail_length"] == 0 and
           result["primary_gate"]["ready_for_f1"] is True,
           "纯 delay+gain 数据必须得到 CORE_ONLY，不能为了更长模型判红")
    expect(result["core_model"]["delay_samples"] == 4 and
           result["core_gate"]["invalid_reasons"] == [] and
           result["tail_gate"]["retained"] is False,
           "core 必须独立通过，T=0 必须显式删除 tail")
    diagnostics = result["core_confirmation"]["witnesses"]["left"][
        "blocks"][0]["residual_input_diagnostics"]
    expect(len(diagnostics["completed_command_dx_counts"]) == 13 and
           len(diagnostics["cumulative_position_counts"]) == 13 and
           diagnostics["automatic_gate"] is False,
           "c/p lag 0..12 必须完整报告但不能充当自动门禁")


def test_known_tail_is_selected_refit_and_retained() -> None:
    sequence = frozen_sequence()
    result = ANALYZER.fit_primary_models(
        sequence, synthetic_measurements(sequence, tail=(-0.18, 0.07)))
    expect(result["status"] == "PRIMARY_CORE_PLUS_TAIL" and
           result["f1_kind"] == "F1_CORE_TAIL" and
           result["selected_tail_length"] == 2 and
           result["tail_gate"]["retained"] is True,
           "已知两 tap transient 必须按 selection pair 选 T=2 并在 confirmation 保留")
    expect(result["selected_model"]["fit_roles"] ==
           ["estimation", "selection"] and
           result["selected_model"]["confirmation_used_for_refit"] is False,
           "选中模型只能在 estimation+selection 重拟合一次")


def test_unconfirmed_tail_falls_back_to_the_frozen_core() -> None:
    sequence = frozen_sequence()
    measurements = synthetic_measurements(
        sequence, tail=(-0.18, 0.07))
    core_only = synthetic_measurements(sequence)
    for block in sequence["blocks"]:
        if block["role"] != "confirmation":
            continue
        first = int(block["first_sample_index"])
        begin = first - 32
        end = first + 64 + 32
        for witness in ("left", "right"):
            measurements[witness][begin:end] = core_only[witness][begin:end]

    result = ANALYZER.fit_primary_models(sequence, measurements)
    expect(result["status"] == "PRIMARY_CORE_ONLY" and
           result["f1_kind"] == "F1_CORE" and
           result["provisional_tail_selection"]
           ["selected_tail_length"] == 2 and
           result["selected_tail_length"] == 0 and
           result["selected_model"]["tail_length"] == 0 and
           result["selected_confirmation"] ==
           result["core_confirmation"],
           "tail 未通过 confirmation 时，F1_CORE 必须删除 tail 并冻结纯 core")


def test_nuisance_only_and_response_leakage_cannot_turn_green() -> None:
    sequence = frozen_sequence()
    nuisance_only = synthetic_measurements(sequence, gains=(0.0, 0.0))
    result = ANALYZER.fit_primary_models(sequence, nuisance_only)
    expect(result["status"] == "PRIMARY_RED" and
           result["primary_gate"]["ready_for_f1"] is False and
           "CORE_DOES_NOT_BEAT_NUISANCE" in
           result["core_gate"]["invalid_reasons"],
           "nuisance-only 输出不得生成 F1")

    leaked = ANALYZER.fit_primary_models(
        sequence,
        synthetic_measurements(
            sequence, gains=(0.0, 0.0), response_only_drift=True),
    )
    expect(leaked["primary_gate"]["ready_for_f1"] is False and
           all(entry["fit_source"] == "exact_dedicated_pre_guard_only"
               for witness in leaked["nuisance_by_witness"].values()
               for entry in witness.values()),
           "response/post-guard 输出不能回流拟合 nuisance 后伪造 green")


def test_wrong_delay_phase_and_time_alignment_are_deleted() -> None:
    sequence = frozen_sequence()
    wrong_delay = ANALYZER.fit_primary_models(
        sequence, synthetic_measurements(sequence, delay=5))
    expect(wrong_delay["status"] == "PRIMARY_RED" and
           "DELAY_4_NOT_REPLICATED" in
           wrong_delay["core_gate"]["invalid_reasons"],
           "真实 delay=5 必须推翻冻结 delay=4，不能事后改 F0")

    time_shifted = ANALYZER.fit_primary_models(
        sequence, synthetic_measurements(sequence, input_shift=13))
    expect(time_shifted["status"] == "PRIMARY_RED" and
           "REAL_ALIGNMENT_NOT_BETTER_THAN_TIME_CONTROLS" in
           time_shifted["core_gate"]["invalid_reasons"],
           "预注册 time shift 能解释输出时真实对齐必须判红")

    phase_shifted = ANALYZER.fit_primary_models(
        sequence, synthetic_measurements(sequence, phase_rotation=13))
    expect(phase_shifted["status"] == "PRIMARY_RED" and
           "REAL_ALIGNMENT_NOT_BETTER_THAN_PHASE_CONTROLS" in
           phase_shifted["core_gate"]["invalid_reasons"],
           "PRBS phase surrogate 能解释输出时真实对齐必须判红")


def test_witness_direction_mismatch_and_missing_rows_fail_closed() -> None:
    sequence = frozen_sequence()
    mismatch = ANALYZER.fit_primary_models(
        sequence,
        synthetic_measurements(sequence, gains=(-0.45, 0.45)),
    )
    expect(mismatch["status"] == "PRIMARY_RED" and
           ("GAIN_DIRECTION_MISMATCH" in
            mismatch["core_gate"]["invalid_reasons"] or
            "WITNESS_DIFFERENTIAL_COMMAND_LEAKAGE" in
            mismatch["core_gate"]["invalid_reasons"]),
           "左右 witness 方向不一致不得用 aggregate 隐藏")

    missing = synthetic_measurements(sequence)
    missing["left"] = missing["left"][:-1]
    rejected = False
    try:
        ANALYZER.fit_primary_models(sequence, missing)
    except ValueError:
        rejected = True
    expect(rejected, "缺任一 measurement row 必须 fail closed")


def test_png_witness_extraction_preserves_known_core_delay() -> None:
    sequence = frozen_sequence()
    positions = [
        int(sample["identification_input_x_counts"])
        for sample in sequence["samples"]
    ]
    left_roi = (16, 48, 96, 224)
    right_roi = (208, 48, 96, 224)
    rng = np.random.default_rng(20260903)
    base = rng.integers(0, 256, size=(320, 320, 3), dtype=np.uint8)
    base[48:272, 208:304] = base[48:272, 16:112]

    with tempfile.TemporaryDirectory(prefix="xen-probe-b-png-") as root_text:
        pixel_root = pathlib.Path(root_text) / "pixel-evidence"
        frames_root = pixel_root / "frames"
        frames_root.mkdir(parents=True)
        frame_records: dict[int, dict] = {}
        for delayed_position in (-1, 0, 1):
            image = base.copy()
            for roi, multiplier in ((left_roi, 2), (right_roi, 2)):
                x, y, width, height = roi
                image[y:y + height, x:x + width] = np.roll(
                    base[y:y + height, x:x + width],
                    -multiplier * delayed_position,
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
            delayed = positions[index - 4] if index >= 4 else 0
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
        expect(len(rows) == 768 and
               geometry["decoded_matched_frame_count"] == 768,
               "像素路径必须覆盖六个独立 pre/response/post block regions: "
               f"rows={len(rows)}, geometry={geometry}")
        result = ANALYZER.fit_primary_models(sequence, measurements)
        expect(result["status"] == "PRIMARY_CORE_ONLY" and
               result["core_model"]["delay_samples"] == 4,
               "PNG witness 提取后必须保留已知的 delay=4 static-gain core: "
               f"status={result['status']}, "
               f"reasons={result['core_gate']['invalid_reasons']}")


if __name__ == "__main__":
    test_contract_freezes_predata_selection_and_validation_rules()
    test_primary_sequence_has_three_pairs_and_nonshared_guard_rows()
    test_pure_core_freezes_f1_without_forcing_a_tail()
    test_known_tail_is_selected_refit_and_retained()
    test_unconfirmed_tail_falls_back_to_the_frozen_core()
    test_nuisance_only_and_response_leakage_cannot_turn_green()
    test_wrong_delay_phase_and_time_alignment_are_deleted()
    test_witness_direction_mismatch_and_missing_rows_fail_closed()
    test_png_witness_extraction_preserves_known_core_delay()
    print("Mouse Effect Probe Physical B analysis 测试全部通过。")
