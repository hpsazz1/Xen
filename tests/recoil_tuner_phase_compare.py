"""Compare complete production reports with a frozen pre-change benchmark oracle."""
import json
import struct
import sys
from pathlib import Path


def exact(expected, actual, path):
    if type(expected) is not type(actual):
        raise AssertionError(f"{path}: type changed")
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            raise AssertionError(f"{path}: fields changed")
        for key in expected:
            exact(expected[key], actual[key], f"{path}.{key}")
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            raise AssertionError(f"{path}: length changed")
        for i, (left, right) in enumerate(zip(expected, actual)):
            exact(left, right, f"{path}[{i}]")
    elif isinstance(expected, float):
        if struct.pack("!d", expected) != struct.pack("!d", actual):
            raise AssertionError(f"{path}: double bits changed")
    elif expected != actual:
        raise AssertionError(f"{path}: value changed")


def compare(before, after):
    for key in ("fixture_seed", "warmup", "repeats"):
        exact(before[key], after[key], key)
    if len(before["cases"]) != len(after["cases"]):
        raise AssertionError("case count changed")
    for i, (left, right) in enumerate(zip(before["cases"], after["cases"])):
        for key in ("points", "phase_tolerance_ms", "trials", "oracle"):
            exact(left[key], right[key], f"case[{i}].{key}")
        if left["failures"] or right["failures"]:
            raise AssertionError(f"case[{i}]: production run failed")
        print(f"case[{i}] {left['points']} points: complete report bit-exact")


def self_test():
    exact({"v": [1.0, -0.0]}, {"v": [1.0, -0.0]}, "equal")
    cases = [(-0.0, 0.0), (1.0, 1.0000000000000002),
             (["a", "b"], ["b", "a"]), ({"a": 1}, {"b": 1}), (1, 1.0)]
    for left, right in cases:
        try:
            exact(left, right, "different")
        except AssertionError:
            continue
        raise AssertionError("comparison accepted a changed value")
    print("comparison self-test: signed zero/ULP/order/fields/type rejected")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        raise SystemExit(0)
    if len(sys.argv) != 3:
        raise SystemExit("usage: recoil_tuner_phase_compare.py baseline.json optimized.json")
    compare(*(json.loads(Path(path).read_text(encoding="utf-8")) for path in sys.argv[1:]))
