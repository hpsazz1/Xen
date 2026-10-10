"""Actual evaluator CLI rejects inconsistent time fields in rehashed evidence."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import uuid
from aim_production_red_evaluator_tests import (
    MODULE_PATH, measured_reference, valid_row, write_manifest, write_multi_manifest)


def rows_for(outside, block="DEV-PIX-S0", asset="time-contract"):
    rows = [valid_row(i, value, len(outside), block_id=block, asset_id=asset) for i, value in enumerate(outside)]
    for row in rows:
        for key in ("captured_at_ns", "control_at_ns", "backend_completed_at_ns"):
            row[key] += 100
    return rows


def collapse(rows):
    first = rows[0]
    for row in rows[1:]:
        for key in ("captured_at_ns", "control_at_ns", "backend_completed_at_ns"):
            row[key] = first[key]


def run_case(root, name):
    root.mkdir()
    candidate = name.startswith("candidate")
    rows = rows_for([True, True, False] if candidate else [True, False], "TAIL" if candidate else "DEV-PIX-S0")
    reference = measured_reference(rows, 2 if candidate else 1, 2 if candidate else 1)
    expected = "CANDIDATE_DEVELOPMENT_GREEN" if candidate else "BASELINE_RED_LOCKED"
    if name in ("collapse", "candidate-collapse"):
        collapse(rows)
        expected = "RED_INVALID"
    elif name == "backward":
        for key in ("captured_at_ns", "control_at_ns", "backend_completed_at_ns"):
            rows[1][key] = rows[0][key] - 1
        expected = "RED_INVALID"
    elif name == "dt-mismatch":
        rows[1]["controller_dt_ns"] += 1
        expected = "RED_INVALID"
    elif name in ("age-mismatch", "negative-age"):
        rows[1]["observation_age_ns"] = -1 if name == "negative-age" else 999
        expected = "RED_INVALID"
    elif name == "uneven":
        for key in ("captured_at_ns", "control_at_ns", "backend_completed_at_ns"):
            rows[1][key] += 7
        rows[1]["controller_dt_ns"] = 17
    elif name == "first-dt":
        rows = rows[:1]
        rows[0]["score_end"] = 1
        rows[0]["controller_dt_ns"] = 42
        reference = measured_reference(rows, 1, 1)
        for key in ("outside_duration_ns", "longest_outside_ns", "outside_area_px_ns"):
            reference["metrics"][key] = 42
    if candidate:
        improved = rows_for([True, False, False], "TAIL")
        if name == "candidate-collapse":
            collapse(improved)
        common = dict(block_id="TAIL", role="development", block_kind="tail", plant_profile_id="P-LEGACY-D3-G05215")
        manifest = write_multi_manifest(root, [
            ({**common, "trace_id": "TAIL:B0", "variant": "B0"}, rows, reference),
            ({**common, "trace_id": "TAIL:C", "variant": "C"}, improved, None),
            ({**common, "trace_id": "TAIL:C-D0", "variant": "C-D0"}, copy.deepcopy(rows), None),
        ], asset_id="time-contract")
    else:
        manifest = write_manifest(root, rows, stem=name, measured_reference=reference)
    output = root / "evaluation.json"
    result = subprocess.run([sys.executable, str(MODULE_PATH), "--manifest", str(manifest), "--output", str(output)],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
    report = json.loads(output.read_text(encoding="utf-8")) if output.is_file() else {}
    return dict(case=name, passed=result.returncode == 0 and report.get("status") == expected,
                expected=expected, status=report.get("status"), exit_code=result.returncode,
                failure_codes=report.get("failure_codes"), stderr=result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("evaluator-time-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    names = ("healthy", "collapse", "backward", "dt-mismatch", "age-mismatch", "negative-age",
             "uneven", "first-dt", "candidate-healthy", "candidate-collapse")
    results = [run_case(root / name, name) for name in names]
    (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
