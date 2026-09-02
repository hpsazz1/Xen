#!/usr/bin/env python3
"""离线生成并审计 Mouse Effect Probe Physical B PRBS 候选。"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import sys

import numpy as np


INPUT_DEFINITIONS = {
    "direct_command_counts",
    "cumulative_position_counts",
}


def generate_maximum_length_period(
        order: int,
        feedback_mask: int,
        seed: int,
        phase: int) -> list[int]:
    """按公开的 LSB 输出/右移 recurrence 生成并证明一个完整周期。"""
    if order < 2 or order > 20:
        raise ValueError("PRBS order 必须位于 [2, 20]")
    state_limit = 1 << order
    if feedback_mask <= 0 or feedback_mask >= state_limit:
        raise ValueError("feedback_mask 必须位于 order-bit 非零范围")
    if seed <= 0 or seed >= state_limit:
        raise ValueError("seed 必须是 order-bit 非零状态")
    if phase < 0:
        raise ValueError("phase 不能为负")

    expected_period = state_limit - 1
    state = seed
    seen: set[int] = set()
    bits: list[int] = []
    for _ in range(expected_period):
        if state == 0 or state in seen:
            break
        seen.add(state)
        bits.append(state & 1)
        feedback = (state & feedback_mask).bit_count() & 1
        state = (state >> 1) | (feedback << (order - 1))

    if len(bits) != expected_period or state != seed:
        raise ValueError(
            "feedback_mask/seed 未形成覆盖全部非零状态的 maximum-length 周期"
        )
    offset = phase % expected_period
    return bits[offset:] + bits[:offset]


def build_candidate_sequence(
        period_bits: list[int],
        input_definition: str,
        guard_sample_count: int,
        pair_repetitions: int,
        role: str) -> dict:
    """构造完整周期/反相周期；只返回离线整数合同，不含执行能力。"""
    if input_definition not in INPUT_DEFINITIONS:
        raise ValueError("未知 PRBS input definition")
    if not period_bits or any(bit not in (0, 1) for bit in period_bits):
        raise ValueError("period_bits 必须是非空二值完整周期")
    if guard_sample_count <= 0 or pair_repetitions <= 0 or not role:
        raise ValueError("guard、pair repetitions 与 role 必须为正/非空")

    bipolar = [1 if bit else -1 for bit in period_bits]
    if input_definition == "direct_command_counts":
        period_input = bipolar
    else:
        period_input = list(period_bits)
    inverted_input = [-value for value in period_input]

    samples: list[dict] = []
    blocks: list[dict] = []
    position_x = 0
    maximum_position = 0

    def append_sample(
            pair_index: int,
            block_id: int,
            phase: str,
            identification_input: int,
            command_dx: int,
            prbs_index: int | None = None,
            prbs_bit: int | None = None) -> None:
        nonlocal position_x, maximum_position
        if command_dx not in (-1, 0, 1):
            raise ValueError("PRBS candidate 产生了超出 ±1 的差分命令")
        position_x += command_dx
        maximum_position = max(maximum_position, abs(position_x))
        samples.append({
            "sample_index": len(samples),
            "pair_index": pair_index,
            "block_id": block_id,
            "role": role,
            "phase": phase,
            "prbs_index": prbs_index,
            "prbs_bit": prbs_bit,
            "identification_input_x_counts": identification_input,
            "command_dx_counts": command_dx,
            "command_dy_counts": 0,
            "position_x_counts": position_x,
        })

    def append_guard(pair_index: int) -> None:
        for _ in range(guard_sample_count):
            append_sample(pair_index, 0, "guard", 0, 0)

    def append_period(pair_index: int, block_id: int, inverted: bool) -> None:
        nonlocal position_x
        values = inverted_input if inverted else period_input
        first_sample_index = len(samples)
        for prbs_index, (bit, value) in enumerate(zip(period_bits, values)):
            if input_definition == "direct_command_counts":
                command_dx = value
            else:
                command_dx = value - position_x
            append_sample(
                pair_index,
                block_id,
                "inverted_period" if inverted else "period",
                value,
                command_dx,
                prbs_index,
                bit,
            )
        if input_definition == "cumulative_position_counts":
            append_sample(
                pair_index,
                block_id,
                "return_to_zero",
                0,
                -position_x,
            )
        blocks.append({
            "block_id": block_id,
            "pair_index": pair_index,
            "role": role,
            "polarity": "inverted" if inverted else "normal",
            "first_sample_index": first_sample_index,
            "period_sample_count": len(period_bits),
            "sample_count": len(samples) - first_sample_index,
        })

    append_guard(0)
    block_id = 1
    for pair_index in range(1, pair_repetitions + 1):
        append_period(pair_index, block_id, inverted=False)
        block_id += 1
        append_guard(pair_index)
        append_period(pair_index, block_id, inverted=True)
        block_id += 1
        append_guard(pair_index)

    if position_x != 0:
        raise ValueError("完整正/反相周期未回到零位置")
    return {
        "input_definition": input_definition,
        "physical_output_capability": False,
        "period": {
            "bits": list(period_bits),
            "identification_input": period_input,
            "inverted_identification_input": inverted_input,
        },
        "guard_sample_count": guard_sample_count,
        "pair_repetitions": pair_repetitions,
        "blocks": blocks,
        "samples": samples,
        "summary": {
            "sample_count": len(samples),
            "net_command_dx_counts": position_x,
            "max_abs_position_x_counts": maximum_position,
            "all_commands_x_only_single_count": True,
        },
    }


def _matrix_metrics(matrix: np.ndarray) -> dict:
    singular_values = np.linalg.svd(matrix, compute_uv=False)
    largest = float(singular_values[0]) if singular_values.size else 0.0
    tolerance = (
        max(matrix.shape, default=0) * largest * np.finfo(np.float64).eps
    )
    rank = int(np.count_nonzero(singular_values > tolerance))
    full_column_rank = rank == matrix.shape[1]
    condition_number = None
    if full_column_rank and singular_values.size:
        smallest = float(singular_values[-1])
        if smallest > 0.0:
            condition_number = largest / smallest
    return {
        "shape": [int(matrix.shape[0]), int(matrix.shape[1])],
        "rank": rank,
        "rank_tolerance": tolerance,
        "full_column_rank": full_column_rank,
        "singular_values": [float(value) for value in singular_values],
        "condition_number": condition_number,
    }


def audit_identification_period(
        identification_input: list[int],
        horizons: list[int]) -> dict:
    """审计确切完整周期，不用 PRBS 标签替代 rank/频带事实。"""
    if not identification_input:
        raise ValueError("identification period 不能为空")
    if not horizons or any(horizon <= 0 for horizon in horizons) or \
            len(set(horizons)) != len(horizons):
        raise ValueError("horizons 必须是非空、正数且不重复")
    period = np.asarray(identification_input, dtype=np.float64)
    period_sample_count = int(period.size)
    power = np.abs(np.fft.fft(period)) ** 2
    non_dc = power[1:]
    power_tolerance = (
        max(1.0, float(np.max(power))) * period_sample_count *
        np.finfo(np.float64).eps
    )
    frequency_lines = []
    for index, value in enumerate(power):
        signed_index = index if index <= period_sample_count // 2 \
            else index - period_sample_count
        frequency_lines.append({
            "bin": index,
            "cycles_per_sample": signed_index / period_sample_count,
            "power": float(value),
        })

    horizon_audits = []
    for horizon in horizons:
        circular = np.asarray([
            [period[(row - column - 1) % period_sample_count]
             for column in range(horizon)]
            for row in range(period_sample_count)
        ], dtype=np.float64)
        horizon_audits.append({
            "horizon": horizon,
            "circular_window_matrix": _matrix_metrics(circular),
        })

    return {
        "period_sample_count": period_sample_count,
        "frequency": {
            "dft_normalization": "unnormalized_squared_magnitude",
            "dc_power": float(power[0]),
            "non_dc_line_count": max(period_sample_count - 1, 0),
            "nonzero_non_dc_line_count": int(np.count_nonzero(
                non_dc > power_tolerance)),
            "non_dc_power_min": float(np.min(non_dc))
                if non_dc.size else 0.0,
            "non_dc_power_max": float(np.max(non_dc))
                if non_dc.size else 0.0,
            "lines": frequency_lines,
        },
        "horizons": horizon_audits,
        "all_circular_window_matrices_full_column_rank": all(
            entry["circular_window_matrix"]["full_column_rank"]
            for entry in horizon_audits
        ),
    }


def _frequency_metrics(values: list[int]) -> dict:
    if not values:
        raise ValueError("frequency input 不能为空")
    data = np.asarray(values, dtype=np.float64)
    power = np.abs(np.fft.fft(data)) ** 2
    sample_count = int(data.size)
    non_dc = power[1:]
    tolerance = (
        max(1.0, float(np.max(power))) * sample_count *
        np.finfo(np.float64).eps
    )
    lines = []
    for index, value in enumerate(power):
        signed_index = index if index <= sample_count // 2 \
            else index - sample_count
        lines.append({
            "bin": index,
            "cycles_per_sample": signed_index / sample_count,
            "power": float(value),
        })
    return {
        "sample_count": sample_count,
        "dft_normalization": "unnormalized_squared_magnitude",
        "dc_power": float(power[0]),
        "non_dc_line_count": max(sample_count - 1, 0),
        "nonzero_non_dc_line_count": int(np.count_nonzero(
            non_dc > tolerance)),
        "non_dc_power_min": float(np.min(non_dc))
            if non_dc.size else 0.0,
        "non_dc_power_max": float(np.max(non_dc))
            if non_dc.size else 0.0,
        "lines": lines,
    }


def _exact_schedule_audit(sequence: dict, horizons: list[int]) -> dict:
    samples = sequence["samples"]
    identification_input = [
        int(sample["identification_input_x_counts"]) for sample in samples
    ]
    actual_command = [int(sample["command_dx_counts"]) for sample in samples]
    block_frequencies = []
    all_blocks_return = True
    for block in sequence["blocks"]:
        first = block["first_sample_index"]
        end = first + block["sample_count"]
        block_samples = samples[first:end]
        start_position = (
            block_samples[0]["position_x_counts"] -
            block_samples[0]["command_dx_counts"]
        )
        end_position = block_samples[-1]["position_x_counts"]
        returns_to_anchor = end_position == start_position
        all_blocks_return = all_blocks_return and returns_to_anchor
        block_frequencies.append({
            "block_id": block["block_id"],
            "pair_index": block["pair_index"],
            "role": block["role"],
            "polarity": block["polarity"],
            "net_command_dx_counts": sum(
                sample["command_dx_counts"] for sample in block_samples),
            "max_abs_position_from_block_anchor_counts": max(
                abs(sample["position_x_counts"] - start_position)
                for sample in block_samples
            ),
            "returns_to_block_anchor": returns_to_anchor,
            "identification_input": _frequency_metrics([
                sample["identification_input_x_counts"]
                for sample in block_samples
            ]),
            "actual_command": _frequency_metrics([
                sample["command_dx_counts"] for sample in block_samples
            ]),
        })

    horizon_audits = []
    all_input_full_rank = True
    all_augmented_identifiable = True
    for horizon in horizons:
        block_audits = []
        for block in sequence["blocks"]:
            first = block["first_sample_index"]
            block_end = first + block["sample_count"]
            output_end = block_end
            while output_end < len(samples) and \
                    samples[output_end]["phase"] == "guard":
                output_end += 1
            output_indices = list(range(first, output_end))
            unknown_prehistory = max(0, horizon - first)
            if unknown_prehistory:
                input_metrics = {
                    "shape": [len(output_indices), horizon],
                    "rank": 0,
                    "rank_tolerance": None,
                    "full_column_rank": False,
                    "singular_values": [],
                    "condition_number": None,
                }
                nuisance_metrics = _matrix_metrics(np.asarray([
                    [1.0, row / max(len(output_indices) - 1, 1)]
                    for row in range(len(output_indices))
                ], dtype=np.float64))
                augmented_metrics = {
                    "shape": [len(output_indices), horizon + 2],
                    "rank": nuisance_metrics["rank"],
                    "rank_tolerance": None,
                    "full_column_rank": False,
                    "singular_values": [],
                    "condition_number": None,
                }
            else:
                input_matrix = np.asarray([
                    [identification_input[index - lag]
                     for lag in range(1, horizon + 1)]
                    for index in output_indices
                ], dtype=np.float64)
                nuisance_matrix = np.asarray([
                    [1.0, row / max(len(output_indices) - 1, 1)]
                    for row in range(len(output_indices))
                ], dtype=np.float64)
                input_metrics = _matrix_metrics(input_matrix)
                nuisance_metrics = _matrix_metrics(nuisance_matrix)
                augmented_metrics = _matrix_metrics(np.hstack(
                    (nuisance_matrix, input_matrix)))
            augmented_identifiable = (
                augmented_metrics["rank"] ==
                nuisance_metrics["rank"] + horizon
            )
            all_input_full_rank = (
                all_input_full_rank and input_metrics["full_column_rank"]
            )
            all_augmented_identifiable = (
                all_augmented_identifiable and augmented_identifiable
            )
            block_audits.append({
                "block_id": block["block_id"],
                "pair_index": block["pair_index"],
                "role": block["role"],
                "polarity": block["polarity"],
                "output_sample_index_start": first,
                "output_sample_index_end_exclusive": output_end,
                "prehistory_source": "exact_sequence_samples"
                    if unknown_prehistory == 0 else "unknown_run_history",
                "unknown_prehistory_sample_count": unknown_prehistory,
                "input_matrix": input_metrics,
                "nuisance_columns": [
                    "constant_1", "linear_block_fraction_0_to_1"
                ],
                "nuisance_matrix": nuisance_metrics,
                "augmented_design_matrix": augmented_metrics,
                "augmented_design_identifiable": augmented_identifiable,
            })
        horizon_audits.append({
            "horizon": horizon,
            "blocks": block_audits,
        })

    return {
        "matrix_semantic": (
            "U_H[row,lag-1]=exact_identification_input[k_row-lag]; "
            "lag starts at 1; rows cover period/return plus exact post-guard"
        ),
        "unknown_prehistory_zero_padding_allowed": False,
        "all_input_matrices_full_column_rank": all_input_full_rank,
        "all_augmented_designs_identifiable": all_augmented_identifiable,
        "all_blocks_return_to_anchor": all_blocks_return,
        "frequency": {
            "identification_input_full_sequence":
                _frequency_metrics(identification_input),
            "actual_command_full_sequence": _frequency_metrics(actual_command),
            "blocks": block_frequencies,
        },
        "horizons": horizon_audits,
    }


def _maximum_length_feedback_masks(order: int) -> list[int]:
    masks: list[int] = []
    for feedback_mask in range(1, 1 << order):
        try:
            generate_maximum_length_period(
                order=order,
                feedback_mask=feedback_mask,
                seed=1,
                phase=0,
            )
        except ValueError:
            continue
        masks.append(feedback_mask)
    if not masks:
        raise ValueError(f"order={order} 没有找到 maximum-length recurrence")
    return masks


def _prefix_extent(values: list[int]) -> int:
    position = 0
    maximum = 0
    for value in values:
        position += value
        maximum = max(maximum, abs(position))
    return maximum


def _ranked_recurrences(order: int) -> list[dict]:
    ranked: list[tuple[tuple[int, int, int, int], dict]] = []
    for feedback_mask in _maximum_length_feedback_masks(order):
        original = generate_maximum_length_period(
            order=order,
            feedback_mask=feedback_mask,
            seed=1,
            phase=0,
        )
        for phase in range(len(original)):
            bits = original[phase:] + original[:phase]
            bipolar = [1 if bit else -1 for bit in bits]
            key = (
                _prefix_extent(bipolar + [-value for value in bipolar]),
                _prefix_extent(bipolar),
                feedback_mask,
                phase,
            )
            ranked.append((key, {
                "order": order,
                "feedback_mask": feedback_mask,
                "feedback_mask_hex": f"0x{feedback_mask:x}",
                "seed": 1,
                "phase": phase,
                "period_sample_count": len(bits),
                "recurrence": (
                    "output=state_lsb; feedback=parity(state & "
                    "feedback_mask); state=(state>>1)|(feedback<<(order-1))"
                ),
                "maximum_length_proven_by_state_cycle": True,
                "bits": bits,
                "direct_pair_prefix_extent": key[0],
                "direct_period_prefix_extent": key[1],
            }))
    ranked.sort(key=lambda entry: entry[0])
    return [entry for _, entry in ranked]


def _assign_pair_roles(sequence: dict, roles: list[str]) -> None:
    if sequence["pair_repetitions"] != len(roles):
        raise ValueError("pair role 数量与完整 pair 重复数不一致")
    for sample in sequence["samples"]:
        pair_index = sample["pair_index"]
        if pair_index > 0:
            sample["role"] = roles[pair_index - 1]
    for block in sequence["blocks"]:
        block["role"] = roles[block["pair_index"] - 1]
    sequence["pair_roles"] = roles


def _bind_sequence_semantics(sequence: dict) -> None:
    payload = json.dumps(
        sequence,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    sequence["sequence_semantic_sha256"] = hashlib.sha256(payload).hexdigest()


def _audit_candidate(
        lfsr: dict,
        input_definition: str,
        horizons: list[int],
        guard_sample_count: int,
        observed_lag: int,
        pair_roles: list[str]) -> dict:
    sequence = build_candidate_sequence(
        period_bits=lfsr["bits"],
        input_definition=input_definition,
        guard_sample_count=guard_sample_count,
        pair_repetitions=len(pair_roles),
        role=pair_roles[0],
    )
    _assign_pair_roles(sequence, pair_roles)
    _bind_sequence_semantics(sequence)
    audit = audit_identification_period(
        sequence["period"]["identification_input"], horizons)
    for entry in audit["horizons"]:
        entry["observed_first_visible_lag_covered"] = (
            entry["horizon"] >= observed_lag
        )
        entry["candidate_guard_sample_count"] = guard_sample_count
        entry["guard_covers_horizon"] = (
            guard_sample_count >= entry["horizon"]
        )
    exact_schedule_audit = _exact_schedule_audit(sequence, horizons)
    full_rank = (
        audit["all_circular_window_matrices_full_column_rank"] and
        exact_schedule_audit["all_input_matrices_full_column_rank"] and
        exact_schedule_audit["all_augmented_designs_identifiable"]
    )
    conditions = [
        entry["circular_window_matrix"]["condition_number"]
        for entry in audit["horizons"]
        if entry["circular_window_matrix"]["condition_number"] is not None
    ]
    exact_input_conditions = [
        block["input_matrix"]["condition_number"]
        for entry in exact_schedule_audit["horizons"]
        for block in entry["blocks"]
        if block["input_matrix"]["condition_number"] is not None
    ]
    exact_augmented_conditions = [
        block["augmented_design_matrix"]["condition_number"]
        for entry in exact_schedule_audit["horizons"]
        for block in entry["blocks"]
        if block["augmented_design_matrix"]["condition_number"] is not None
    ]
    return {
        "role": "design_comparison",
        "input_definition": input_definition,
        "lfsr": {
            key: value for key, value in lfsr.items()
            if key not in {"bits", "direct_pair_prefix_extent",
                           "direct_period_prefix_extent"}
        },
        "sequence": sequence,
        "period_audit": audit,
        "exact_schedule_audit": exact_schedule_audit,
        "all_horizons_full_rank": full_rank,
        "worst_circular_condition_number": max(conditions)
            if full_rank and conditions else None,
        "worst_exact_input_condition_number": max(exact_input_conditions)
            if full_rank and exact_input_conditions else None,
        "worst_exact_augmented_condition_number": max(
            exact_augmented_conditions)
            if full_rank and exact_augmented_conditions else None,
    }


CONDITION_SELECTION_DECIMAL_PLACES = 12


def _recurrence_selection_key(candidate: dict) -> tuple:
    input_condition = candidate["worst_exact_input_condition_number"]
    augmented_condition = candidate[
        "worst_exact_augmented_condition_number"]
    if not candidate["all_horizons_full_rank"] or \
            input_condition is None or augmented_condition is None:
        raise ValueError("未通过 exact schedule rank 的 recurrence 不得参与择优")
    return (
        round(input_condition, CONDITION_SELECTION_DECIMAL_PLACES),
        round(augmented_condition, CONDITION_SELECTION_DECIMAL_PLACES),
        candidate["lfsr"]["feedback_mask"],
        candidate["lfsr"]["phase"],
    )


def _recurrence_summary(candidate: dict) -> dict:
    return {
        "feedback_mask": candidate["lfsr"]["feedback_mask"],
        "feedback_mask_hex": candidate["lfsr"]["feedback_mask_hex"],
        "phase": candidate["lfsr"]["phase"],
        "worst_exact_input_condition_number":
            candidate["worst_exact_input_condition_number"],
        "worst_exact_augmented_condition_number":
            candidate["worst_exact_augmented_condition_number"],
        "selection_key": list(_recurrence_selection_key(candidate)),
    }


def _select_recurrences_for_family(
        recurrences: list[dict],
        input_definition: str,
        horizons: list[int],
        guard_sample_count: int,
        observed_lag: int) -> tuple[dict, dict, dict]:
    """穷举所选阶数的 recurrence/phase，并预注册不同 recurrence 留出。"""
    best_by_mask: dict[int, tuple[tuple, dict, dict]] = {}
    eligible_count = 0
    for lfsr in recurrences:
        candidate = _audit_candidate(
            lfsr=lfsr,
            input_definition=input_definition,
            horizons=horizons,
            guard_sample_count=guard_sample_count,
            observed_lag=observed_lag,
            pair_roles=["estimation", "within_run_validation"],
        )
        if not candidate["all_horizons_full_rank"]:
            continue
        eligible_count += 1
        key = _recurrence_selection_key(candidate)
        feedback_mask = candidate["lfsr"]["feedback_mask"]
        incumbent = best_by_mask.get(feedback_mask)
        if incumbent is None or key < incumbent[0]:
            best_by_mask[feedback_mask] = (key, lfsr, candidate)

    ranked = sorted(best_by_mask.values(), key=lambda entry: entry[0])
    if len(ranked) < 2:
        raise ValueError("跨 Run 留出至少需要两个不同 maximum-length recurrence")
    primary_entry = ranked[0]
    holdout_entry = ranked[1]
    primary = primary_entry[2]
    primary["role"] = "primary_estimation_and_within_run_validation"
    cross_run = _audit_candidate(
        lfsr=holdout_entry[1],
        input_definition=input_definition,
        horizons=horizons,
        guard_sample_count=guard_sample_count,
        observed_lag=observed_lag,
        pair_roles=["cross_run_holdout"],
    )
    cross_run["role"] = "cross_run_holdout"

    metadata = {
        "order": primary["lfsr"]["order"],
        "input_definition": input_definition,
        "evaluated_recurrence_phase_count": len(recurrences),
        "eligible_recurrence_phase_count": eligible_count,
        "distinct_maximum_length_feedback_mask_count": len(best_by_mask),
        "condition_round_decimal_places":
            CONDITION_SELECTION_DECIMAL_PLACES,
        "ranking_rule": [
            "lowest_rounded_worst_exact_input_condition_number",
            "lowest_rounded_worst_exact_augmented_condition_number",
            "lowest_feedback_mask",
            "lowest_phase",
        ],
        "primary": _recurrence_summary(primary),
        "cross_run_holdout": _recurrence_summary(cross_run),
        "best_phase_per_feedback_mask": [
            _recurrence_summary(entry[2]) for entry in ranked
        ],
    }
    return primary, cross_run, metadata


def _physical_a_contract(analysis: dict) -> tuple[int, float, float, list[str]]:
    if analysis.get("status") != "VALID" or \
            analysis.get("machine_visible_effect_observed") is not True or \
            analysis.get("human_physical_acceptance") != \
            "NOT_INFERRED_BY_ANALYZER":
        raise ValueError("输入必须是未外推人工结论的 VALID Physical A 分析")
    pulses = analysis.get("pulse_responses")
    if not isinstance(pulses, list) or len(pulses) < 2:
        raise ValueError("Physical A pulse response 数量不足")
    lags = {
        pulse.get("onset", {}).get("first_changed_frame_lag")
        for pulse in pulses
    }
    if len(lags) != 1 or next(iter(lags)) is None:
        raise ValueError("Physical A first-visible lag 不一致或缺失")
    observed_lag = int(next(iter(lags)))
    if observed_lag <= 0:
        raise ValueError("Physical A first-visible lag 必须为正")
    gains = [float(pulse["x_px_per_count"]) for pulse in pulses]
    if any(not math.isfinite(gain) or gain <= 0.0 for gain in gains):
        raise ValueError("Physical A run-local transition gain 非法")

    geometry = analysis.get("geometry", {})
    left = geometry.get("left_roi", {})
    right = geometry.get("right_roi", {})
    margins = [
        left.get("left_margin_px"), left.get("right_margin_px"),
        right.get("left_margin_px"), right.get("right_margin_px"),
    ]
    if any(value is None or float(value) <= 0.0 for value in margins):
        raise ValueError("Physical A witness 水平图像边界余量缺失")
    image_edge_margin = min(float(value) for value in margins)

    blockers: list[str] = []
    baseline = analysis.get("zero_input_baseline", {})
    witness = analysis.get("witness_state_summary", {})
    if witness.get("statistical_independence_claimed") is not True or \
            baseline.get("left_exact_state_count", 0) <= 1 or \
            baseline.get("right_exact_state_count", 0) <= 1:
        blockers.append("INDEPENDENT_NONDEGENERATE_NOISE_MISSING")
    if analysis.get("independent_tail_support_proven") is not True:
        blockers.append("INDEPENDENT_TAIL_SUPPORT_MISSING")
    if geometry.get("occlusion_margin_px") is None:
        blockers.append("OCCLUSION_MARGIN_MISSING")
    if analysis.get("mapping_uncertainty_px") is None:
        blockers.append("MAPPING_UNCERTAINTY_PX_MISSING")
    if analysis.get("general_gain_upper_bound_px_per_count") is None:
        blockers.append("GENERAL_GAIN_UPPER_BOUND_MISSING")
    return observed_lag, max(gains), image_edge_margin, blockers


def design_physical_b_candidates(
        physical_a_analysis: dict,
        orders: list[int],
        horizons: list[int],
        guard_sample_count: int) -> dict:
    """比较两种输入定义并生成不含 Physical 执行能力的 exact 候选。"""
    observed_lag, run_local_gain_max, image_edge_margin, blockers = \
        _physical_a_contract(physical_a_analysis)
    if len(orders) < 2 or len(set(orders)) != len(orders) or \
            orders != sorted(orders):
        raise ValueError("orders 必须递增、唯一且至少包含两个候选")
    if len(horizons) < 2 or len(set(horizons)) != len(horizons) or \
            horizons != sorted(horizons) or \
            any(horizon < observed_lag for horizon in horizons):
        raise ValueError(
            "horizons 必须递增、唯一、至少两个且全部覆盖 observed lag"
        )
    if guard_sample_count < max(horizons):
        raise ValueError("候选 guard 必须覆盖本轮最大 H 包络")

    recurrence_rankings = {
        order: _ranked_recurrences(order) for order in orders
    }
    candidates: list[dict] = []
    for order in orders:
        lfsr = recurrence_rankings[order][0]
        for input_definition in (
                "direct_command_counts",
                "cumulative_position_counts"):
            candidates.append(_audit_candidate(
                lfsr=lfsr,
                input_definition=input_definition,
                horizons=horizons,
                guard_sample_count=guard_sample_count,
                observed_lag=observed_lag,
                pair_roles=["estimation", "within_run_validation"],
            ))

    eligible = [
        candidate for candidate in candidates
        if candidate["all_horizons_full_rank"]
    ]
    if not eligible:
        raise ValueError("没有候选对全部 H 形成满列秩 circular window matrix")
    selected_family = min(eligible, key=lambda candidate: (
        candidate["sequence"]["summary"]["max_abs_position_x_counts"],
        candidate["lfsr"]["period_sample_count"],
        candidate["input_definition"],
    ))
    selected_order = selected_family["lfsr"]["order"]
    selected, cross_run, recurrence_selection = \
        _select_recurrences_for_family(
            recurrences=recurrence_rankings[selected_order],
            input_definition=selected_family["input_definition"],
            horizons=horizons,
            guard_sample_count=guard_sample_count,
            observed_lag=observed_lag,
        )
    if selected["sequence"]["summary"][
            "max_abs_position_x_counts"] != selected_family[
                "sequence"]["summary"]["max_abs_position_x_counts"]:
        raise ValueError("recurrence 择优不得改变已选 input family 的前缀边界")

    selected_prefix = selected["sequence"]["summary"][
        "max_abs_position_x_counts"]
    run_local_displacement = selected_prefix * run_local_gain_max
    return {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_offline_design",
        "status": "VALID_OFFLINE_DESIGN",
        "physical_output_capability": False,
        "physical_b_launch_authorized": False,
        "production_aim_changed": False,
        "source_physical_a": {
            "run_uuid": physical_a_analysis.get("run_binding", {}).get(
                "run_uuid"),
            "sequence_sha256": physical_a_analysis.get(
                "run_binding", {}).get("sequence_sha256"),
            "observed_first_visible_lag": observed_lag,
            "timestamp_semantic": physical_a_analysis.get(
                "method", {}).get("timestamp_semantic"),
            "run_local_transition_gain_max_px_per_count":
                run_local_gain_max,
            "statistical_independence_claimed": physical_a_analysis.get(
                "witness_state_summary", {}).get(
                    "statistical_independence_claimed"),
        },
        "candidate_horizons": horizons,
        "guard": {
            "candidate_sample_count": guard_sample_count,
            "covers_observed_lag": guard_sample_count >= observed_lag,
            "covers_candidate_horizon_envelope":
                guard_sample_count >= max(horizons),
            "tail_support_proven":
                "INDEPENDENT_TAIL_SUPPORT_MISSING" not in blockers,
            "semantics": "offline_candidate_not_physical_tail_acceptance",
        },
        "selection_rule": [
            "all_candidate_horizons_full_column_rank",
            "minimum_max_abs_physical_position_prefix",
            "shortest_complete_period",
            "enumerate_all_selected_order_recurrence_phase_variants",
            "lowest_rounded_worst_exact_input_condition_number",
            "lowest_rounded_worst_exact_augmented_condition_number",
        ],
        "candidates": candidates,
        "recurrence_selection": recurrence_selection,
        "selected_candidate": selected,
        "cross_run_holdout_candidate": cross_run,
        "holdout_contract": {
            "random_frame_split_allowed": False,
            "estimation": "first complete normal/inverted pair",
            "within_run_validation": "second complete normal/inverted pair",
            "cross_run_validation": (
                "independent Run with a different maximum-length recurrence"
            ),
        },
        "physical_prepare_gate": {
            "ready": not blockers,
            "allowed_prefix_counts": None if blockers else selected_prefix,
            "blockers": blockers,
            "witness_geometry": {
                "image_edge_margin_px": image_edge_margin,
                "occlusion_margin_px": physical_a_analysis.get(
                    "geometry", {}).get("occlusion_margin_px"),
                "mapping_uncertainty_px": physical_a_analysis.get(
                    "mapping_uncertainty_px"),
                "general_gain_upper_bound_px_per_count":
                    physical_a_analysis.get(
                        "general_gain_upper_bound_px_per_count"),
                "run_local_gain_reference_px_per_count":
                    run_local_gain_max,
                "run_local_prefix_displacement_reference_px":
                    run_local_displacement,
                "residual_image_edge_margin_reference_px":
                    image_edge_margin - run_local_displacement,
                "reference_only_not_safety_limit": True,
            },
        },
    }


def _canonical_semantic_sha256(value: dict, field: str) -> str:
    payload = dict(value)
    payload.pop(field, None)
    return hashlib.sha256(json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")).hexdigest()


def _physical_b_analysis_contract() -> tuple[dict, dict]:
    analyzer_path = pathlib.Path(__file__).resolve().with_name(
        "analyze_mouse_effect_probe_b.py")
    if not analyzer_path.is_file():
        raise ValueError("Physical B analyzer 文件不存在")
    spec = importlib.util.spec_from_file_location(
        "xen_physical_b_f0_analyzer_contract", analyzer_path)
    if spec is None or spec.loader is None:
        raise ValueError("Physical B analyzer 无法加载")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    contract = module.physical_b_analysis_contract()
    if not isinstance(contract, dict) or \
            contract.get("contract_semantic_sha256") != \
            module.canonical_semantic_sha256(
                contract, "contract_semantic_sha256"):
        raise ValueError("Physical B analyzer contract 语义 SHA 无效")
    return contract, {
        "file": analyzer_path.name,
        "file_sha256": hashlib.sha256(analyzer_path.read_bytes()).hexdigest(),
        "contract_semantic_sha256": contract["contract_semantic_sha256"],
        "python_version": sys.version.split()[0],
        "numpy_version": np.__version__,
        "opencv_version": module.cv2.__version__,
    }


def _require_sha256(value: object, description: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or any(
            character not in "0123456789abcdef" for character in value):
        raise ValueError(f"{description} 不是小写 SHA-256")
    return value


def _validate_bound_json(
        value: dict,
        content: bytes,
        expected_file_sha256: str,
        description: str) -> str:
    if not isinstance(value, dict) or not isinstance(content, bytes):
        raise ValueError(f"{description} 必须是 JSON object 与原始 bytes")
    expected = _require_sha256(
        expected_file_sha256, f"{description} expected file SHA-256")
    actual = hashlib.sha256(content).hexdigest()
    if actual != expected:
        raise ValueError(f"{description} 文件 SHA-256 不匹配")
    parsed = json.loads(content.decode("utf-8"))
    if not isinstance(parsed, dict) or parsed != value:
        raise ValueError(f"{description} object 与原始 bytes 不一致")
    return actual


def _require_false(value: object, description: str) -> None:
    if value is not False:
        raise ValueError(f"{description} 必须精确为 false")


def _require_true(value: object, description: str) -> None:
    if value is not True:
        raise ValueError(f"{description} 必须精确为 true")


def _rebuild_frozen_sequence(candidate: dict) -> dict:
    lfsr = candidate["lfsr"]
    sequence = candidate["sequence"]
    pair_roles = sequence["pair_roles"]
    bits = generate_maximum_length_period(
        order=int(lfsr["order"]),
        feedback_mask=int(lfsr["feedback_mask"]),
        seed=int(lfsr["seed"]),
        phase=int(lfsr["phase"]),
    )
    rebuilt = build_candidate_sequence(
        period_bits=bits,
        input_definition=str(candidate["input_definition"]),
        guard_sample_count=int(sequence["guard_sample_count"]),
        pair_repetitions=len(pair_roles),
        role=str(pair_roles[0]),
    )
    _assign_pair_roles(rebuilt, list(pair_roles))
    _bind_sequence_semantics(rebuilt)
    if rebuilt != sequence:
        raise ValueError("Physical B frozen sequence 无法由 recurrence 精确重建")
    return rebuilt


def bind_physical_b_primary_prepare_plan(
        offline_design: dict,
        offline_design_content: bytes,
        expected_offline_design_sha256: str,
        a2_decision: dict,
        a2_decision_content: bytes,
        expected_a2_decision_sha256: str) -> dict:
    """把冻结离线设计与正式 A2 green 绑定成无 Launch 能力的 Primary F0。"""
    design_file_sha256 = _validate_bound_json(
        offline_design,
        offline_design_content,
        expected_offline_design_sha256,
        "Physical B offline design",
    )
    decision_file_sha256 = _validate_bound_json(
        a2_decision,
        a2_decision_content,
        expected_a2_decision_sha256,
        "A2 dependency decision",
    )

    if offline_design.get("schema_version") != 1 or \
            offline_design.get("evidence_type") != \
            "mouse_effect_probe_physical_b_offline_design" or \
            offline_design.get("status") != "VALID_OFFLINE_DESIGN":
        raise ValueError("Physical B offline design 身份或状态非法")
    _require_false(offline_design.get("physical_output_capability"),
                   "offline design physical_output_capability")
    _require_false(offline_design.get("physical_b_launch_authorized"),
                   "offline design physical_b_launch_authorized")
    _require_false(offline_design.get("production_aim_changed"),
                   "offline design production_aim_changed")
    claimed_design_sha = _require_sha256(
        offline_design.get("design_semantic_sha256"),
        "offline design semantic SHA-256",
    )
    if claimed_design_sha != _canonical_semantic_sha256(
            offline_design, "design_semantic_sha256"):
        raise ValueError("Physical B offline design 规范语义 SHA-256 不匹配")
    if offline_design.get("candidate_horizons") != [4, 8, 16, 32]:
        raise ValueError("Physical B candidate H 集合已漂移")
    guard = offline_design["guard"]
    if int(guard.get("candidate_sample_count", 0)) != 32 or \
            guard.get("covers_candidate_horizon_envelope") is not True:
        raise ValueError("Physical B guard 合同已漂移")
    expected_legacy_blockers = {
        "INDEPENDENT_NONDEGENERATE_NOISE_MISSING",
        "INDEPENDENT_TAIL_SUPPORT_MISSING",
        "OCCLUSION_MARGIN_MISSING",
        "MAPPING_UNCERTAINTY_PX_MISSING",
        "GENERAL_GAIN_UPPER_BOUND_MISSING",
    }
    prepare_gate = offline_design["physical_prepare_gate"]
    if prepare_gate.get("ready") is not False or \
            set(prepare_gate.get("blockers", [])) != expected_legacy_blockers:
        raise ValueError("offline design 的 pre-A2 blocker 集合已漂移")

    selected = offline_design["selected_candidate"]
    cross_run = offline_design["cross_run_holdout_candidate"]
    if selected.get("role") != \
            "primary_estimation_and_within_run_validation" or \
            selected.get("input_definition") != \
            "cumulative_position_counts" or \
            selected.get("all_horizons_full_rank") is not True or \
            selected["sequence"].get("pair_roles") != \
            ["estimation", "within_run_validation"]:
        raise ValueError("Physical B Primary candidate 角色或输入定义非法")
    if cross_run.get("role") != "cross_run_holdout" or \
            cross_run.get("input_definition") != \
            "cumulative_position_counts" or \
            cross_run.get("all_horizons_full_rank") is not True or \
            cross_run["sequence"].get("pair_roles") != \
            ["cross_run_holdout"]:
        raise ValueError("Physical B cross-Run candidate 角色非法")
    primary_sequence = _rebuild_frozen_sequence(selected)
    holdout_sequence = _rebuild_frozen_sequence(cross_run)
    if int(selected["lfsr"]["feedback_mask"]) == \
            int(cross_run["lfsr"]["feedback_mask"]):
        raise ValueError("cross-Run holdout 必须使用不同 recurrence")
    primary_summary = primary_sequence["summary"]
    if primary_summary.get("net_command_dx_counts") != 0 or \
            primary_summary.get("max_abs_position_x_counts") != 1 or \
            primary_summary.get("all_commands_x_only_single_count") is not True:
        raise ValueError("Physical B Primary X-only/net/prefix 合同非法")

    if a2_decision.get("schema_version") != 1 or \
            a2_decision.get("evidence_type") != \
            "mouse_effect_probe_a2_dependency_holdout_decision" or \
            a2_decision.get("status") != "A2_DEPENDENCY_GREEN" or \
            a2_decision.get("invalid_reasons") != [] or \
            a2_decision.get("run_role") != "p-holdout" or \
            a2_decision.get("profile") != \
            "dependency_calibration_a2_p_holdout":
        raise ValueError("A2 dependency decision 身份或状态非法")
    _require_false(a2_decision.get("physical_output_capability"),
                   "A2 decision physical_output_capability")
    _require_false(a2_decision.get("production_aim_changed"),
                   "A2 decision production_aim_changed")
    _require_false(a2_decision.get("candidate_values_changed"),
                   "A2 decision candidate_values_changed")
    _require_false(a2_decision.get("holdout_used_for_tuning"),
                   "A2 decision holdout_used_for_tuning")
    _require_true(a2_decision.get("a2_dependency_gate_claimed"),
                  "A2 decision a2_dependency_gate_claimed")
    _require_false(a2_decision.get("physical_b_authorized"),
                   "historical A2 decision physical_b_authorized")
    _require_sha256(a2_decision.get("scope_id"), "A2 scope_id")
    _require_sha256(a2_decision.get("sequence_sha256"),
                    "A2 sequence SHA-256")
    _require_sha256(a2_decision.get("candidate_sha256"),
                    "A2 candidate SHA-256")

    independence = a2_decision["independence"]
    for field in (
            "different_run_uuid", "different_activation_epoch",
            "different_sidecar_manifest", "same_analyzer",
            "same_capture_source"):
        _require_true(independence.get(field), f"A2 independence.{field}")
    observation = a2_decision["human_observation"]
    _require_sha256(observation.get("observation_sha256"),
                    "A2 observation SHA-256")
    _require_true(observation.get("visible_effect_reported"),
                  "A2 visible effect")
    _require_false(observation.get("manual_mouse_or_wasd_used"),
                   "A2 manual mouse/WASD")
    _require_true(observation.get("left_right_witness_consistent"),
                  "A2 left/right consistency")
    _require_false(observation.get("occlusion_or_scene_cut_reported"),
                   "A2 occlusion/scene cut")
    _require_false(observation.get("anomaly_or_emergency_stop_reported"),
                   "A2 anomaly/emergency stop")

    comparisons = a2_decision["comparisons"]
    for name in (
            "tail_support", "mapping_uncertainty",
            "single_count_gain_upper_scope", "witness_occlusion_margin",
            "physical_b_prefix_candidate"):
        _require_true(comparisons[name].get("passed"),
                      f"A2 comparisons.{name}.passed")
    prefix_comparison = comparisons["physical_b_prefix_candidate"]
    _require_false(prefix_comparison.get("physical_b_authorized"),
                   "A2 prefix comparison physical_b_authorized")
    tail_upper = int(comparisons["tail_support"]["candidate_upper_lag"])
    observed_tail = int(
        comparisons["tail_support"]["holdout_observed_upper_lag"])
    guard_sample_count = int(guard["candidate_sample_count"])
    allowed_prefix = int(
        prefix_comparison["candidate_allowed_prefix_counts"])
    holdout_allowed_prefix = int(
        prefix_comparison["holdout_allowed_prefix_counts"])
    actual_prefix = int(primary_summary["max_abs_position_x_counts"])
    if tail_upper != 7 or observed_tail > tail_upper or \
            guard_sample_count < tail_upper or \
            actual_prefix > allowed_prefix or \
            allowed_prefix > holdout_allowed_prefix:
        raise ValueError("A2 tail/guard/prefix 未闭合 Physical B Primary F0")
    eligible_horizons = [
        horizon for horizon in offline_design["candidate_horizons"]
        if horizon >= tail_upper
    ]
    deletion_horizons = [
        horizon for horizon in offline_design["candidate_horizons"]
        if horizon < tail_upper
    ]
    if eligible_horizons != [8, 16, 32] or deletion_horizons != [4]:
        raise ValueError("A2 tail 对 Physical B H 集合的裁决异常")

    analysis_contract, analyzer = _physical_b_analysis_contract()
    mapping_uncertainty_upper_px = float(
        comparisons["mapping_uncertainty"]["candidate_upper_px"])
    if not math.isfinite(mapping_uncertainty_upper_px) or \
            mapping_uncertainty_upper_px < 0.0:
        raise ValueError("A2 mapping uncertainty upper 非有限或为负")
    plan = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_b_primary_f0",
        "status": "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE",
        "physical_output_capability": False,
        "physical_b_launch_authorized": False,
        "production_aim_changed": False,
        "source_offline_design": {
            "file_size": len(offline_design_content),
            "file_sha256": design_file_sha256,
            "design_semantic_sha256": claimed_design_sha,
        },
        "source_a2_dependency_decision": {
            "file_size": len(a2_decision_content),
            "file_sha256": decision_file_sha256,
            "scope_id": a2_decision["scope_id"],
            "candidate_sha256": a2_decision["candidate_sha256"],
            "observation_sha256": observation["observation_sha256"],
        },
        "model_contract": {
            "identification_input_definition":
                "cumulative_position_counts",
            "actuator_audit_input": "completed_command_dx_counts",
            "model_boundary":
                "commanded_cumulative_position_to_visible_displacement",
            "strictly_causal_lag_origin": 1,
            "whole_block_only": True,
            "random_frame_split_allowed": False,
            "input_forced_validation_required": True,
            "output_free_run_validation_required": True,
            "one_step_prediction_acceptance_allowed": False,
        },
        "analysis_contract": analysis_contract,
        "analyzer": analyzer,
        "candidate_horizons": list(offline_design["candidate_horizons"]),
        "deletion_control_horizons": deletion_horizons,
        "acceptance_eligible_horizons": eligible_horizons,
        "primary_sequence": {
            "schema": 4,
            "profile": "physical_b_prbs_primary",
            "offline_sequence_semantic_sha256":
                primary_sequence["sequence_semantic_sha256"],
            "input_definition": selected["input_definition"],
            "lfsr": dict(selected["lfsr"]),
            "guard_sample_count": guard_sample_count,
            "pair_roles": list(primary_sequence["pair_roles"]),
            "sample_count": int(primary_summary["sample_count"]),
            "net_x_counts": int(
                primary_summary["net_command_dx_counts"]),
            "max_abs_prefix_x_counts": actual_prefix,
            "all_commands_x_only_single_count":
                primary_summary["all_commands_x_only_single_count"],
        },
        "cross_run_holdout": {
            "preregistered": True,
            "prepare_allowed": False,
            "run_required": "different_run_activation_and_session",
            "lfsr": dict(cross_run["lfsr"]),
            "sequence_semantic_sha256":
                holdout_sequence["sequence_semantic_sha256"],
            "holdout_used_for_tuning": False,
        },
        "physical_b_primary_prepare_gate": {
            "ready": True,
            "blockers": [],
            "a2_dependency_status": "A2_DEPENDENCY_GREEN",
            "a2_tail_upper_lag": tail_upper,
            "a2_holdout_observed_upper_lag": observed_tail,
            "mapping_uncertainty_upper_px": mapping_uncertainty_upper_px,
            "guard_sample_count": guard_sample_count,
            "actual_prefix_counts": actual_prefix,
            "allowed_prefix_counts": allowed_prefix,
            "holdout_allowed_prefix_counts": holdout_allowed_prefix,
            "candidate_values_changed": False,
            "holdout_used_for_tuning": False,
        },
        "authorization_boundary": {
            "prepare_requires_current_user_authorization": True,
            "launch_requires_separate_current_user_action": True,
            "recover_authorized": False,
            "cross_run_holdout_prepare_authorized": False,
        },
    }
    plan["f0_semantic_sha256"] = _canonical_semantic_sha256(
        plan, "f0_semantic_sha256")
    return plan


def _parse_positive_integer_list(value: str) -> list[int]:
    try:
        result = [int(item, 10) for item in value.split(",")]
    except ValueError as exception:
        raise argparse.ArgumentTypeError("必须是逗号分隔的正整数") from exception
    if not result or any(item <= 0 for item in result):
        raise argparse.ArgumentTypeError("必须是逗号分隔的正整数")
    return result


def _parse_arguments(arguments: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "只生成/审计 Mouse Effect Probe Physical B 离线 PRBS 候选；"
            "不 Prepare、不打开 Mouse、不含 Physical Launch 能力。"
        )
    )
    parser.add_argument(
        "--physical-a-analysis", type=pathlib.Path, required=True,
        help="VALID Physical A response JSON 的绝对路径",
    )
    parser.add_argument(
        "--output", type=pathlib.Path, required=True,
        help="拒绝覆盖的新离线设计 JSON 绝对路径",
    )
    parser.add_argument(
        "--orders", type=_parse_positive_integer_list, required=True,
        help="递增、唯一的 LFSR order 候选，例如 5,6,7",
    )
    parser.add_argument(
        "--horizons", type=_parse_positive_integer_list, required=True,
        help="递增、唯一的 causal H 候选，例如 4,8,16,32",
    )
    parser.add_argument(
        "--guard-samples", type=int, required=True,
        help="仅供离线审计的 guard 候选；不是 Physical tail 通过值",
    )
    return parser.parse_args(arguments)


def _parse_bind_primary_arguments(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="design_mouse_effect_probe_prbs.py bind-primary",
        description=(
            "把冻结的 Physical B 离线设计与正式 A2 green 绑定为 Primary "
            "F0；只生成 Prepare 输入，不含 Launch 能力。"
        ),
    )
    parser.add_argument(
        "--offline-design", type=pathlib.Path, required=True,
        help="冻结 Physical B offline design JSON 的绝对路径",
    )
    parser.add_argument(
        "--expected-offline-design-sha256", required=True,
        help="offline design 文件的小写 SHA-256",
    )
    parser.add_argument(
        "--a2-decision", type=pathlib.Path, required=True,
        help="正式 A2 dependency decision JSON 的绝对路径",
    )
    parser.add_argument(
        "--expected-a2-decision-sha256", required=True,
        help="A2 decision 文件的小写 SHA-256",
    )
    parser.add_argument(
        "--output", type=pathlib.Path, required=True,
        help="拒绝覆盖的新 Primary F0 JSON 绝对路径",
    )
    return parser.parse_args(arguments)


def _load_json_bytes(path: pathlib.Path) -> tuple[dict, bytes]:
    if not path.is_absolute() or not path.is_file():
        raise ValueError("Physical A analysis 必须是绝对路径普通文件")
    size = path.stat().st_size
    if size <= 0 or size > 16 * 1024 * 1024:
        raise ValueError("Physical A analysis 为空或超过 16 MiB 固定边界")
    content = path.read_bytes()
    value = json.loads(content.decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("Physical A analysis 根对象必须是 JSON object")
    return value, content


def _load_bound_json_bytes(
        path: pathlib.Path,
        description: str) -> tuple[dict, bytes]:
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"{description} 必须是绝对路径普通文件")
    size = path.stat().st_size
    if size <= 0 or size > 128 * 1024 * 1024:
        raise ValueError(f"{description} 为空或超过 128 MiB 固定边界")
    content = path.read_bytes()
    value = json.loads(content.decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{description} 根对象必须是 JSON object")
    return value, content


def _write_new_json(path: pathlib.Path, value: dict) -> None:
    if not path.is_absolute():
        raise ValueError("output 必须是绝对路径")
    if path.exists():
        raise FileExistsError("离线设计输出已存在，拒绝覆盖")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".pending-{os.getpid()}")
    if temporary.exists():
        raise FileExistsError("离线设计临时输出已存在，拒绝覆盖")
    content = (
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    ).encode("utf-8")
    try:
        with temporary.open("xb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        if path.exists():
            raise FileExistsError("离线设计输出在发布前已出现，拒绝覆盖")
        os.rename(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _bind_primary_main(arguments: list[str]) -> int:
    options = _parse_bind_primary_arguments(arguments)
    design, design_content = _load_bound_json_bytes(
        options.offline_design, "Physical B offline design")
    decision, decision_content = _load_bound_json_bytes(
        options.a2_decision, "A2 dependency decision")
    plan = bind_physical_b_primary_prepare_plan(
        design,
        design_content,
        options.expected_offline_design_sha256,
        decision,
        decision_content,
        options.expected_a2_decision_sha256,
    )
    plan["source_offline_design"]["path"] = str(options.offline_design)
    plan["source_a2_dependency_decision"]["path"] = str(
        options.a2_decision)
    binder_path = pathlib.Path(__file__).resolve()
    plan["binder"] = {
        "file": str(binder_path),
        "file_sha256": hashlib.sha256(binder_path.read_bytes()).hexdigest(),
        "python_version": sys.version.split()[0],
        "numpy_version": np.__version__,
    }
    plan["f0_semantic_sha256"] = _canonical_semantic_sha256(
        plan, "f0_semantic_sha256")
    _write_new_json(options.output, plan)
    print(
        "Physical B Primary F0 已生成: "
        f"output={options.output}, "
        f"sequence={plan['primary_sequence']['offline_sequence_semantic_sha256']}, "
        f"eligible_h={plan['acceptance_eligible_horizons']}, "
        "launch_authorized=false"
    )
    return 0


def main(arguments: list[str] | None = None) -> int:
    try:
        effective_arguments = list(
            sys.argv[1:] if arguments is None else arguments)
        if effective_arguments and effective_arguments[0] == "bind-primary":
            return _bind_primary_main(effective_arguments[1:])
        options = _parse_arguments(effective_arguments)
        analysis, analysis_bytes = _load_json_bytes(
            options.physical_a_analysis)
        design = design_physical_b_candidates(
            analysis,
            orders=options.orders,
            horizons=options.horizons,
            guard_sample_count=options.guard_samples,
        )
        design["source_physical_a"].update({
            "analysis_file": str(options.physical_a_analysis),
            "analysis_file_size": len(analysis_bytes),
            "analysis_file_sha256": hashlib.sha256(
                analysis_bytes).hexdigest(),
        })
        designer_path = pathlib.Path(__file__).resolve()
        design["designer"] = {
            "file": str(designer_path),
            "file_sha256": hashlib.sha256(designer_path.read_bytes()).hexdigest(),
            "python_version": sys.version.split()[0],
            "numpy_version": np.__version__,
        }
        semantic_payload = json.dumps(
            design,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
        design["design_semantic_sha256"] = hashlib.sha256(
            semantic_payload).hexdigest()
        _write_new_json(options.output, design)
        print(
            "Physical B 离线候选已生成: "
            f"output={options.output}, "
            f"definition={design['selected_candidate']['input_definition']}, "
            f"order={design['selected_candidate']['lfsr']['order']}, "
            f"prepare_ready={design['physical_prepare_gate']['ready']}"
        )
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) \
            as exception:
        print(f"Physical B 离线候选生成失败: {exception}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
