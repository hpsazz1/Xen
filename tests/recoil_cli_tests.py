"""Offline cross-language integration; argv[1] is the rebuilt absolute CLI path."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dataset_fixture", ROOT / "tests/recoil_dataset_tests.py")
FIXTURES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURES)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def complete_profile(profile):
    """Only exact integers and .0 doubles: matches nlohmann sorted JSON encoding."""
    profile.update(phase_tolerance_ms=20.0, recovery_ms=100.0)
    profile["source"] = {"repository": "synthetic:test-only", "commit": "synthetic",
                         "sha256": "b" * 64, "license": "synthetic", "source_unit": "device_counts",
                         "conversion_revision": "test-only", "redistribution_verified": True}
    profile["calibration"] = {"game_build": "synthetic_build", "input_path": "kmbox_net",
                              "conditions": "synthetic_condition", "evidence": "synthetic:test-only",
                              "sensitivity": 1.0}
    fingerprint = {"schema_version": 1, "weapon_id": profile["weapon_id"], "profile_id": profile["id"],
                   "unit": profile["unit"], "sample_semantics": profile["sample_semantics"],
                   "fire_mode": profile["fire_mode"], "game_build": "synthetic_build", "input_path": "kmbox_net",
                   "conditions": "synthetic_condition", "sensitivity": 1.0, "source_sha256": "b" * 64,
                   "phase_tolerance_ms": 20.0, "recovery_ms": 100.0}
    profile["environment_fingerprint"] = json.dumps(fingerprint, sort_keys=True, separators=(",", ":"))


def run(command, directory, environment):
    return subprocess.run([str(arg) for arg in command], cwd=directory, env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)


def main():
    check(len(sys.argv) == 2, "usage: recoil_cli_tests.py <absolute xen_recoil_tuner.exe>")
    cli = Path(sys.argv[1])
    check(cli.is_absolute() and cli.is_file(), "CLI must be an existing absolute path")
    fixture = FIXTURES.DatasetTests()
    fixture.setUp()
    try:
        directory = fixture.directory
        for path in directory.glob("synthetic-run-*.json"):
            debug = json.loads(path.read_text(encoding="utf-8"))
            for profile in debug["recoil"]["execution"]["profiles"]:
                complete_profile(profile)
            path.write_text(json.dumps(debug), encoding="utf-8")
        all_entries = list(fixture.manifest["trials"])
        for experiment in fixture.manifest["response_experiments"]:
            all_entries.extend((experiment["baseline"], experiment["changed"]))
        for entry in all_entries:
            entry["measurement"]["noise"] = [0.01, 0.02]
        fixture.save_manifest()
        first = json.loads((directory / fixture.manifest["base"]["debug_report"]).read_text(encoding="utf-8"))
        base = first["recoil"]["execution"]["profiles"][0]
        base_path = directory / "base.json"
        base_path.write_text(json.dumps(base), encoding="utf-8")
        original = base_path.read_bytes()
        environment = os.environ.copy()
        # Per-child only. Never modify the real process/user environment or ledger.
        local_data = directory / "isolated-local-app-data"
        local_data.mkdir()
        environment["LOCALAPPDATA"] = str(local_data)
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        dataset = directory / "dataset.json"
        converted = run([sys.executable, ROOT / "scripts/build_recoil_dataset.py", fixture.path, dataset], directory, environment)
        check(converted.returncode == 0, "dataset conversion failed: " + repr(converted.stderr))
        output = directory / "candidate-output"
        optimized = run([cli, "optimize", dataset, base_path, output, "1", "2"], directory, environment)
        check(optimized.returncode == 0, "CLI optimize failed: " + repr(optimized.stdout + optimized.stderr))
        report = json.loads((output / "analysis/report.json").read_text(encoding="utf-8"))
        check(report["status"] == "CANDIDATE_VALIDATED", "CLI did not validate candidate")
        profiles = list(output.glob("*.json"))
        check(len(profiles) == 1 and profiles[0].name == "synthetic-r2.json", "root must contain one editor-loadable profile")
        candidate = json.loads(profiles[0].read_text(encoding="utf-8"))
        check(candidate["id"] == base["id"] and candidate["weapon_id"] == base["weapon_id"] and candidate["revision"] == 2,
              "candidate lost base identity or requested revision")
        check(candidate["schema_version"] == 1 and candidate["unit"] == "device_counts" and
              candidate["sample_semantics"] == "cumulative" and candidate["state"] == "SCHEMA_VALID", "invalid production profile schema")
        check(candidate["phase_tolerance_ms"] is None and candidate["recovery_ms"] is None and
              candidate["calibration"]["evidence"] == "", "candidate inherited physical calibration")
        check(candidate["points"][0] == [0, 0, 0] and all(isinstance(p, list) and len(p) == 3 for p in candidate["points"]),
              "profile must use production [time,x,y] points")
        generic = json.loads((output / "analysis/candidate.json").read_text(encoding="utf-8"))
        check(candidate["points"] == [[p["time_ms"], p["x_counts"], p["y_counts"]] for p in generic["points"]],
              "editor profile does not equal validated candidate")
        check(base_path.read_bytes() == original and not list(directory.rglob("active.json")), "optimization altered base or active selection")
        check(str(profiles[0]).encode() in optimized.stdout, "CLI did not print saved production filename")
        registry = local_data / "Xen/recoil-tuner-usage-v1/usage.json"
        check(registry.is_file(), "isolated holdout ledger missing")
        rejected_output = directory / "reused-holdout-output"
        second = run([cli, "optimize", dataset, base_path, rejected_output, "2", "3"], directory, environment)
        check(second.returncode != 0, "same holdouts were reused across generations")
        rejected = json.loads((rejected_output / "analysis/report.json").read_text(encoding="utf-8"))
        check(rejected["status"] == "INVALID_DATA" and not list(rejected_output.glob("*-r*.json")),
              "reused holdout produced another profile")
        check(not list(directory.rglob("active.json")), "rejected retry published active selection")
        print("recoil_cli_tests passed: conversion -> CLI -> SCHEMA_VALID profile -> holdout reuse rejected")
    finally:
        fixture.tearDown()


if __name__ == "__main__":
    main()
