"""批次导入回归：仅使用TemporaryDirectory内的合成文件，不访问设备。"""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("legacy_dataset_fixtures", Path(__file__).with_name("recoil_dataset_tests.py"))
LEGACY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEGACY)
MODULE = LEGACY.MODULE


class BatchDatasetTests(unittest.TestCase):
    def setUp(self):
        self.fixture = LEGACY.DatasetTests("test_complete_conversion_and_no_fabricated_h")
        self.fixture.setUp()
        self.directory = self.fixture.directory
        self.manifest = self.fixture.manifest
        self.path = self.fixture.path
        for path in self.directory.glob("synthetic-run-*.json"):
            root = json.loads(path.read_text(encoding="utf-8"))
            self.to_batch(root)
            path.write_text(json.dumps(root), encoding="utf-8")

    def tearDown(self):
        self.fixture.tearDown()

    @staticmethod
    def to_batch(root, begin=3000):
        root["acquisition_run_id"] = root["session_id"]
        recoil = root["recoil"]
        recoil["schema"] = 2
        execution = recoil["execution"]
        first = execution["records"][0]
        for index, record in enumerate(execution["records"]):
            record["event_sequence"] = begin + index + 1
        execution["batch"] = {
            "firing_id": first["firing_id"], "begin_sequence": begin,
            "end_sequence": begin + len(execution["records"]) + 1,
            "coverage_complete": True, "end_reason": "EXHAUSTED",
            "begin_at_steady_ns": first["firing_started_at_steady_ns"],
            "end_at_steady_ns": execution["records"][-1]["completed_at_steady_ns"],
            "records_count": len(execution["records"]),
            "firing_started_at_steady_ns": first["firing_started_at_steady_ns"],
            "profile": first["profile"], "weapon_generation": first["weapon_generation"],
            "device_epoch": first["device_epoch"], "firing_source": first["firing_source"]}
        return root

    def root(self):
        return json.loads((self.directory / self.manifest["trials"][0]["debug_report"]).read_text(encoding="utf-8"))

    def save_root(self, root):
        self.fixture.save(self.manifest["trials"][0]["debug_report"], root)

    def rejected(self, root):
        self.save_root(root)
        with self.assertRaises((ValueError, KeyError)):
            MODULE.build_dataset(self.path)

    def test_new_complete_batch_survives_old_global_coverage(self):
        root = self.root()
        root["recoil"]["execution"]["dropped_count"] = 9000
        self.save_root(root)
        result = MODULE.build_dataset(self.path)
        self.assertEqual(result["trials"][0]["executed_counts"], [2, 4])
        self.assertEqual(result["trials"][0]["source_run"], root["acquisition_run_id"])
        self.assertIn("controller_exhausted", result["trials"][0]["completion_evidence"])
        self.assertIsNone(result["provenance"]["physical_acceptance"])

    def test_schema1_still_rejects_global_gap(self):
        root = self.root()
        root["recoil"]["schema"] = 1
        root["recoil"]["execution"]["dropped_count"] = 1
        self.rejected(root)

    def test_missing_boundary_fields(self):
        original = self.root()
        for key in ("begin_sequence", "end_sequence", "coverage_complete", "end_reason", "records_count",
                    "begin_at_steady_ns", "end_at_steady_ns", "firing_started_at_steady_ns", "firing_source"):
            with self.subTest(key=key):
                root = copy.deepcopy(original)
                del root["recoil"]["execution"]["batch"][key]
                self.rejected(root)

    def test_gap_repeat_reorder_and_wrong_count(self):
        original = self.root()
        changes = [lambda e: e["batch"].update(coverage_complete=False),
                   lambda e: e["batch"].update(records_count=3),
                   lambda e: e["batch"].update(end_sequence=3009),
                   lambda e: e["records"][1].update(event_sequence=3001),
                   lambda e: e["records"].reverse(),
                   lambda e: e["records"].pop()]
        for change in changes:
            with self.subTest(change=change):
                root = copy.deepcopy(original)
                change(root["recoil"]["execution"])
                self.rejected(root)

    def test_non_natural_completion_not_optimization_data(self):
        original = self.root()
        for reason in ("CANCELED", "RELEASED", "UNKNOWN", "STOPPED", "EXCEPTION", "exhausted"):
            with self.subTest(reason=reason):
                root = copy.deepcopy(original)
                root["recoil"]["execution"]["batch"]["end_reason"] = reason
                self.rejected(root)

    def test_complete_flag_cannot_override_missing_ranges_or_reason(self):
        original = self.root()
        for field, value in (("missing_sequence_ranges", [[1, 2]]),
                             ("incomplete_reason", "EVENT_GAP"),
                             ("missing_sequence_ranges", None), ("incomplete_reason", None)):
            with self.subTest(field=field, value=value):
                root = copy.deepcopy(original)
                root["recoil"]["execution"]["batch"][field] = value
                self.rejected(root)

    def test_boundary_identity_and_source_must_match_every_record(self):
        original = self.root()
        for key, value in (("firing_id", 2), ("profile", "different:1"), ("weapon_generation", 3),
                           ("device_epoch", 2), ("firing_source", "COMMAND_ESTIMATED"),
                           ("firing_started_at_steady_ns", 999999999)):
            with self.subTest(key=key):
                root = copy.deepcopy(original)
                root["recoil"]["execution"]["batch"][key] = value
                self.rejected(root)
        root = copy.deepcopy(original)
        root["recoil"]["execution"]["records"][1]["device_epoch"] = True
        self.rejected(root)

    def test_end_after_measurement_or_before_curve_end_rejected(self):
        original = self.root()
        start = original["recoil"]["execution"]["batch"]["firing_started_at_steady_ns"]
        for duration in (1_000_000, 26_000_000):
            root = copy.deepcopy(original)
            root["recoil"]["execution"]["batch"]["end_at_steady_ns"] = start + duration
            self.rejected(root)

    def test_same_run_different_files_not_independent_holdout(self):
        trial = self.manifest["trials"][4]
        root = json.loads((self.directory / trial["debug_report"]).read_text(encoding="utf-8"))
        root["session_id"] = root["acquisition_run_id"] = self.manifest["trials"][0]["source_run"]
        trial["source_run"] = root["session_id"]
        self.fixture.save(trial["debug_report"], root)
        self.fixture.save_manifest()
        with self.assertRaises(ValueError): MODULE.build_dataset(self.path)

    def test_model_segment_cannot_replace_acquisition_id(self):
        root = self.root()
        root["session_id"] += "-g2-s3"
        self.manifest["trials"][0]["source_run"] = root["session_id"]
        self.fixture.save_manifest()
        self.rejected(root)

    def test_calibration_phase_budget_is_not_profile_calibration(self):
        original = self.root()
        for keep_phase in (False, True):
            root = copy.deepcopy(original)
            profile = root["recoil"]["execution"]["profiles"][0]
            profile["state"] = "SCHEMA_VALID"
            if not keep_phase: profile.pop("phase_tolerance_ms")
            root["recoil"]["execution"]["batch"]["command_phase_budget_ms"] = 20
            self.rejected(root)
        root = copy.deepcopy(original)
        root["recoil"]["execution"]["profiles"][0].pop("phase_tolerance_ms")
        root["recoil"]["execution"]["batch"]["command_phase_budget_ms"] = 20
        self.rejected(root)

    def test_zero_command_batch_archivable_but_not_optimizer_receipts(self):
        root = self.root()
        e = root["recoil"]["execution"]
        e["records"] = []
        e["batch"].update(records_count=0, end_sequence=e["batch"]["begin_sequence"] + 1)
        self.rejected(root)

    def test_unknown_receipt_cannot_be_hidden_by_complete_batch(self):
        root = self.root()
        root["recoil"]["execution"]["records"][0]["receipt"] = "UNKNOWN"
        self.rejected(root)

    def test_more_than_old_ring_capacity_is_accepted(self):
        root = self.root()
        e = root["recoil"]["execution"]
        sample = e["records"][0]
        start = sample["firing_started_at_steady_ns"]
        count = 2200
        profile = e["profiles"][0]
        profile["points"] = [[i, i, 2 * i] for i in range(count + 1)]
        e["records"] = []
        for i in range(1, count + 1):
            record = dict(sample, command_id=i, planned_at_steady_ns=start + i * 1_000_000,
                          completed_at_steady_ns=start + (i + 1) * 1_000_000,
                          expires_at_steady_ns=start + (i + 20) * 1_000_000,
                          requested_counts=[1, 2])
            e["records"].append(record)
        self.to_batch(root)
        self.save_root(root)
        entry = copy.deepcopy(self.manifest["trials"][0])
        entry["measurement"]["at_steady_ns"] = start + (count + 2) * 1_000_000
        trial, _ = MODULE.Builder(self.directory).trial(entry, "fit")
        self.assertEqual(len(trial["receipts"]), count)
        self.assertEqual(trial["executed_counts"], [count, 2 * count])

    def test_new_schema_keeps_strict_resource_and_integer_bounds(self):
        original = self.root()
        for value in (True, 0, 32769):
            root = copy.deepcopy(original)
            root["recoil"]["execution"]["batch"]["records_count"] = value
            self.rejected(root)


if __name__ == "__main__":
    unittest.main()
