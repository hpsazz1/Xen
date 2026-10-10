"""Actual output-off producer CLI integer-domain tests with retained fixtures."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import uuid
from aim_production_red_producer_integration_tests import make_plan


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("integer-bounds-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    config, measured = root / "config.ini", root / "measured.txt"
    config.write_text("[aim]\nperson_class_ids=0,2\nhead_class_ids=1,3\n", encoding="utf-8")
    measured.write_text("synthetic reference; no measured acceptance\n", encoding="utf-8")
    baseline = make_plan()
    baseline["blocks"][0]["samples"] = baseline["blocks"][0]["samples"][:8]
    baseline["blocks"][0]["score_end"] = 8
    cases = [("healthy", {}, True)]
    for name, begin, end in [
        ("score-pair-wrap", 4294967298, 4294967304),
        ("score-begin-wrap", 4294967298, 8),
        ("score-end-wrap", 2, 4294967304),
        ("score-negative-wrap", -4294967294, 8),
        ("score-unsigned-wrap", 18446744069414584322, 8),
        ("score-int64-overflow", 2, 9223372036854775808),
        ("score-uint64-max", 2, 18446744073709551615),
        ("score-float", 2.0, 8), ("score-bool", True, 8),
        ("score-reversed", 8, 2), ("score-empty", 2, 2), ("score-past-end", 2, 9),
    ]:
        cases.append((name, {"score_begin": begin, "score_end": end}, False))
    for name, value in [("delay-wrap", 4294967299), ("delay-negative-wrap", -4294967293), ("delay-zero", 0)]:
        cases.append((name, {"delay": value}, False))
    for name, roi, accepted in [
        ("roi-width-wrap", [4294967616, 320], False),
        ("roi-height-negative-wrap", [320, -4294966976], False),
        ("roi-unsigned-wrap", [18446744069414584640, 320], False),
        ("roi-int32-overflow", [2147483648, 320], False),
        ("roi-zero", [0, 320], False), ("roi-int32-max", [2147483647, 320], True),
    ]:
        cases.append((name, {"roi_size": roi}, accepted))
    results = []
    for name, edits, accepted in cases:
        plan = copy.deepcopy(baseline)
        if "delay" in edits:
            plan["plant_profiles"][0]["delay_samples"] = edits["delay"]
        else:
            plan["blocks"][0].update(edits)
        directory = root / name
        directory.mkdir()
        path, bundle = directory / "plan.json", directory / "bundle"
        path.write_text(json.dumps(plan), encoding="utf-8")
        result = subprocess.run([
            str(args.producer.resolve()), "--plan", str(path), "--config", str(config),
            "--measured-reference-source", str(measured), "--output-directory", str(bundle),
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        passed = (result.returncode == 0 and (bundle / "manifest.json").is_file() and
                  "physical_output_capability=false" in result.stdout) if accepted else result.returncode == 2 and not bundle.exists()
        if accepted and passed:
            manifest = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
            trace = manifest["traces"][0]
            rows = [json.loads(line) for line in (bundle / trace["relative_path"]).read_text(encoding="utf-8").splitlines()]
            passed = trace["score_begin"] == 2 and trace["score_end"] == 8 and len(rows) == 8 and all(
                row["score_begin"] == 2 and row["score_end"] == 8 for row in rows)
        record = dict(case=name, passed=passed, expected_accept=accepted, exit_code=result.returncode,
                      bundle_exists=bundle.exists(), stdout=result.stdout, stderr=result.stderr)
        results.append(record)
        print(json.dumps(record, ensure_ascii=False))
    (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"retained={root}")
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
