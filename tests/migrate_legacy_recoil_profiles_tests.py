"""迁移契约回归；17份曲线均为自制数据，不包含第三方CSV。"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import migrate_legacy_recoil_profiles as migration
from import_recoil_profiles import convert

STORE_TEST = None
if "--store-test" in sys.argv:
    position = sys.argv.index("--store-test")
    STORE_TEST = Path(sys.argv[position + 1]).resolve()
    del sys.argv[position:position + 2]


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "csv"
        self.target = self.root / "profiles"
        self.plan = self.root / "plan"
        self.data = self.root / "data"
        self.source.mkdir(); self.target.mkdir()
        (self.data / "assets/recoil").mkdir(parents=True)
        entries, profiles, catalog = [], [], []
        raw = b"0,0,3\n-2.3,-4.7,10\n1.1,2.2,12\n"
        active = {"schema_version": 1, "extra": "preserve", "active": {}}
        real_catalog = (Path(__file__).resolve().parents[1] / "assets/weapon_catalog.inc").read_text(encoding="utf-8")
        weapons = re.findall(r'XEN_WEAPON\("([^"]+)", "([^"]+)", "([^"]+)"\)', real_catalog)[:17]
        self.gsi_names = {name: gsi for name, gsi, _ in weapons}
        for i, (name, gsi, display) in enumerate(weapons):
            entry = {"id": name, "canonical_weapon_id": name, "file": name + ".csv",
                     "sha256": migration.digest(raw), "legacy_length": 3, "multiple": 3,
                     "sleep_divider": 3, "sleep_suber_ms": -0.1}
            entries.append(entry)
            profiles.append({"name": name, "pattern_file": "patterns/" + entry["file"],
                             "length": 3, "multiple": 3, "sleep_divider": 3, "sleep_suber": -0.1})
            catalog.append(f'XEN_WEAPON("{name}", "{gsi}", "{display}")')
            (self.source / entry["file"]).write_bytes(raw)
        manifest = {"profiles": entries, "repository": "self-authored-test", "commit": "fixture"}
        self.source_profiles = self.root / "source-profiles.json"
        self.source_profiles.write_bytes(migration.encoded({"weapons": profiles}))
        self.source_hash = migration.digest(self.source_profiles.read_bytes())
        (self.data / "assets/recoil/legacy_manifest.json").write_bytes(migration.encoded(manifest))
        (self.data / "assets/weapon_catalog.inc").write_text("\n".join(catalog), encoding="utf-8")
        for entry in entries:
            current = {"schema_version": 2, "id": "weapon_" + entry["id"], "weapon_id": entry["id"],
                       "revision": 7, "verified": True, "sensitivity": 1.4,
                       "points": convert(raw, entry, manifest, 1.4)["points"]}
            current["confirmation_sha256"] = migration.confirmation_sha256(current, self.gsi_names[entry["id"]])
            name = current["id"] + "-r7.json"
            (self.target / name).write_bytes(migration.encoded(current))
            active["active"][self.gsi_names[entry["id"]]] = {"file": name, "previous": "history.json"}
        active["active"]["other"] = {"file": "other.json", "previous": ""}
        (self.target / "active.json").write_bytes(migration.encoded(active))
        (self.target / "candidate.json").write_bytes(b"candidate unchanged")
        (self.target / "history.json").write_bytes(b"history unchanged")

    def tearDown(self):
        self.temp.cleanup()

    def prepare(self):
        return migration.prepare(self.source, self.source_profiles, self.target, self.plan, self.data, self.source_hash)

    def test_prepare_and_apply_preserve_versions_candidates_and_backup(self):
        before = {p.name: p.read_bytes() for p in self.target.iterdir()}
        plan = self.prepare()
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.target.iterdir()})
        self.assertEqual(len(plan["changes"]), 17)
        migration.apply(self.plan, self.target)
        after = json.loads((self.target / "active.json").read_bytes())
        self.assertEqual(after["extra"], "preserve")
        self.assertEqual(after["active"]["other"]["file"], "other.json")
        for name, raw in before.items():
            if name != "active.json":
                self.assertEqual((self.target / name).read_bytes(), raw)
        self.assertEqual((self.target / "migration-backups" / plan["migration_id"] / "active-before.json").read_bytes(), before["active.json"])
        new = json.loads((self.target / after["active"]["weapon_ak47"]["file"]).read_bytes())
        self.assertEqual(new["revision"], 8)
        self.assertEqual(new["sensitivity"], 1.4)
        self.assertTrue(new["verified"])
        self.assertEqual(new["sample_semantics"], "discrete_delta")
        self.assertEqual(after["active"]["weapon_ak47"]["previous"], "weapon_ak47-r7.json")
        self.assertEqual(new["confirmation_sha256"], migration.confirmation_sha256(new, "weapon_ak47"))
        if STORE_TEST:
            # 同一迁移输入/输出经真实 C++ 解析器验证，覆盖双精度时刻、非整数节点和 GSI 别名。
            for change in plan["changes"]:
                for name in (change["old_file"], change["new_file"]):
                    result = subprocess.run([str(STORE_TEST), "--validate-confirmed-file", str(self.target / name)],
                                            capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))

    def test_source_hash_mismatch_rejects_without_output(self):
        self.source_profiles.write_bytes(b"{}")
        with self.assertRaises(ValueError): self.prepare()
        self.assertFalse(self.plan.exists())

    def test_unconfirmed_curve_rejects(self):
        path = self.target / "weapon_ak47-r7.json"
        value = json.loads(path.read_bytes()); value["verified"] = False
        path.write_bytes(migration.encoded(value))
        with self.assertRaises(ValueError): self.prepare()
        self.assertFalse(self.plan.exists())

    def test_missing_confirmation_binding_rejects(self):
        path = self.target / "weapon_ak47-r7.json"
        value = json.loads(path.read_bytes())
        value.pop("confirmation_sha256", None)
        path.write_bytes(migration.encoded(value))
        with self.assertRaisesRegex(ValueError, "确认"):
            self.prepare()
        self.assertFalse(self.plan.exists())

    def test_edited_curve_rejects(self):
        path = self.target / "weapon_ak47-r7.json"
        value = json.loads(path.read_bytes()); value["points"][2][1] += 0.01
        path.write_bytes(migration.encoded(value))
        with self.assertRaises(ValueError): self.prepare()

    def test_changed_sensitivity_cannot_reuse_confirmation(self):
        path = self.target / "weapon_ak47-r7.json"
        value = json.loads(path.read_bytes()); value["sensitivity"] = 2.0
        path.write_bytes(migration.encoded(value))
        with self.assertRaisesRegex(ValueError, "确认"):
            self.prepare()
        self.assertFalse(self.plan.exists())

    def test_stale_plan_does_not_switch(self):
        self.prepare()
        path = self.target / "active.json"
        value = json.loads(path.read_bytes()); value["extra"] = "user changed"
        changed = migration.encoded(value); path.write_bytes(changed)
        with self.assertRaises(ValueError): migration.apply(self.plan, self.target)
        self.assertEqual(path.read_bytes(), changed)

    def test_modified_stage_rejects(self):
        plan = self.prepare()
        (self.plan / "profiles" / plan["changes"][0]["new_file"]).write_bytes(b"{}")
        with self.assertRaises(ValueError): migration.apply(self.plan, self.target)

    def test_exclusive_index_lock_rejects_contention(self):
        self.prepare()
        with migration.index_lock(self.target):
            with self.assertRaises(OSError): migration.apply(self.plan, self.target)

    def test_existing_new_version_rejects(self):
        (self.target / "weapon_ak47-r8-discrete.json").write_bytes(b"existing")
        with self.assertRaises(FileExistsError): self.prepare()


if __name__ == "__main__":
    unittest.main()
