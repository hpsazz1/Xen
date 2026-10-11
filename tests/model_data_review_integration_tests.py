"""生产审核 HTML 导出字节 → Python 导入 → 冻结数据集；仅合成审核，不启动浏览器或训练。"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageDraw


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "model_data_pipeline.py"
spec = importlib.util.spec_from_file_location("review_integration_pipeline", SCRIPT)
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)
NODE = shutil.which("node")
OUTPUT_ROOT = None

# 只桥接真实 HTML 的数据和核心，决策输入是合成夹具真值；不重写导出协议。
EXPORT_SCRIPT = r'''
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const [htmlPath, choicesPath, outputPath] = process.argv.slice(1);
const html = fs.readFileSync(htmlPath, 'utf8');
const core = html.match(/<script id="review-core">([\s\S]*?)<\/script>/);
const payload = html.match(/<script type="application\/json" id="review-data">([\s\S]*?)<\/script>/);
assert.ok(core && payload, '必须读取生产生成 HTML 中的核心与内嵌数据');
const context = vm.createContext({});
vm.runInContext(core[1], context);
const C = context.XenReviewCore;
const data = C.validate(JSON.parse(payload[1]));
const choices = JSON.parse(fs.readFileSync(choicesPath, 'utf8'));
const decisions = new Map();
for (const choice of choices) {
  const sample = data.samples.find(s => s.session_id === choice.session_id && s.sample_id === choice.sample_id);
  assert.ok(sample, '决策必须对应真实报告样本');
  assert.ok(!decisions.has(C.key(sample)), '夹具不得覆盖重复决策');
  decisions.set(C.key(sample), C.decision(sample, choice.state, choice.detections, data.class_names));
}
fs.writeFileSync(outputPath, JSON.stringify(C.exportDraft(data, decisions), null, 2), 'utf8');
'''

# 64×48 合成图上的两个明确框，独立归一化真值为 0.375/0.375/0.5/0.5 和 0.75/0.625/0.25/0.25。
BOXES = [dict(class_id=0, x1=8, y1=6, x2=40, y2=30),
         dict(class_id=1, x1=40, y1=24, x2=56, y2=36)]
EXPECTED_LABEL = ("0 0.3750000000 0.3750000000 0.5000000000 0.5000000000\n"
                  "1 0.7500000000 0.6250000000 0.2500000000 0.2500000000\n")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, allow_nan=False), encoding="utf-8")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


class ReviewIntegrationTests(unittest.TestCase):
    def setUp(self):
        if OUTPUT_ROOT:
            self.base = OUTPUT_ROOT / self._testMethodName
            self.base.mkdir(parents=True, exist_ok=False)
        else:
            temporary = tempfile.TemporaryDirectory(prefix="xen-review-integration-")
            self.addCleanup(temporary.cleanup)
            self.base = Path(temporary.name).resolve()
        self.raw = self.base / "合成数据 space & literal"
        self.names = ["目标", "辅助类"]
        self.serial = 0
        for index, color in enumerate(((200, 10, 10), (10, 200, 10), (10, 10, 200))):
            session = self.raw / f"session-{index}"
            (session / "images").mkdir(parents=True)
            (session / "samples").mkdir()
            write_json(session / "session.json", dict(schema_version=1, session_id=session.name,
                                                      class_names=self.names))
            for sample_id in ("1", "2"):
                image = session / "images" / f"{sample_id}.png"
                picture = Image.new("RGB", (64, 48), color)
                if sample_id == "1":
                    draw = ImageDraw.Draw(picture)
                    draw.rectangle((8, 6, 39, 29), fill=(240, 240, 240))
                    draw.rectangle((40, 24, 55, 35), fill=(20, 20, 20))
                picture.save(image)
                # 生产预标注只是提示；测试必须导入显式修订后的独立框真值。
                hints = [dict(class_id=0, x1=3, y1=4, x2=28, y2=38)] if sample_id == "1" else []
                write_json(session / "samples" / f"{sample_id}.json", dict(schema_version=1,
                    session_id=session.name, sample_id=sample_id, image=f"images/{sample_id}.png",
                    image_sha256=digest(image), width=64, height=48, detections=hints,
                    detection_status="SUCCESS", review_state="PRELABELED"))

    def job(self, operation, expect=0, root=None, **kwargs):
        self.assertIn(operation, {"inspect", "review_curate", "import_labels", "export"})
        self.serial += 1
        job_path = self.base / f"job-{self.serial}.json"
        status_path = self.base / f"status-{self.serial}.json"
        write_json(job_path, dict(operation=operation, root=str(root or self.raw),
                                 class_names=self.names, **kwargs))
        result = subprocess.run([sys.executable, "-B", "-X", "utf8", str(SCRIPT),
            "--job", str(job_path), "--status", str(status_path)],
            capture_output=True, text=True, encoding="utf-8", timeout=30)
        (self.base / f"cli-{self.serial}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        status = read_json(status_path)
        self.assertEqual(status["state"], "SUCCEEDED" if expect == 0 else "FAILED", status)
        return status["result"] if expect == 0 else status

    def curate(self, name="report", root=None):
        return Path(self.job("review_curate", root=root,
                            output=str(self.base / name))["review_preview"])

    def choice(self, session, sample="1", state="VERIFIED_POSITIVE"):
        return dict(session_id=f"session-{session}", sample_id=sample, state=state,
                    detections=BOXES if state == "VERIFIED_POSITIVE" else [])

    def export(self, html, choices, name="decisions"):
        choice_path, exported = self.base / f"{name}-choices.json", self.base / f"{name}.json"
        write_json(choice_path, choices)
        result = subprocess.run([NODE, "-e", EXPORT_SCRIPT, str(html), str(choice_path), str(exported)],
            capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(exported.is_file())
        return exported

    def files(self, directory):
        return {str(p.relative_to(directory)): digest(p) for p in directory.rglob("*") if p.is_file()}

    def seed_revision(self):
        html = self.curate()
        exported = self.export(html, [self.choice(0)], "seed")
        self.job("import_labels", review_manifest=str(exported))
        return html

    def reject_unchanged(self, exported, message):
        before = self.files(self.raw)
        contents = exported.read_bytes()
        failed = self.job("import_labels", expect=1, review_manifest=str(exported))
        self.assertRegex(failed["message"], message)
        # 包括全部旧 revision.json、原图、样本元数据；锁也必须释放。
        self.assertEqual(self.files(self.raw), before)
        self.assertFalse((self.raw / "reviews" / ".import.lock").exists())
        self.assertEqual(exported.read_bytes(), contents)

    def refresh_and_import(self, choices):
        before = self.files(self.raw / "reviews")
        refreshed = self.export(self.curate("refreshed"), choices, "refreshed")
        result = self.job("import_labels", review_manifest=str(refreshed))
        self.assertEqual(result["samples"], len(choices))
        self.assertEqual({p: digest(self.raw / "reviews" / p) for p in before}, before)
        self.assertEqual(len(list((self.raw / "reviews").glob("*/revision.json"))), 2)
        revision = read_json(Path(result["revision"]) / "revision.json")
        self.assertEqual(revision["source_manifest_sha256"], digest(refreshed))
        return revision

    def test_html_decisions_reach_frozen_dataset_with_exact_identity_and_labels(self):
        original = self.files(self.raw)
        self.assertEqual(self.job("inspect")["states"], {"UNKNOWN": 6})
        self.job("export", expect=1, output=str(self.base / "unreviewed"))
        html = self.curate()
        choices = [self.choice(i) for i in range(3)]
        choices += [self.choice(0, "2", "VERIFIED_NEGATIVE"), self.choice(1, "2", "EXCLUDED")]
        exported = self.export(html, choices)
        exported_bytes = exported.read_bytes()
        imported = self.job("import_labels", review_manifest=str(exported))
        revision = read_json(Path(imported["revision"]) / "revision.json")
        self.assertEqual(revision["source_manifest_sha256"], digest(exported))
        self.assertEqual(imported["samples"], 5)
        self.assertEqual(self.job("inspect")["states"],
                         {"VERIFIED_POSITIVE": 3, "VERIFIED_NEGATIVE": 1, "EXCLUDED": 1, "UNKNOWN": 1})
        dataset = Path(self.job("export", output=str(self.base / "dataset"))["dataset"])
        frozen = pipeline.validate_dataset(dataset)
        self.assertEqual(frozen["class_names"], self.names)
        self.assertEqual(frozen["states"], {"VERIFIED_POSITIVE": 3, "VERIFIED_NEGATIVE": 1})
        self.assertEqual(frozen["selection"]["omitted_counts"], {"EXCLUDED": 1, "UNKNOWN": 1})
        self.assertEqual({(s["session_id"], s["sample_id"], s["state"]) for s in frozen["selection"]["omitted"]},
                         {("session-1", "2", "EXCLUDED"), ("session-2", "2", "UNKNOWN")})
        expected_keys = {("session-0", "1"), ("session-1", "1"), ("session-2", "1"), ("session-0", "2")}
        self.assertEqual({(s["session_id"], s["sample_id"]) for s in frozen["samples"]}, expected_keys)
        self.assertEqual(set(frozen["split_groups"].values()), {"train", "val", "test"})
        for sample in frozen["samples"]:
            original_image = self.raw / sample["session_id"] / "images" / f"{sample['sample_id']}.png"
            self.assertEqual(sample["image_sha256"], digest(original_image))
            self.assertEqual((dataset / sample["image"]).read_bytes(), original_image.read_bytes())
            self.assertEqual((dataset / sample["label"]).read_text(encoding="utf-8"),
                             EXPECTED_LABEL if sample["sample_id"] == "1" else "")
            self.assertEqual(sample["revision"], Path(imported["revision"]).name)
        self.assertEqual(exported.read_bytes(), exported_bytes)
        self.assertEqual({p: digest(self.raw / p) for p in original}, original)

    def test_stale_html_batch_rejects_valid_prefix_and_refresh_recovers(self):
        old_html = self.seed_revision()
        # 第一条仍有效，第二条刚被另一份导入修订；必须整批拒绝。
        choices = [self.choice(1), self.choice(0)]
        stale = self.export(old_html, choices, "stale")
        self.reject_unchanged(stale, "过期审核")
        self.assertEqual(self.job("inspect")["states"], {"VERIFIED_POSITIVE": 1, "UNKNOWN": 5})
        self.refresh_and_import(choices)
        self.assertEqual(self.job("inspect")["states"], {"VERIFIED_POSITIVE": 2, "UNKNOWN": 4})

    def test_actual_foreign_root_export_rejected_and_local_refresh_recovers(self):
        self.seed_revision()
        foreign = self.base / "另一数据根"
        shutil.copytree(self.raw, foreign)
        foreign_before = self.files(foreign)
        exported = self.export(self.curate("foreign-report", root=foreign), [self.choice(1)], "foreign")
        self.reject_unchanged(exported, "根身份不匹配")
        self.assertEqual(self.files(foreign), foreign_before)
        self.refresh_and_import([self.choice(1)])

    def test_changed_unreviewed_image_rejects_old_export_and_refresh_recovers(self):
        html = self.seed_revision()
        choices = [self.choice(2), self.choice(1)]
        stale = self.export(html, choices, "old-image")
        image = self.raw / "session-1" / "images" / "1.png"
        with Image.open(image) as opened:
            picture = opened.copy()
        picture.putpixel((0, 0), (99, 88, 77))
        picture.save(image)
        self.reject_unchanged(stale, "原图缺失或 SHA-256 不匹配")
        # 仅夹具中的未审核样本建立新图片身份；不改旧修订，不假设刷新能洗掉已审核图变更。
        record = image.parent.parent / "samples" / "1.json"
        sample = read_json(record)
        sample["image_sha256"] = digest(image)
        write_json(record, sample)
        self.reject_unchanged(stale, "审核图片哈希不匹配")
        revision = self.refresh_and_import(choices)
        updated = next(s for s in revision["samples"] if s["session_id"] == "session-1")
        self.assertEqual(updated["image_sha256"], digest(image))

    def test_corrupted_export_rejects_duplicate_or_invalid_tail_without_partial_revision(self):
        self.seed_revision()
        choices = [self.choice(1), self.choice(2)]
        exported = self.export(self.curate("current"), choices)
        actual_bytes = exported.read_bytes()
        # 下列是对真实导出字节的明确损坏，不冒充生产者能生成这些非法状态。
        cases = [("duplicate", "重复或未知样本"), ("unknown", "UNKNOWN 不能训练"),
                 ("empty-positive", "状态与标签内容矛盾"), ("class", "类别超出"),
                 ("bounds", "位于图片内"), ("schema", "schema 或类别不匹配")]
        for kind, message in cases:
            with self.subTest(kind=kind):
                damaged = json.loads(actual_bytes)
                tail = damaged["samples"][-1]
                if kind == "duplicate":
                    damaged["samples"].append(damaged["samples"][0])
                elif kind == "unknown":
                    tail["state"] = "UNKNOWN"
                elif kind == "empty-positive":
                    tail["detections"] = []
                elif kind == "class":
                    tail["detections"][0]["class_id"] = 2
                elif kind == "bounds":
                    tail["detections"][0]["x2"] = 65
                elif kind == "schema":
                    damaged["class_names"].reverse()
                path = self.base / f"damaged-{kind}.json"
                write_json(path, damaged)
                self.reject_unchanged(path, message)
        self.assertEqual(exported.read_bytes(), actual_bytes)
        self.refresh_and_import(choices)
        self.assertEqual(self.job("inspect")["states"], {"VERIFIED_POSITIVE": 3, "UNKNOWN": 3})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=NODE)
    parser.add_argument("--output-root", type=Path)
    options, remaining = parser.parse_known_args()
    if not options.node:
        parser.error("需要 Node.js；可通过 --node 指定，测试不会安装依赖或静默跳过")
    NODE = options.node
    OUTPUT_ROOT = options.output_root.resolve() if options.output_root else None
    unittest.main(argv=[sys.argv[0], *remaining])
