"""自制 CSV 通过正式导入器验证旧离散语义，不分发外部弹道。"""
import hashlib
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("import_recoil_profiles", Path(__file__).resolve().parents[1] / "scripts/import_recoil_profiles.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DiscreteImporterTests(unittest.TestCase):
    def test_length_first_wait_rounding_and_tail(self):
        raw = b"3,-6,10.25\n-6,3,20\n999,999,99\n"
        entry = dict(id="synthetic", file="synthetic.csv", canonical_weapon_id="ak47",
                     sha256=hashlib.sha256(raw).hexdigest(), multiple=3, legacy_length=2,
                     sleep_divider=1, sleep_suber_ms=0)
        profile = MODULE.convert_discrete(raw, entry, dict(repository="synthetic", commit="fixture"), 2.45)
        self.assertEqual(profile["schema_version"], 3)
        self.assertEqual(profile["sample_semantics"], "discrete_delta")
        self.assertEqual(profile["events"], [[10.3, 1.0, 2.0], [20.6, 1.0, 2.0],
                                            [30.900000000000002, -2.0, -1.0],
                                            [50.900000000000006, -2.0, -1.0], [70.9, -2.0, -1.0]])
        self.assertFalse(profile["verified"])

    def test_legacy_csv_bounds_and_zero_normalized_delay(self):
        for raw in (b"1,1,0.01\n", b"1,1,1001\n", b"10001,1,10\n"):
            entry = dict(id="synthetic", file="synthetic.csv", canonical_weapon_id="ak47",
                         sha256=hashlib.sha256(raw).hexdigest(), multiple=2, legacy_length=1,
                         sleep_divider=1, sleep_suber_ms=-0.1)
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                MODULE.convert_discrete(raw, entry, dict(repository="synthetic", commit="fixture"), 2.45)


if __name__ == "__main__":
    unittest.main()
