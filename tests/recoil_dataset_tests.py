"""Synthetic offline-only fixtures; every file lives in TemporaryDirectory."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("build_recoil_dataset", Path(__file__).resolve().parents[1] / "scripts/build_recoil_dataset.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DatasetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.profile = {"schema_version": 1, "id": "synthetic", "revision": 1, "weapon_id": "ak47",
                        "unit": "device_counts", "sample_semantics": "cumulative", "fire_mode": "automatic",
                        "state": "CALIBRATED", "environment_fingerprint": '{"synthetic":true}',
                        "phase_tolerance_ms": 20, "points": [[0, 0, 0], [10, 1, 2], [20, 2, 4]]}
        self.index = 0
        trials = [self.make_run("fit" if i < 3 else "holdout") for i in range(5)]
        responses = []
        for i, delta in enumerate(([2, 0], [0, 2], [2, 2])):
            before = self.make_run()
            changed = copy.deepcopy(self.profile)
            changed["revision"] = 2
            for p in changed["points"]:
                t = p[0] / 20
                for axis in range(2):
                    p[axis + 1] += delta[axis] * t * t * (3 - 2 * t)
            after = self.make_run(profile=changed, residual=[8 - delta[0], 6 - delta[1]])
            responses.append({"id": "response" + str(i), "baseline": before, "changed": after})
        self.manifest = {"schema_version": 1, "base": {"debug_report": trials[0]["debug_report"], "profile_key": "synthetic:1"},
                         "trials": trials, "response_experiments": responses}
        self.path = self.directory / "manifest.json"
        self.save_manifest()

    def tearDown(self):
        self.temp.cleanup()

    def save(self, name, value):
        (self.directory / name).write_text(json.dumps(value), encoding="utf-8")

    def save_manifest(self):
        self.save("manifest.json", self.manifest)

    def make_run(self, use=None, profile=None, residual=None):
        self.index += 1
        name = "synthetic-run-" + str(self.index)
        profile = copy.deepcopy(profile or self.profile)
        start = self.index * 1000000000
        records = []
        last = [0, 0]
        for i, p in enumerate(profile["points"][1:]):
            planned = start + int(p[0] * 1000000)
            counts = [int(p[j + 1] - last[j]) for j in range(2)]
            records.append({"command_id": i + 1, "firing_id": 1, "profile": profile["id"] + ":" + str(profile["revision"]),
                            "weapon_generation": 2, "device_epoch": 1, "firing_started_at_steady_ns": start,
                            "firing_source": "INPUT_ESTIMATED", "planned_at_steady_ns": planned,
                            "completed_at_steady_ns": planned + 1000000, "expires_at_steady_ns": planned + 20000000,
                            "requested_counts": counts, "backend_called": True, "receipt": "ACKNOWLEDGED"})
            last = p[1:]
        debug = {"session_id": name, "recoil": {"schema": 1, "config": {"enabled": True, "mixed_aim": False},
                 "execution": {"clock_domain": "local_steady", "dropped_count": 0, "records": records, "profiles": [profile]}}}
        self.save(name + ".json", debug)
        (self.directory / (name + "-measurement.txt")).write_text("SYNTHETIC TEST ONLY", encoding="utf-8")
        entry = {"id": name, "debug_report": name + ".json", "source_run": name, "firing_id": 1,
                 "completed": True, "independent_recoil": True, "timing_valid": True, "previously_used_generation": 0,
                 "measurement": {"source": name + "-measurement.txt", "clock_domain": "local_steady",
                                  "at_steady_ns": start + 25000000, "residual": residual or [8, 6], "noise": [0.1, 0.2],
                                  "timing_uncertainty_ms": 1, "reference_confirmed": True}}
        if use is not None:
            entry["use"] = use
        return entry

    def mutate_debug(self, change):
        name = self.manifest["trials"][0]["debug_report"]
        debug = json.loads((self.directory / name).read_text())
        change(debug["recoil"]["execution"])
        self.save(name, debug)

    def test_complete_conversion_and_no_fabricated_h(self):
        result = MODULE.build_dataset(self.path)
        self.assertEqual(result["trials"][0]["executed_counts"], [2, 4])
        self.assertEqual(result["trials"][0]["receipts"][0]["planned_ms"], 10)
        self.assertEqual(result["trials"][0]["receipts"][0]["completed_ms"], 11)
        self.assertEqual(result["response_experiments"][0]["delta_counts"], [2, 0])
        self.assertEqual(result["response_experiments"][0]["delta_residual"], [-2, 0])
        self.assertNotIn("response_matrix", result)
        self.assertIsNone(result["provenance"]["physical_acceptance"])

    def test_no_overwrite(self):
        output = self.directory / "dataset.json"
        MODULE.write_dataset(self.path, output)
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            MODULE.write_dataset(self.path, output)
        self.assertEqual(before, output.read_bytes())

    def test_dropped_records_rejected(self):
        self.mutate_debug(lambda e: e.update(dropped_count=1))
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_unknown_and_not_sent_rejected(self):
        for status in ("UNKNOWN", "NOT_SENT"):
            self.mutate_debug(lambda e: e["records"][0].update(receipt=status))
            with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_select_one_explicit_firing_not_all_counts(self):
        self.manifest["trials"][0]["firing_id"] = 2
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_same_total_wrong_timing_rejected(self):
        self.mutate_debug(lambda e: (e["records"][0].update(requested_counts=[2, 4]),
                                    e["records"][1].update(requested_counts=[0, 0])))
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_measurement_or_completion_not_assumed(self):
        self.manifest["trials"][0]["completed"] = False
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)
        self.manifest["trials"][0]["completed"] = True
        self.manifest["trials"][0]["measurement"].pop("noise")
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_reused_or_leaked_run_rejected(self):
        self.manifest["trials"][4] = copy.deepcopy(self.manifest["trials"][0])
        self.manifest["trials"][4]["use"] = "holdout"
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_fake_counts_in_manifest_rejected(self):
        self.manifest["trials"][0]["executed_counts"] = [2, 4]
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_missing_debug_clock_and_fingerprint_rejected(self):
        self.mutate_debug(lambda e: e["records"][0].pop("firing_started_at_steady_ns"))
        with self.assertRaises(KeyError): MODULE.build_dataset(self.path)

    def test_old_debug_without_environment_fingerprint_rejected(self):
        self.mutate_debug(lambda e: e["profiles"][0].pop("environment_fingerprint"))
        with self.assertRaises(KeyError): MODULE.build_dataset(self.path)

    def test_incomplete_input_never_creates_output(self):
        self.manifest["trials"][0]["measurement"]["noise"] = [0, 0]
        self.save_manifest()
        output = self.directory / "must-not-exist.json"
        with self.assertRaises(ValueError): MODULE.write_dataset(self.path, output)
        self.assertFalse(output.exists())

    def test_missing_middle_command_rejected(self):
        self.mutate_debug(lambda e: e["records"][1].update(command_id=3))
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_boolean_schema_is_not_integer(self):
        self.manifest["schema_version"] = True
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_consumed_holdout_rejected(self):
        self.manifest["trials"][4]["previously_used_generation"] = 1
        self.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_response_basis_not_guessed(self):
        entry = self.manifest["response_experiments"][0]["changed"]
        name = entry["debug_report"]
        debug = json.loads((self.directory / name).read_text())
        debug["recoil"]["execution"]["profiles"][0]["points"][1][1] += 0.25
        self.save(name, debug)
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)


if __name__ == "__main__":
    unittest.main()
