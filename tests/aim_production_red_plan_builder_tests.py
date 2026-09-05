#!/usr/bin/env python3
"""验证 dbed788b output-off whole-block plan 的来源与 41/7 参考。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys

from xen_owned_test_directory import owned_test_directory


WINDOWS = ((40, 340), (700, 1000), (1400, 1700), (2099, 2399))


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: object) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def write_measured_runtime(path: pathlib.Path) -> None:
    header = [
        "sequence", "aim_control_center_x",
        "aim_matched_observation_x1", "aim_matched_observation_x2",
        "aim_base_x", "aim_controller_dt_ms",
    ]
    outside_indices: set[int] = set()
    # 第二段 36 个、最长 7；第四段 5 个连续样本，合计 41/7。
    for begin in (80, 90, 100, 110, 120):
        outside_indices.update(range(WINDOWS[1][0] + begin,
                                     WINDOWS[1][0] + begin + 7))
    outside_indices.add(WINDOWS[1][0] + 130)
    outside_indices.update(range(WINDOWS[3][0] + 100,
                                 WINDOWS[3][0] + 105))
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        for sidecar_index in range(2400):
            # builder 固定复算：sidecar frame i 对应 Runtime seq i+470，
            # world transition i 对应除首帧后的 seq i+471。
            sequence = sidecar_index + 470
            transition_index = sidecar_index - 1
            outside = transition_index in outside_indices
            writer.writerow([
                sequence,
                "160.0",
                "161.0" if outside else "150.0",
                "181.0" if outside else "170.0",
                "177.158386" if outside else "164.0",
                "4.1667",
            ])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--builder", required=True, type=pathlib.Path)
    parser.add_argument("--fixture-header", required=True, type=pathlib.Path)
    parser.add_argument("--test-root", required=True, type=pathlib.Path)
    parser.add_argument("--powershell-executable", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    with owned_test_directory(
            arguments.test_root, arguments.powershell_executable) as root:
        return run_contract(arguments, root)


def run_contract(arguments: argparse.Namespace, root: pathlib.Path) -> int:
    measured_path = root / "measured-runtime.csv"
    output_path = root / "plan.json"
    write_measured_runtime(measured_path)

    command = [
        sys.executable, str(arguments.builder),
        "--fixture-header", str(arguments.fixture_header),
        "--measured-runtime-csv", str(measured_path),
        "--output", str(output_path),
    ]
    built = subprocess.run(
        command, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    expect(built.returncode == 0,
           f"plan builder 必须成功: {built.stdout}{built.stderr}")
    plan = json.loads(output_path.read_text(encoding="utf-8"))
    expect(plan["plan_schema"] == 1 and
           plan["evidence_type"] ==
               "aim_production_red_output_off_plan" and
           plan["source_clock_session_id"].startswith("synthetic:"),
           "plan 必须显式声明 v1 synthetic source clock")
    expect([profile["plant_profile_id"] for profile in
            plan["plant_profiles"]] == [
                "P-LEGACY-D3-G05215", "P-F1-LEFT",
                "P-F1-MID", "P-F1-RIGHT"],
           "plan 必须冻结 legacy 与 F1 left/mid/right 全 profile 集")
    blocks = plan["blocks"]
    expect(len(blocks) == 4 and
           [block["block_id"] for block in blocks] ==
               [f"DEV-PIX-S{i}" for i in range(4)] and
           all(len(block["samples"]) == 300 and
               block["score_begin"] == 80 and block["score_end"] == 300
               for block in blocks),
           "四个像素窗必须保持 reset-separated 300/80..299 whole blocks")
    expect(sum(block["measured_reference"]["metrics"]["outside_samples"]
               for block in blocks) == 41 and
           max(block["measured_reference"]["metrics"]
               ["longest_outside_samples"] for block in blocks) == 7,
           "独立 measured reference 必须保留真实 41/7，而非 6/3")
    for block in blocks:
        identities = [[
            row["source_sequence"], row["source_timestamp"],
            plan["source_clock_session_id"]] for row in block["samples"]]
        expect(block["measured_reference"]["source_identity_sha256"] ==
               canonical_sha256(identities),
               "measured reference 必须绑定 producer 将消费的 source identity")
        sequences = [row["source_sequence"] for row in block["samples"]]
        timestamps = [row["source_timestamp"] for row in block["samples"]]
        expect(all(right > left for left, right in
                   zip(sequences, sequences[1:])) and
               all(right > left for left, right in
                   zip(timestamps, timestamps[1:])),
               "每个 block 的 source sequence/timestamp 必须原生严格递增")

    original_sha256 = hashlib.sha256(output_path.read_bytes()).hexdigest()
    repeated = subprocess.run(
        command, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    expect(repeated.returncode != 0 and
           hashlib.sha256(output_path.read_bytes()).hexdigest() ==
               original_sha256,
           "plan builder 必须拒绝覆盖既有输出")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
