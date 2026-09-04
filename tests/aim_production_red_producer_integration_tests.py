#!/usr/bin/env python3
"""验证 output-off producer bundle 可由正式 evaluator 直接消费。"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def make_plan() -> dict[str, object]:
    samples = []
    for index in range(12):
        samples.append({
            "source_sequence": 500 + index,
            "source_timestamp": 900000 + index * 41667,
            "captured_at_ns": 2_000_000_000 + index * 4_166_700,
            "control_at_ns": 2_003_000_000 + index * 4_166_700,
            "controller_dt_ns": 4_166_700,
            "world_delta_x": 0.0 if index == 0 else 1.25,
            "world_delta_y": 0.0,
            "box_width": 20.0,
            "box_height": 40.0,
            "pose": "body",
            "visible": True,
            "target_id": 11,
            "lock_active": True,
            "backend_failure": False,
        })
    return {
        "plan_schema": 1,
        "evidence_type": "aim_production_red_output_off_plan",
        "asset_id": "producer-evaluator-integration-test",
        "source_clock_session_id": "synthetic:integration-clock-v1",
        "production_plant_profile_id": "P-TEST-D3",
        "plant_profiles": [{
            "plant_profile_id": "P-TEST-D3",
            "delay_samples": 3,
            "pixels_per_completed_count_x": -0.5,
            "pixels_per_completed_count_y": -0.5,
        }],
        "blocks": [{
            "block_id": "RED-EVALUATOR-SEAM",
            "role": "development",
            "block_kind": "dynamic",
            "score_begin": 2,
            "score_end": len(samples),
            "initial_world_aim_point": [180.0, 160.0],
            "control_center": [160.0, 160.0],
            "roi_size": [320, 320],
            # 故意冻结不相符的独立参考；正确产物应到达 fidelity 门，
            # 不能因 producer 缺 schema/source/completed 而更早失败。
            "measured_reference": {
                "source_identity_sha256": "0" * 64,
                "outside_sequence_sha256": "1" * 64,
                "metrics": {
                    "outside_samples": 99,
                    "outside_duration_ns": 99,
                    "longest_outside_samples": 99,
                    "longest_outside_ns": 99,
                    "outside_area_px_ns": 99.0,
                    "max_excess_x_px": 99.0,
                    "max_abs_error_x_px": 99.0,
                },
                "absolute_uncertainty": {
                    "outside_area_px_ns": 0.0,
                    "max_excess_x_px": 0.0,
                    "max_abs_error_x_px": 0.0,
                },
            },
            "samples": samples,
        }],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--test-root", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    root = arguments.test_root.resolve()
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    plan_path = root / "plan.json"
    config_path = root / "config.ini"
    measured_path = root / "measured.csv"
    bundle_path = root / "bundle"
    report_path = root / "evaluation.json"
    plan_path.write_text(
        json.dumps(make_plan(), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    config_path.write_text("""[aim]
person_class_ids=0,2
head_class_ids=1,3
high_confidence=0.25
low_confidence=0.10
min_confirmed_hits=2
max_lost_frames=8
min_iou=0.10
max_center_distance=0.25
switch_margin=0.20
switch_confirm_frames=3
switch_cooldown_frames=5
acquisition_range_percent=90
body_aim_height_ratio=0.35
body_aim_range_percent=50
deadzone_pixels=1.5
smoothing=0.475
counts_per_pixel_x=0.425
counts_per_pixel_y=0.4
max_counts_per_frame=14
enable_delay_compensation=true
control_delay_ms=15
max_delay_compensation_ms=44
max_delay_compensation_percent=15
enable_prediction=false
max_prediction_lead_percent=35
predicted_gain=0.5
""", encoding="utf-8")
    measured_path.write_text(
        "independent measured reference\n", encoding="utf-8")

    produced = subprocess.run([
        str(arguments.producer),
        "--plan", str(plan_path),
        "--config", str(config_path),
        "--measured-reference-source", str(measured_path),
        "--output-directory", str(bundle_path),
    ], capture_output=True, text=True, encoding="utf-8", errors="replace")
    expect(produced.returncode == 0,
           f"producer CLI 必须成功: {produced.stdout}{produced.stderr}")
    expect("physical_output_capability=false" in produced.stdout,
           "CLI 必须明确声明没有物理输出能力")

    evaluated = subprocess.run([
        sys.executable, str(arguments.evaluator),
        "--manifest", str(bundle_path / "manifest.json"),
        "--output", str(report_path),
    ], capture_output=True, text=True, encoding="utf-8", errors="replace")
    expect(evaluated.returncode == 0,
           f"evaluator CLI 必须可消费 producer bundle: "
           f"{evaluated.stdout}{evaluated.stderr}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    expect(report["status"] == "BASELINE_REPLAY_FIDELITY_INVALID",
           f"完整 producer bundle 应抵达 fidelity 门: {report}")
    expect(not any(code.startswith("MISSING_") for code in
                   report["failure_codes"]),
           f"producer 不得留下 schema/source/completed 缺口: {report}")
    expect(report["physical_output_capability"] is False and
           report["physical_dispatch_count"] == 0,
           "组合报告必须保持 output-off")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
