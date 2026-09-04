#!/usr/bin/env python3
"""从冻结 fixture 与独立 measured Runtime 构建 dbed788b output-off plan。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import re
import sys
from typing import Any


PLAN_SCHEMA = 1
EVIDENCE_TYPE = "aim_production_red_output_off_plan"
ASSET_ID = "dbed788b-legacy-output-off-v1"
SOURCE_CLOCK_SESSION_ID = "synthetic:dbed788b-fixed-windows-v1"
RUNTIME_SEQUENCE_OFFSET = 469
WINDOWS = ((40, 340), (700, 1000), (1400, 1700), (2099, 2399))
SEGMENT_SAMPLES = 300
WARMUP_SAMPLES = 80
SAMPLE_PATTERN = re.compile(
    r"^\s*\{\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
    r"\s*(\d+)\s*,\s*(true|false)\s*\},\s*$")


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_sha256(value: object) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def parse_fixture(path: pathlib.Path) -> list[tuple[int, int, int, int, bool]]:
    text = path.read_text(encoding="utf-8")
    expected_constants = {
        "kSegmentCount": "4",
        "kSegmentSamples": "300",
        "kWarmupSamplesPerSegment": "80",
        "kPlantDelayFrames": "3",
        "kPlantPixelsPerCount": "0.5215f",
    }
    for name, expected in expected_constants.items():
        match = re.search(
            rf"inline\s+constexpr\s+(?:int|float)\s+{name}\s*=\s*([^;]+);",
            text)
        if match is None or match.group(1).strip() != expected:
            raise ValueError(f"fixture 常量 {name} 未冻结为 {expected}")
    samples: list[tuple[int, int, int, int, bool]] = []
    for line in text.splitlines():
        match = SAMPLE_PATTERN.match(line)
        if match is None:
            continue
        samples.append((
            int(match.group(1)), int(match.group(2)),
            int(match.group(3)), int(match.group(4)),
            match.group(5) == "true"))
    if len(samples) != len(WINDOWS) * SEGMENT_SAMPLES:
        raise ValueError(
            f"fixture 样本数必须为 1200，实际 {len(samples)}")
    for index, sample in enumerate(samples):
        expected_reset = index % SEGMENT_SAMPLES == 0
        if sample[4] is not expected_reset or sample[3] <= 0 or \
                sample[1] <= 0 or sample[2] <= 0:
            raise ValueError(f"fixture reset/尺寸/dt 合同无效，index={index}")
    return samples


def load_runtime(path: pathlib.Path) -> tuple[list[str], dict[int, list[str]]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = [row for row in csv.reader(stream) if row]
    try:
        header = next(row for row in rows if row[0] == "sequence")
    except StopIteration as error:
        raise ValueError("measured Runtime 缺少 sequence header") from error
    required = {
        "sequence", "aim_control_center_x",
        "aim_matched_observation_x1", "aim_matched_observation_x2",
        "aim_base_x", "aim_controller_dt_ms",
    }
    if not required.issubset(header):
        raise ValueError(
            "measured Runtime 缺字段: " +
            ",".join(sorted(required - set(header))))
    by_sequence: dict[int, list[str]] = {}
    for row in rows:
        if len(row) != len(header) or not row[0].isdigit():
            continue
        sequence = int(row[0])
        if sequence in by_sequence:
            raise ValueError(f"measured Runtime sequence 重复: {sequence}")
        by_sequence[sequence] = row
    return header, by_sequence


def measured_reference(
        header: list[str], by_sequence: dict[int, list[str]],
        window_begin: int, identities: list[list[Any]]) -> dict[str, Any]:
    columns = {name: index for index, name in enumerate(header)}
    outside_flags: list[bool] = []
    outside_duration_ns = 0
    outside_area_px_ns = 0.0
    longest_samples = 0
    longest_duration_ns = 0
    current_samples = 0
    current_duration_ns = 0
    max_excess = 0.0
    max_error = 0.0
    for local_index in range(WARMUP_SAMPLES, SEGMENT_SAMPLES):
        transition_index = window_begin + local_index
        # sidecar frame 0 对齐 Runtime seq 470；world transition 0 使用
        # 下一帧，因此 transition i 的 measured 行是 i+471。
        sequence = transition_index + RUNTIME_SEQUENCE_OFFSET + 2
        row = by_sequence.get(sequence)
        if row is None:
            raise ValueError(
                f"measured Runtime 缺少 exact join sequence={sequence}")
        try:
            center = float(row[columns["aim_control_center_x"]])
            x1 = float(row[columns["aim_matched_observation_x1"]])
            x2 = float(row[columns["aim_matched_observation_x2"]])
            base = float(row[columns["aim_base_x"]])
            dt_ns = round(
                float(row[columns["aim_controller_dt_ms"]]) * 1_000_000)
        except ValueError as error:
            raise ValueError(
                f"measured Runtime 非数值 sequence={sequence}") from error
        if not all(math.isfinite(value) for value in (center, x1, x2, base)) \
                or x1 > x2 or dt_ns <= 0:
            raise ValueError(
                f"measured Runtime 几何/时间无效 sequence={sequence}")
        outside = center < x1 or center > x2
        excess = max(x1 - center, center - x2, 0.0)
        outside_flags.append(outside)
        max_excess = max(max_excess, excess)
        max_error = max(max_error, abs(base - center))
        if outside:
            outside_duration_ns += dt_ns
            outside_area_px_ns += excess * dt_ns
            current_samples += 1
            current_duration_ns += dt_ns
            longest_samples = max(longest_samples, current_samples)
            longest_duration_ns = max(
                longest_duration_ns, current_duration_ns)
        else:
            current_samples = 0
            current_duration_ns = 0
    return {
        "source_identity_sha256": canonical_sha256(identities),
        "outside_sequence_sha256": canonical_sha256(outside_flags),
        "metrics": {
            "outside_samples": sum(outside_flags),
            "outside_duration_ns": outside_duration_ns,
            "longest_outside_samples": longest_samples,
            "longest_outside_ns": longest_duration_ns,
            "outside_area_px_ns": outside_area_px_ns,
            "max_excess_x_px": max_excess,
            "max_abs_error_x_px": max_error,
        },
        # 参考由 hash-bound CSV 的十进制字节确定性解析；这里的零表示
        # 数字提取容差为零，不把它外推为物理测量误差为零。
        "absolute_uncertainty": {
            "outside_area_px_ns": 0.0,
            "max_excess_x_px": 0.0,
            "max_abs_error_x_px": 0.0,
        },
    }


def build_plan(fixture_path: pathlib.Path,
               runtime_path: pathlib.Path) -> dict[str, Any]:
    fixture = parse_fixture(fixture_path)
    header, by_sequence = load_runtime(runtime_path)
    blocks: list[dict[str, Any]] = []
    for segment, (window_begin, window_end) in enumerate(WINDOWS):
        if window_end - window_begin != SEGMENT_SAMPLES:
            raise ValueError("冻结 window 不是 300 samples")
        segment_samples = fixture[
            segment * SEGMENT_SAMPLES:(segment + 1) * SEGMENT_SAMPLES]
        captured_at_ns = 1_000_000_000_000 + window_begin * 10_000_000
        samples: list[dict[str, Any]] = []
        identities: list[list[Any]] = []
        for local_index, (
                world_delta, width, height, dt_us, _) in enumerate(
                    segment_samples):
            dt_ns = dt_us * 1000
            if local_index != 0:
                captured_at_ns += dt_ns
            source_sequence = window_begin + local_index + 1
            source_timestamp = captured_at_ns // 100
            samples.append({
                "source_sequence": source_sequence,
                "source_timestamp": source_timestamp,
                "captured_at_ns": captured_at_ns,
                "control_at_ns": captured_at_ns + 3_000_000,
                "controller_dt_ns": dt_ns,
                "world_delta_x": world_delta / 1000.0,
                "world_delta_y": 0.0,
                "box_width": width / 100.0,
                "box_height": height / 100.0,
                "pose": "body",
                "visible": True,
                "target_id": 1,
                "lock_active": True,
                "backend_failure": False,
            })
            identities.append([
                source_sequence, source_timestamp, SOURCE_CLOCK_SESSION_ID])
        blocks.append({
            "block_id": f"DEV-PIX-S{segment}",
            "role": "development",
            "block_kind": "dynamic",
            "score_begin": WARMUP_SAMPLES,
            "score_end": SEGMENT_SAMPLES,
            "initial_world_aim_point": [160.0, 160.0],
            "control_center": [160.0, 160.0],
            "roi_size": [320, 320],
            "measured_reference": measured_reference(
                header, by_sequence, window_begin, identities),
            "samples": samples,
        })
    return {
        "plan_schema": PLAN_SCHEMA,
        "evidence_type": EVIDENCE_TYPE,
        "asset_id": ASSET_ID,
        "source_clock_session_id": SOURCE_CLOCK_SESSION_ID,
        "production_plant_profile_id": "P-LEGACY-D3-G05215",
        "plant_profiles": [
            {
                "plant_profile_id": "P-LEGACY-D3-G05215",
                "delay_samples": 3,
                "pixels_per_completed_count_x": -0.5215,
                "pixels_per_completed_count_y": -0.5215,
            },
            {
                "plant_profile_id": "P-F1-LEFT",
                "delay_samples": 4,
                "pixels_per_completed_count_x": -0.387711133637,
                "pixels_per_completed_count_y": -0.5215,
            },
            {
                "plant_profile_id": "P-F1-MID",
                "delay_samples": 4,
                "pixels_per_completed_count_x": -0.3970329663485,
                "pixels_per_completed_count_y": -0.5215,
            },
            {
                "plant_profile_id": "P-F1-RIGHT",
                "delay_samples": 4,
                "pixels_per_completed_count_x": -0.406354799060,
                "pixels_per_completed_count_y": -0.5215,
            },
        ],
        "provenance": {
            "fixture_sha256": file_sha256(fixture_path),
            "measured_runtime_sha256": file_sha256(runtime_path),
            "runtime_sequence_offset": RUNTIME_SEQUENCE_OFFSET,
            "windows": [list(window) for window in WINDOWS],
            "source_identity_kind":
                "synthetic-block-clock-from-fixture-dt",
            "measurement_uncertainty_basis":
                "exact-decimal-extraction-only",
            "physical_output_capability": False,
        },
        "blocks": blocks,
    }


def write_json_atomic(path: pathlib.Path, value: dict[str, Any]) -> None:
    path = path.resolve()
    incoming = path.with_name(f".{path.name}.incoming")
    if path.exists() or incoming.exists():
        raise FileExistsError("拒绝覆盖既有 plan 或 incoming")
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
        description="构建 dbed788b hash-bound output-off whole-block plan")
    parser.add_argument("--fixture-header", required=True, type=pathlib.Path)
    parser.add_argument(
        "--measured-runtime-csv", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args(argv)
    try:
        plan = build_plan(
            arguments.fixture_header.resolve(),
            arguments.measured_runtime_csv.resolve())
        write_json_atomic(arguments.output, plan)
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        print(f"dbed788b plan builder failed: {error}", file=sys.stderr)
        return 2
    print(
        "dbed788b plan builder: physical_output_capability=false, "
        f"blocks={len(plan['blocks'])}, samples="
        f"{sum(len(block['samples']) for block in plan['blocks'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
