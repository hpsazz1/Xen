import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_lineup_calibration.py"
spec = importlib.util.spec_from_file_location("lineup_calibration_tool", SCRIPT)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

class CalibrationBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xen-calibration-tool-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "measurement.json"
        self.data = {"schema": 1, "id": "TEST-ONLY", "source_id": "fixture", "backend": "fake", "context_identity": "test-only",
            "geometry": {"width":320,"height":320,"source_width":1920,"source_height":1080,"encoded_width":320,"encoded_height":320,"roi_x":800,"roi_y":380,"scale_x":1,"scale_y":1,"mapping_verified":True},
            "observation_delay_ms":20,"samples":[]}
        for index in range(8):
            before,after=f"before{index}.png",f"after{index}.png"
            (self.root / before).write_bytes(b"test-only-before-"+bytes([index]))
            (self.root / after).write_bytes(b"test-only-after-"+bytes([index]))
            counts=(10,20,-10,-20)[index%4]
            self.data["samples"].append({"axis":"x" if index<4 else "y","counts":counts,"delta_error_pixels":-counts*2,"cross_error_pixels":0,"observed_delay_ms":10,"before_image":before,"after_image":after})
        self.save()
    def save(self):
        self.manifest.write_text(json.dumps(self.data),encoding="utf-8")
    def test_bundle_preserves_originals_and_hashes(self):
        original={p:p.read_bytes() for p in self.root.iterdir() if p.is_file()}
        result=tool.build_bundle(self.manifest,self.root/"bundle")
        built=json.loads(Path(result["bundle"]).read_text(encoding="utf-8"))
        self.assertEqual(built["origin"],"simulation");self.assertIs(built["reviewed"],False)
        self.assertFalse(result["production_validated"])
        for sample in built["samples"]:
            for prefix in ("before","after"):
                image=self.root/"bundle"/sample[prefix+"_image"]
                self.assertEqual(hashlib.sha256(image.read_bytes()).hexdigest(),sample[prefix+"_sha256"])
        for path,content in original.items(): self.assertEqual(path.read_bytes(),content)
    def test_simulation_never_upgraded_even_reviewed(self):
        self.data.update(origin="simulation",reviewed=True);self.save()
        result=tool.build_bundle(self.manifest,self.root/"bundle")
        built=json.loads(Path(result["bundle"]).read_text())
        self.assertEqual(built["origin"],"simulation");self.assertTrue(built["reviewed"]);self.assertFalse(result["production_validated"])
    def test_explicit_measured_decision_preserved_not_certified(self):
        self.data.update(origin="measured",reviewed=True);self.save()
        result=tool.build_bundle(self.manifest,self.root/"bundle")
        self.assertEqual(result["origin"],"measured");self.assertTrue(result["reviewed"]);self.assertFalse(result["production_validated"])
    def test_hash_mismatch_creates_no_output(self):
        self.data["samples"][0]["before_sha256"]="0"*64;self.save()
        with self.assertRaisesRegex(ValueError,"SHA-256"):tool.build_bundle(self.manifest,self.root/"bundle")
        self.assertFalse((self.root/"bundle").exists())
    def test_reject_existing_output_even_empty(self):
        (self.root/"bundle").mkdir()
        with self.assertRaises(FileExistsError):tool.build_bundle(self.manifest,self.root/"bundle")
        self.assertEqual(list((self.root/"bundle").iterdir()),[])
    def test_check_only_no_output_and_cli(self):
        before=set(self.root.iterdir());result=tool.build_bundle(self.manifest,self.root/"unused",True)
        self.assertEqual(result["evidence_sha256_checked"],16);self.assertEqual(before,set(self.root.iterdir()))
        completed=subprocess.run([sys.executable,str(SCRIPT),str(self.manifest),"--check-only"],capture_output=True,text=True)
        self.assertEqual(completed.returncode,0,completed.stderr);self.assertFalse(json.loads(completed.stdout)["production_validated"])
    def test_no_origin_or_reviewed_cli_override(self):
        completed=subprocess.run([sys.executable,str(SCRIPT),str(self.manifest),"--check-only","--reviewed"],capture_output=True,text=True)
        self.assertNotEqual(completed.returncode,0)
    def test_missing_evidence_does_not_change_inputs(self):
        self.data["samples"][0]["after_image"]="not-found.png";self.save()
        with self.assertRaises(FileNotFoundError):tool.build_bundle(self.manifest,self.root/"bundle")
        self.assertFalse((self.root/"bundle").exists())
    def test_no_guessed_delay_or_nonboolean_review(self):
        self.data["observation_delay_ms"]=None;self.save()
        with self.assertRaises(ValueError):tool.build_bundle(self.manifest,check_only=True)
        self.data["observation_delay_ms"]=20;self.data["reviewed"]="true";self.save()
        with self.assertRaises(ValueError):tool.build_bundle(self.manifest,check_only=True)

if __name__ == "__main__": unittest.main()
