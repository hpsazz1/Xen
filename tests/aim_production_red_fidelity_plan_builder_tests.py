#!/usr/bin/env python3
"""验证 dbed788b B0 保真反事实 plan 只改变预注册字段。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys


WINDOWS = ((40, 340), (700, 1000), (1400, 1700), (2099, 2399))
SAMPLES_PER_BLOCK = 300


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_runtime(path: pathlib.Path, physical: bool) -> None:
    header = [
        "sequence", "aim_matched_observation_x1",
        "aim_matched_observation_x2", "source_to_control_ms",
        "aim_controller_dt_ms",
    ]
    first = 470 if physical else 1
    last = 2869 if physical else 2400
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        for sequence in range(first, last + 1):
            center = 150.0 + sequence / 1000.0
            source_to_control_ms = 2.75 + (sequence % 7) * 0.05
            writer.writerow([
                sequence,
                f"{center - 10.0:.6f}",
                f"{center + 10.0:.6f}",
                f"{source_to_control_ms:.6f}",
                "4.000000",
            ])


def sample(index: int, captured_at_ns: int) -> dict[str, object]:
    return {
        "source_sequence": index + 1,
        "source_timestamp": 10_000_000 + index,
        "captured_at_ns": captured_at_ns,
        "control_at_ns": captured_at_ns + 3_000_000,
        "controller_dt_ns": 4_000_000,
        "world_delta_x": index / 1000.0,
        "world_delta_y": 0.0,
        "box_width": 20.0,
        "box_height": 48.0,
        "pose": "body",
        "visible": True,
        "target_id": 1,
        "lock_active": True,
        "backend_failure": False,
    }


def write_baseline(path: pathlib.Path) -> dict[str, object]:
    blocks = []
    for block_index, (window_begin, _) in enumerate(WINDOWS):
        captured_at_ns = 1_000_000_000_000 + window_begin * 4_000_000
        samples = [
            sample(window_begin + local_index,
                   captured_at_ns + local_index * 4_000_000)
            for local_index in range(SAMPLES_PER_BLOCK)
        ]
        blocks.append({
            "block_id": f"DEV-PIX-S{block_index}",
            "role": "development",
            "block_kind": "dynamic",
            "score_begin": 80,
            "score_end": 300,
            "initial_world_aim_point": [160.0, 160.0],
            "control_center": [160.0, 160.0],
            "roi_size": [320, 320],
            "measured_reference": {
                "source_identity_sha256": "1" * 64,
                "outside_sequence_sha256": "2" * 64,
                "metrics": {"outside_samples": block_index},
                "absolute_uncertainty": {},
            },
            "samples": samples,
        })
    plan = {
        "plan_schema": 1,
        "evidence_type": "aim_production_red_output_off_plan",
        "asset_id": "dbed788b-legacy-output-off-v1",
        "source_clock_session_id": "synthetic:dbed788b-fixed-windows-v1",
        "production_plant_profile_id": "P-LEGACY-D3-G05215",
        "plant_profiles": [{
            "plant_profile_id": "P-LEGACY-D3-G05215",
            "delay_samples": 3,
            "pixels_per_completed_count_x": -0.5215,
            "pixels_per_completed_count_y": -0.5215,
        }],
        "provenance": {"fixture_sha256": "3" * 64},
        "blocks": blocks,
    }
    path.write_text(
        json.dumps(plan, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return plan


def write_magnitude_analysis(path: pathlib.Path) -> dict[str, object]:
    analysis = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_magnitude_domain_analysis",
        "status": "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "production_aim_changed": False,
        "analysis_contract": {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_a2_magnitude_domain_contract",
            "physical_output_capability": False,
            "production_aim_changed": False,
            "model": {
                "family": "delayed_static_gain_diagnostic",
                "input":
                    "backend_completed_cumulative_position_counts",
                "delay_samples": 4,
                "output_feedback_used": False,
            },
            "split": {
                "fit": "primary_whole_blocks_only",
                "evaluation": "validation_whole_blocks_only",
                "random_frame_split_allowed": False,
            },
            "deletion_tests": {
                "new_production_gain_may_be_claimed": False,
            },
            "contract_semantic_sha256": "4" * 64,
        },
        "evaluation": {
            "status": "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN",
            "physical_output_capability": False,
            "production_aim_changed": False,
            "f1_deleted_for_magnitude_domain": True,
            "new_production_gain_claimed": False,
            "invalid_reasons": [],
            "primary_model": {
                "delay_samples": 4,
                "fit_role": "primary_whole_blocks_only",
                "validation_used_for_refit": False,
                "left_gain": -0.496,
                "right_gain": -0.504,
            },
        },
    }
    path.write_text(
        json.dumps(analysis, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return analysis


def invoke(builder: pathlib.Path, baseline: pathlib.Path,
           replay: pathlib.Path, physical: pathlib.Path,
           hypothesis: str, output: pathlib.Path,
           magnitude_analysis: pathlib.Path | None = None
           ) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable, str(builder),
        "--baseline-plan", str(baseline),
        "--replay-runtime-csv", str(replay),
        "--physical-runtime-csv", str(physical),
        "--hypothesis", hypothesis,
        "--output", str(output),
    ]
    if magnitude_analysis is not None:
        command.extend(["--magnitude-analysis", str(magnitude_analysis)])
    return subprocess.run(
        command, capture_output=True, text=True, encoding="utf-8",
        errors="replace")


def assert_common_unchanged(candidate: dict[str, object],
                            baseline: dict[str, object],
                            allow_appended_profiles: bool = False) -> None:
    expect(candidate["plan_schema"] == baseline["plan_schema"] and
           candidate["evidence_type"] == baseline["evidence_type"] and
           candidate["source_clock_session_id"] ==
               baseline["source_clock_session_id"] and
           candidate["production_plant_profile_id"] ==
               baseline["production_plant_profile_id"],
           "反事实不得改变 red schema、source session 或 production profile")
    if allow_appended_profiles:
        expect(candidate["plant_profiles"][:len(baseline["plant_profiles"])] ==
                   baseline["plant_profiles"],
               "幅值反事实必须原样保留全部冻结 plant profiles")
    else:
        expect(candidate["plant_profiles"] == baseline["plant_profiles"],
               "初态/时钟反事实不得改变 plant profiles")
    for actual, expected in zip(candidate["blocks"], baseline["blocks"]):
        for field in ("block_id", "role", "block_kind", "score_begin",
                      "score_end", "control_center", "roi_size",
                      "measured_reference"):
            expect(actual[field] == expected[field],
                   f"反事实不得改变 block 字段 {field}")
        for actual_sample, expected_sample in zip(
                actual["samples"], expected["samples"]):
            for field in ("source_sequence", "source_timestamp",
                          "captured_at_ns", "world_delta_x",
                          "world_delta_y", "box_width", "box_height",
                          "pose", "visible", "target_id", "lock_active",
                          "backend_failure"):
                expect(actual_sample[field] == expected_sample[field],
                       f"反事实不得改变 sample 字段 {field}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--builder", required=True, type=pathlib.Path)
    parser.add_argument("--test-root", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    root = arguments.test_root.resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    baseline_path = root / "baseline.json"
    replay_path = root / "replay.csv"
    physical_path = root / "physical.csv"
    magnitude_path = root / "magnitude-analysis.json"
    baseline = write_baseline(baseline_path)
    write_runtime(replay_path, physical=False)
    write_runtime(physical_path, physical=True)
    write_magnitude_analysis(magnitude_path)

    plans: dict[str, dict[str, object]] = {}
    for hypothesis in (
            "initial-position", "control-clock",
            "initial-position-control-clock"):
        output_path = root / f"{hypothesis}.json"
        built = invoke(arguments.builder, baseline_path, replay_path,
                       physical_path, hypothesis, output_path)
        expect(built.returncode == 0,
               f"{hypothesis} plan 必须成功: {built.stdout}{built.stderr}")
        plans[hypothesis] = json.loads(
            output_path.read_text(encoding="utf-8"))
        original_sha256 = file_sha256(output_path)
        repeated = invoke(arguments.builder, baseline_path, replay_path,
                          physical_path, hypothesis, output_path)
        expect(repeated.returncode != 0 and
               file_sha256(output_path) == original_sha256,
               "反事实 builder 必须拒绝覆盖既有输出")

    initial = plans["initial-position"]
    clock = plans["control-clock"]
    combined = plans["initial-position-control-clock"]
    for plan in plans.values():
        assert_common_unchanged(plan, baseline)
        provenance = plan["provenance"]["fidelity_counterfactual"]
        expect(provenance["baseline_plan_sha256"] ==
                   file_sha256(baseline_path) and
               provenance["replay_runtime_sha256"] ==
                   file_sha256(replay_path) and
               provenance["physical_runtime_sha256"] ==
                   file_sha256(physical_path),
               "反事实 plan 必须 hash-bind 三个输入")

    for block_index, (window_begin, _) in enumerate(WINDOWS):
        baseline_block = baseline["blocks"][block_index]
        expected_initial_x = 150.0 + (window_begin + 1) / 1000.0
        expect(initial["blocks"][block_index]["initial_world_aim_point"] ==
                   [expected_initial_x, 160.0] and
               combined["blocks"][block_index]["initial_world_aim_point"] ==
                   [expected_initial_x, 160.0] and
               clock["blocks"][block_index]["initial_world_aim_point"] ==
                   baseline_block["initial_world_aim_point"],
               "initial-position 只能恢复 output-off matched box 初始 X")
        for local_index in range(SAMPLES_PER_BLOCK):
            baseline_sample = baseline_block["samples"][local_index]
            initial_sample = initial["blocks"][block_index]["samples"][local_index]
            expect(initial_sample["control_at_ns"] ==
                       baseline_sample["control_at_ns"] and
                   initial_sample["controller_dt_ns"] ==
                       baseline_sample["controller_dt_ns"],
                   "initial-position 不得改变控制时钟")
            physical_sequence = window_begin + local_index + 470
            age_ns = round((2.75 + (physical_sequence % 7) * 0.05) *
                           1_000_000)
            expected_control_at = baseline_sample["captured_at_ns"] + age_ns
            for clock_plan in (clock, combined):
                actual_sample = clock_plan["blocks"][block_index]["samples"][local_index]
                expect(actual_sample["control_at_ns"] == expected_control_at,
                       "control-clock 必须使用 Physical source_to_control")
                if local_index != 0:
                    previous = clock_plan["blocks"][block_index]["samples"][local_index - 1]
                    expect(actual_sample["controller_dt_ns"] ==
                               actual_sample["control_at_ns"] -
                               previous["control_at_ns"],
                           "controller_dt 必须由同一 Physical control clock 导出")

    magnitude_output = root / "a2-magnitude-domain-plant.json"
    magnitude_built = invoke(
        arguments.builder, baseline_path, replay_path, physical_path,
        "a2-magnitude-domain-plant", magnitude_output, magnitude_path)
    expect(magnitude_built.returncode == 0,
           "A2 幅值 plant 反事实必须成功: " +
           magnitude_built.stdout + magnitude_built.stderr)
    magnitude = json.loads(magnitude_output.read_text(encoding="utf-8"))
    assert_common_unchanged(
        magnitude, baseline, allow_appended_profiles=True)
    expect(magnitude["blocks"] == baseline["blocks"],
           "A2 幅值 plant 反事实不得改变任一 block/sample")
    expected_profiles = [
        {
            "plant_profile_id": "P-A2-MAG-D4-LEFT",
            "delay_samples": 4,
            "pixels_per_completed_count_x": -0.496,
            "pixels_per_completed_count_y": -0.5215,
        },
        {
            "plant_profile_id": "P-A2-MAG-D4-MID",
            "delay_samples": 4,
            "pixels_per_completed_count_x": -0.5,
            "pixels_per_completed_count_y": -0.5215,
        },
        {
            "plant_profile_id": "P-A2-MAG-D4-RIGHT",
            "delay_samples": 4,
            "pixels_per_completed_count_x": -0.504,
            "pixels_per_completed_count_y": -0.5215,
        },
    ]
    expect(magnitude["plant_profiles"][-3:] == expected_profiles,
           "A2 幅值反事实只能追加 Primary left/mid/right profiles")
    magnitude_provenance = magnitude["provenance"]["fidelity_counterfactual"]
    expect(magnitude_provenance["magnitude_analysis_sha256"] ==
               file_sha256(magnitude_path) and
           magnitude_provenance["magnitude_contract_semantic_sha256"] ==
               "4" * 64 and
           magnitude_provenance["validation_used_for_refit"] is False and
           magnitude_provenance["new_production_gain_claimed"] is False and
           magnitude_provenance["production_plant_profile_changed"] is False,
           "A2 幅值反事实必须绑定分析且明确不产生生产增益")

    missing_analysis = invoke(
        arguments.builder, baseline_path, replay_path, physical_path,
        "a2-magnitude-domain-plant", root / "missing-analysis.json")
    expect(missing_analysis.returncode != 0,
           "A2 幅值 plant 反事实缺分析输入时必须 fail closed")

    invalid_analysis_path = root / "invalid-magnitude-analysis.json"
    invalid_analysis = write_magnitude_analysis(invalid_analysis_path)
    invalid_analysis["evaluation"]["new_production_gain_claimed"] = True
    invalid_analysis_path.write_text(
        json.dumps(invalid_analysis, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    invalid = invoke(
        arguments.builder, baseline_path, replay_path, physical_path,
        "a2-magnitude-domain-plant", root / "invalid-analysis-plan.json",
        invalid_analysis_path)
    expect(invalid.returncode != 0,
           "声明生产增益的 A2 分析不得进入离线 plant 反事实")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
