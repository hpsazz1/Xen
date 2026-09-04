#!/usr/bin/env python3
"""从冻结 B0 派生 dbed788b 初态、时钟或 A2 幅值域保真反事实。"""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import math
import pathlib
import sys
from typing import Any


WINDOWS = ((40, 340), (700, 1000), (1400, 1700), (2099, 2399))
SAMPLES_PER_BLOCK = 300
BASELINE_ASSET_ID = "dbed788b-legacy-output-off-v1"
PLAN_EVIDENCE_TYPE = "aim_production_red_output_off_plan"
HYPOTHESES = {
    "initial-position": {
        "asset_id": "dbed788b-fidelity-initial-position-v1",
        "changed_fields": ["blocks[].initial_world_aim_point[0]"],
    },
    "control-clock": {
        "asset_id": "dbed788b-fidelity-control-clock-v1",
        "changed_fields": [
            "blocks[].samples[].control_at_ns",
            "blocks[].samples[].controller_dt_ns",
        ],
    },
    "initial-position-control-clock": {
        "asset_id": "dbed788b-fidelity-initial-position-control-clock-v1",
        "changed_fields": [
            "blocks[].initial_world_aim_point[0]",
            "blocks[].samples[].control_at_ns",
            "blocks[].samples[].controller_dt_ns",
        ],
    },
    "a2-magnitude-domain-plant": {
        "asset_id": "dbed788b-fidelity-a2-magnitude-domain-plant-v1",
        "changed_fields": ["plant_profiles[+3]"],
    },
}


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_runtime(path: pathlib.Path,
                 required: set[str]) -> tuple[list[str], dict[int, list[str]]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = [row for row in csv.reader(stream) if row]
    try:
        header = next(row for row in rows if row[0] == "sequence")
    except StopIteration as error:
        raise ValueError(f"Runtime 缺少 sequence header: {path}") from error
    missing = required - set(header)
    if missing:
        raise ValueError(
            f"Runtime 缺字段 {','.join(sorted(missing))}: {path}")
    by_sequence: dict[int, list[str]] = {}
    for row in rows:
        if len(row) != len(header) or not row[0].isdigit():
            continue
        sequence = int(row[0])
        if sequence in by_sequence:
            raise ValueError(f"Runtime sequence 重复: {sequence}")
        by_sequence[sequence] = row
    return header, by_sequence


def finite_cell(row: list[str], columns: dict[str, int],
                field: str, context: str) -> float:
    try:
        value = float(row[columns[field]])
    except (IndexError, ValueError) as error:
        raise ValueError(f"{context} 的 {field} 不是数值") from error
    if not math.isfinite(value):
        raise ValueError(f"{context} 的 {field} 不是有限值")
    return value


def read_baseline(path: pathlib.Path) -> dict[str, Any]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"baseline plan JSON 无效: {error}") from error
    if not isinstance(plan, dict) or plan.get("plan_schema") != 1 or \
            plan.get("evidence_type") != PLAN_EVIDENCE_TYPE or \
            plan.get("asset_id") != BASELINE_ASSET_ID:
        raise ValueError("只接受冻结 dbed788b legacy output-off v1 baseline")
    blocks = plan.get("blocks")
    if not isinstance(blocks, list) or len(blocks) != len(WINDOWS):
        raise ValueError("baseline 必须包含四个冻结 blocks")
    for index, block in enumerate(blocks):
        if not isinstance(block, dict) or \
                block.get("block_id") != f"DEV-PIX-S{index}" or \
                block.get("score_begin") != 80 or \
                block.get("score_end") != SAMPLES_PER_BLOCK or \
                not isinstance(block.get("samples"), list) or \
                len(block["samples"]) != SAMPLES_PER_BLOCK:
            raise ValueError(f"baseline block 合同无效: index={index}")
        initial = block.get("initial_world_aim_point")
        if not isinstance(initial, list) or len(initial) != 2 or \
                not all(isinstance(value, (int, float)) and
                        math.isfinite(float(value)) for value in initial):
            raise ValueError(f"baseline initial world 无效: index={index}")
        previous_captured = 0
        previous_control = 0
        for local_index, sample in enumerate(block["samples"]):
            if not isinstance(sample, dict):
                raise ValueError("baseline sample 必须是对象")
            captured = sample.get("captured_at_ns")
            control = sample.get("control_at_ns")
            controller_dt = sample.get("controller_dt_ns")
            if not all(isinstance(value, int) for value in
                       (captured, control, controller_dt)) or \
                    captured <= previous_captured or control < captured or \
                    control <= previous_control or controller_dt <= 0:
                raise ValueError(
                    f"baseline sample 时钟无效: block={index}, "
                    f"sample={local_index}")
            previous_captured = captured
            previous_control = control
    return plan


def is_sha256(value: object) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def read_magnitude_analysis(
        path: pathlib.Path) -> tuple[float, float, str]:
    try:
        analysis = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"A2 幅值分析 JSON 无效: {error}") from error
    if not isinstance(analysis, dict) or \
            analysis.get("schema_version") != 1 or \
            analysis.get("evidence_type") != \
                "mouse_effect_probe_a2_magnitude_domain_analysis" or \
            analysis.get("status") != "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" or \
            analysis.get("physical_output_capability") is not False or \
            analysis.get("physical_dispatch_count") != 0 or \
            isinstance(analysis.get("physical_dispatch_count"), bool) or \
            analysis.get("production_aim_changed") is not False:
        raise ValueError("A2 幅值分析顶层合同无效")

    contract = analysis.get("analysis_contract")
    if not isinstance(contract, dict) or \
            contract.get("schema_version") != 1 or \
            contract.get("evidence_type") != \
                "mouse_effect_probe_a2_magnitude_domain_contract" or \
            contract.get("physical_output_capability") is not False or \
            contract.get("production_aim_changed") is not False:
        raise ValueError("A2 幅值分析 contract 无效")
    contract_model = contract.get("model")
    split = contract.get("split")
    deletion_tests = contract.get("deletion_tests")
    if not isinstance(contract_model, dict) or \
            contract_model.get("family") != \
                "delayed_static_gain_diagnostic" or \
            contract_model.get("input") != \
                "backend_completed_cumulative_position_counts" or \
            contract_model.get("delay_samples") != 4 or \
            contract_model.get("output_feedback_used") is not False or \
            not isinstance(split, dict) or \
            split.get("fit") != "primary_whole_blocks_only" or \
            split.get("evaluation") != "validation_whole_blocks_only" or \
            split.get("random_frame_split_allowed") is not False or \
            not isinstance(deletion_tests, dict) or \
            deletion_tests.get("new_production_gain_may_be_claimed") is not False:
        raise ValueError("A2 幅值分析模型/切分合同无效")

    contract_semantic_sha256 = contract.get("contract_semantic_sha256")
    if not is_sha256(contract_semantic_sha256):
        raise ValueError("A2 幅值分析 contract semantic SHA-256 无效")

    evaluation = analysis.get("evaluation")
    if not isinstance(evaluation, dict) or \
            evaluation.get("status") != \
                "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN" or \
            evaluation.get("physical_output_capability") is not False or \
            evaluation.get("production_aim_changed") is not False or \
            evaluation.get("f1_deleted_for_magnitude_domain") is not True or \
            evaluation.get("new_production_gain_claimed") is not False or \
            evaluation.get("invalid_reasons") != []:
        raise ValueError("A2 幅值分析 deletion 结论无效")
    primary_model = evaluation.get("primary_model")
    if not isinstance(primary_model, dict) or \
            primary_model.get("delay_samples") != 4 or \
            primary_model.get("fit_role") != "primary_whole_blocks_only" or \
            primary_model.get("validation_used_for_refit") is not False:
        raise ValueError("A2 幅值分析 Primary model 合同无效")
    gains: list[float] = []
    for side in ("left", "right"):
        value = primary_model.get(f"{side}_gain")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or \
                not math.isfinite(float(value)) or float(value) >= 0.0:
            raise ValueError(f"A2 幅值分析 {side} gain 无效")
        gains.append(float(value))
    return gains[0], gains[1], contract_semantic_sha256


def append_magnitude_profiles(plan: dict[str, Any], left_gain: float,
                              right_gain: float) -> None:
    profiles = plan.get("plant_profiles")
    production_profile_id = plan.get("production_plant_profile_id")
    if not isinstance(profiles, list) or not profiles or \
            not isinstance(production_profile_id, str):
        raise ValueError("baseline plant profiles 合同无效")
    production_profiles = [
        profile for profile in profiles
        if isinstance(profile, dict) and
        profile.get("plant_profile_id") == production_profile_id
    ]
    if len(production_profiles) != 1:
        raise ValueError("baseline production plant profile 必须唯一")
    production_y = production_profiles[0].get(
        "pixels_per_completed_count_y")
    if isinstance(production_y, bool) or \
            not isinstance(production_y, (int, float)) or \
            not math.isfinite(float(production_y)):
        raise ValueError("baseline production plant Y gain 无效")
    existing_ids = {
        profile.get("plant_profile_id") for profile in profiles
        if isinstance(profile, dict)
    }
    profile_ids = (
        "P-A2-MAG-D4-LEFT", "P-A2-MAG-D4-MID",
        "P-A2-MAG-D4-RIGHT")
    if any(profile_id in existing_ids for profile_id in profile_ids):
        raise ValueError("baseline 已包含 A2 幅值 plant profile")
    mid_gain = math.fsum((left_gain, right_gain)) / 2.0
    for profile_id, gain in zip(
            profile_ids, (left_gain, mid_gain, right_gain)):
        profiles.append({
            "plant_profile_id": profile_id,
            "delay_samples": 4,
            "pixels_per_completed_count_x": gain,
            "pixels_per_completed_count_y": float(production_y),
        })


def set_initial_positions(plan: dict[str, Any],
                          header: list[str],
                          replay: dict[int, list[str]]) -> None:
    columns = {name: index for index, name in enumerate(header)}
    for block_index, (window_begin, _) in enumerate(WINDOWS):
        sequence = window_begin + 1
        row = replay.get(sequence)
        if row is None:
            raise ValueError(
                f"output-off replay 缺少初始 frame sequence={sequence}")
        x1 = finite_cell(row, columns, "aim_matched_observation_x1",
                         f"replay sequence={sequence}")
        x2 = finite_cell(row, columns, "aim_matched_observation_x2",
                         f"replay sequence={sequence}")
        if x1 > x2:
            raise ValueError(f"output-off replay 初始 matched box 无效: {sequence}")
        plan["blocks"][block_index]["initial_world_aim_point"][0] = \
            (x1 + x2) * 0.5


def set_control_clock(plan: dict[str, Any],
                      header: list[str],
                      physical: dict[int, list[str]]) -> list[dict[str, int]]:
    columns = {name: index for index, name in enumerate(header)}
    summaries: list[dict[str, int]] = []
    for block_index, (window_begin, _) in enumerate(WINDOWS):
        previous_control = 0
        maximum_logged_difference = 0
        for local_index, sample in enumerate(
                plan["blocks"][block_index]["samples"]):
            sequence = window_begin + local_index + 470
            row = physical.get(sequence)
            if row is None:
                raise ValueError(
                    f"Physical Runtime 缺少 exact join sequence={sequence}")
            age_ms = finite_cell(
                row, columns, "source_to_control_ms",
                f"physical sequence={sequence}")
            logged_dt_ms = finite_cell(
                row, columns, "aim_controller_dt_ms",
                f"physical sequence={sequence}")
            if age_ms < 0.0 or logged_dt_ms <= 0.0:
                raise ValueError(
                    f"Physical Runtime 控制时钟无效: sequence={sequence}")
            captured_at_ns = sample["captured_at_ns"]
            control_at_ns = captured_at_ns + round(age_ms * 1_000_000)
            if local_index == 0:
                controller_dt_ns = round(logged_dt_ms * 1_000_000)
            else:
                controller_dt_ns = control_at_ns - previous_control
                if controller_dt_ns <= 0:
                    raise ValueError(
                        "Physical control clock 非严格递增: "
                        f"block={block_index}, sample={local_index}")
                logged_dt_ns = round(logged_dt_ms * 1_000_000)
                maximum_logged_difference = max(
                    maximum_logged_difference,
                    abs(controller_dt_ns - logged_dt_ns))
            sample["control_at_ns"] = control_at_ns
            sample["controller_dt_ns"] = controller_dt_ns
            previous_control = control_at_ns
        summaries.append({
            "block_index": block_index,
            "max_derived_vs_logged_controller_dt_difference_ns":
                maximum_logged_difference,
        })
    return summaries


def build_plan(baseline_path: pathlib.Path,
               replay_path: pathlib.Path,
               physical_path: pathlib.Path,
               hypothesis: str,
               magnitude_analysis_path: pathlib.Path | None) -> dict[str, Any]:
    plan = copy.deepcopy(read_baseline(baseline_path))
    replay_header, replay = load_runtime(replay_path, {
        "sequence", "aim_matched_observation_x1",
        "aim_matched_observation_x2",
    })
    physical_header, physical = load_runtime(physical_path, {
        "sequence", "source_to_control_ms", "aim_controller_dt_ms",
    })
    use_initial = hypothesis in (
        "initial-position", "initial-position-control-clock")
    use_control_clock = hypothesis in (
        "control-clock", "initial-position-control-clock")
    if use_initial:
        set_initial_positions(plan, replay_header, replay)
    clock_summaries: list[dict[str, int]] = []
    if use_control_clock:
        clock_summaries = set_control_clock(
            plan, physical_header, physical)
    magnitude_analysis_sha256: str | None = None
    magnitude_contract_semantic_sha256: str | None = None
    if hypothesis == "a2-magnitude-domain-plant":
        if magnitude_analysis_path is None:
            raise ValueError("A2 幅值 plant 反事实缺少 --magnitude-analysis")
        left_gain, right_gain, magnitude_contract_semantic_sha256 = \
            read_magnitude_analysis(magnitude_analysis_path)
        append_magnitude_profiles(plan, left_gain, right_gain)
        magnitude_analysis_sha256 = file_sha256(magnitude_analysis_path)
    elif magnitude_analysis_path is not None:
        raise ValueError("初态/时钟反事实不接受 --magnitude-analysis")

    plan["asset_id"] = HYPOTHESES[hypothesis]["asset_id"]
    provenance = plan.setdefault("provenance", {})
    if not isinstance(provenance, dict):
        raise ValueError("baseline provenance 必须是对象")
    provenance["fidelity_counterfactual"] = {
        "hypothesis": hypothesis,
        "changed_fields": HYPOTHESES[hypothesis]["changed_fields"],
        "baseline_plan_sha256": file_sha256(baseline_path),
        "replay_runtime_sha256": file_sha256(replay_path),
        "physical_runtime_sha256": file_sha256(physical_path),
        "replay_join": "sidecar_frame_i_to_replay_sequence_i_plus_1",
        "physical_join": "sidecar_frame_i_to_physical_sequence_i_plus_470",
        "control_clock_source":
            "captured_at_plus_physical_source_to_control_ms",
        "control_clock_crosscheck": clock_summaries,
        "physical_output_capability": False,
    }
    if magnitude_analysis_sha256 is not None:
        provenance["fidelity_counterfactual"].update({
            "magnitude_analysis_sha256": magnitude_analysis_sha256,
            "magnitude_contract_semantic_sha256":
                magnitude_contract_semantic_sha256,
            "magnitude_model_source": "primary_whole_blocks_only",
            "validation_used_for_refit": False,
            "new_production_gain_claimed": False,
            "production_plant_profile_changed": False,
        })
    return plan


def write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    incoming = path.with_name(f".{path.name}.incoming")
    if path.exists() or incoming.exists():
        raise FileExistsError("拒绝覆盖既有 fidelity plan 或 incoming")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        incoming.write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
        incoming.replace(path)
    finally:
        if incoming.exists():
            incoming.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="构建 dbed788b B0 初态/控制时钟单变量保真反事实")
    parser.add_argument("--baseline-plan", required=True, type=pathlib.Path)
    parser.add_argument(
        "--replay-runtime-csv", required=True, type=pathlib.Path)
    parser.add_argument(
        "--physical-runtime-csv", required=True, type=pathlib.Path)
    parser.add_argument(
        "--hypothesis", required=True, choices=tuple(HYPOTHESES))
    parser.add_argument("--magnitude-analysis", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args(argv)
    try:
        plan = build_plan(
            arguments.baseline_plan.resolve(),
            arguments.replay_runtime_csv.resolve(),
            arguments.physical_runtime_csv.resolve(),
            arguments.hypothesis,
            (arguments.magnitude_analysis.resolve()
             if arguments.magnitude_analysis is not None else None))
        write_json_atomic(arguments.output, plan)
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        print(f"dbed788b fidelity plan builder failed: {error}", file=sys.stderr)
        return 2
    print(
        "dbed788b fidelity plan builder: "
        f"hypothesis={arguments.hypothesis}, "
        "physical_output_capability=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
