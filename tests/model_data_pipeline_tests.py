"""模型数据 CLI 契约专项；合成 PNG 与训练器替身，不代表真实训练通过。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from PIL import Image
import numpy as np

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "model_data_pipeline.py"
spec = importlib.util.spec_from_file_location("model_data_pipeline", SCRIPT)
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / "raw"
        self.names = ["person"]
        for index, color in enumerate(((200, 10, 10), (10, 200, 10), (10, 10, 200))):
            sid = f"session-{index}"
            session = self.root / sid
            (session / "images").mkdir(parents=True)
            (session / "samples").mkdir()
            pipeline.write_json(session / "session.json", dict(schema_version=1, session_id=sid, class_names=self.names))
            for sample_id in ("1", "2"):
                image = session / "images" / f"{sample_id}.png"
                picture = Image.new("RGB", (64, 48), color)
                if sample_id == "2":
                    picture.putpixel((0, 0), (255, 255, 255))
                picture.save(image)
                boxes = [dict(class_id=0, x1=3, y1=4, x2=28, y2=38, confidence=0.8)] if sample_id == "1" else []
                pipeline.write_json(session / "samples" / f"{sample_id}.json", dict(schema_version=1, session_id=sid, sample_id=sample_id, image=f"images/{sample_id}.png", image_sha256=pipeline.sha256(image), width=64, height=48, detections=boxes, detection_status="SUCCESS", review_state="PRELABELED"))

    def tearDown(self):
        self.temp.cleanup()

    def job(self, operation, **kwargs):
        return dict(operation=operation, root=str(self.root), class_names=self.names, **kwargs)

    def trust(self, path):
        return dict(trusted_weights=True, trusted_weights_path=str(Path(path).resolve()), expected_weights_sha256=pipeline.sha256(path))

    def context(self, operation, **kwargs):
        return pipeline.Context(self.job(operation, **kwargs))

    def approve(self):
        output = self.base / "review"
        pipeline.review_export(self.context("review_export", output=str(output)))
        manifest = pipeline.read_json(output / "review.json")
        for item in manifest["samples"]:
            item["state"] = "VERIFIED_POSITIVE" if item["sample_id"] == "1" else "VERIFIED_NEGATIVE"
        pipeline.write_json(output / "review.json", manifest)
        pipeline.import_labels(self.context("import_labels", review_manifest=str(output / "review.json")))
        return output

    def freeze(self):
        self.approve()
        output = self.base / "dataset"
        pipeline.export_dataset(self.context("export", output=str(output)))
        return output

    def test_export_rejects_image_replaced_after_review_precheck(self):
        self.approve()
        output = self.base / "replaced-export"
        original_copy = pipeline.shutil.copyfile
        replaced = []
        def replace_then_copy(source, destination):
            if not replaced:
                Image.new("RGB", (64, 48), (99, 88, 77)).save(source)
                replaced.append(Path(source))
            return original_copy(source, destination)
        with patch.object(pipeline.shutil, "copyfile", side_effect=replace_then_copy):
            with self.assertRaises(pipeline.PipelineError):
                pipeline.export_dataset(self.context("export", output=str(output)))
        self.assertEqual(len(replaced), 1)
        self.assertFalse((output / "dataset_identity.json").exists())
        self.assertFalse((self.root / "split_registry.json").exists())

    def test_complete_cli_export_and_explicit_empty_labels(self):
        dataset = self.freeze()
        manifest = pipeline.validate_dataset(dataset)
        self.assertEqual(manifest["counts"], dict(train=2, val=2, test=2))
        self.assertEqual(manifest["states"]["VERIFIED_NEGATIVE"], 3)
        for sample in manifest["samples"]:
            if sample["state"] == "VERIFIED_NEGATIVE":
                self.assertEqual((dataset / sample["label"]).read_text(), "")
        jobfile, status = self.base / "job.json", self.base / "status.json"
        pipeline.write_json(jobfile, self.job("inspect"))
        result = subprocess.run([sys.executable, str(SCRIPT), "--job", str(jobfile), "--status", str(status)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(pipeline.read_json(status)["result"]["samples"], 6)

    def test_export_lists_omitted_unknown_and_excluded_without_training_them(self):
        review = self.approve()
        manifest = pipeline.read_json(review / "review.json")
        manifest["samples"] = [item for item in manifest["samples"]
                               if item["session_id"] == "session-0" and item["sample_id"] == "2"]
        manifest["samples"][0]["state"] = "EXCLUDED"
        pipeline.write_json(review / "review.json", manifest)
        pipeline.import_labels(self.context("import_labels", review_manifest=str(review / "review.json")))
        record = self.root / "session-1" / "samples" / "2.json"
        unknown = pipeline.read_json(record)
        unknown["sample_id"] = "draft-only"
        pipeline.write_json(record.with_name("draft-only.json"), unknown)
        dataset = self.base / "selected"
        pipeline.export_dataset(self.context("export", output=str(dataset)))
        frozen = pipeline.validate_dataset(dataset)
        self.assertEqual(frozen["selection"]["omitted_counts"], {"EXCLUDED": 1, "UNKNOWN": 1})
        self.assertEqual(frozen["selection"]["source_samples"], 7)
        self.assertEqual(len(frozen["samples"]), 5)
        self.assertFalse(any(item["sample_id"] == "draft-only" for item in frozen["samples"]))
        self.assertFalse(any(item["session_id"] == "session-0" and item["sample_id"] == "2"
                             for item in frozen["samples"]))

    def test_training_validation_rejects_missing_review_and_split_leakage(self):
        dataset = self.freeze()
        original = pipeline.read_json(dataset / "dataset.json")
        for mutation in ("revision", "session"):
            manifest = json.loads(json.dumps(original))
            if mutation == "revision":
                manifest["samples"][0].pop("revision")
            else:
                first = manifest["samples"][0]
                other = next(item for item in manifest["samples"] if item["split"] != first["split"])
                other["session_id"] = first["session_id"]
                other["sample_id"] = "different"
            pipeline.write_json(dataset / "dataset.json", manifest)
            pipeline.write_json(dataset / "dataset_identity.json",
                                dict(dataset_sha256=pipeline.sha256(dataset / "dataset.json")))
            with self.subTest(mutation=mutation), self.assertRaises(pipeline.PipelineError):
                pipeline.validate_dataset(dataset)

    def test_yolo_border_boxes_survive_production_round_trip(self):
        # 实际冻结失败样本17：x2=44.78125，独立十位小数舍入使左边界约为-5e-11。
        side = 320
        edge = 44.78125
        boxes = [dict(class_id=0, x1=0, y1=20, x2=edge, y2=80),
                 dict(class_id=0, x1=side-edge, y1=20, x2=side, y2=80),
                 dict(class_id=0, x1=20, y1=0, x2=80, y2=edge),
                 dict(class_id=0, x1=20, y1=side-edge, x2=80, y2=side)]
        label = self.base / "edges.txt"
        label.write_text(pipeline.yolo_text(boxes, side, side), encoding="utf-8")
        actual = pipeline.read_labels(label, side, side, self.names)
        self.assertEqual(len(actual), 4)
        for before, after in zip(boxes, actual):
            for key in ("x1", "y1", "x2", "y2"):
                self.assertAlmostEqual(before[key], after[key], delta=side*1e-10)

    def test_yolo_outside_rounding_budget_is_rejected_on_each_edge(self):
        label = self.base / "outside.txt"
        for cx, cy in ((0.0999999998, 0.5), (0.9000000002, 0.5),
                       (0.5, 0.0999999998), (0.5, 0.9000000002),
                       (-0.1, 0.5), (1.1, 0.5)):
            with self.subTest(cx=cx, cy=cy):
                label.write_text(f"0 {cx:.10f} {cy:.10f} 0.2 0.2\n", encoding="utf-8")
                with self.assertRaisesRegex(pipeline.PipelineError, "归一化坐标越界"):
                    pipeline.read_labels(label, 320, 320, self.names)

    def test_frozen_edge_annotations_validate_after_export(self):
        sample_path = self.root / "session-0" / "samples" / "1.json"
        sample = pipeline.read_json(sample_path)
        sample["detections"] = [dict(class_id=0, x1=0, y1=4, x2=8.95625, y2=38)]
        pipeline.write_json(sample_path, sample)
        dataset = self.freeze()
        self.assertEqual(pipeline.validate_dataset(dataset)["counts"], dict(train=2, val=2, test=2))

    def test_unknown_cannot_be_negative(self):
        with self.assertRaisesRegex(pipeline.PipelineError, "至少需要"):
            pipeline.export_dataset(self.context("export", output=str(self.base / "dataset")))
        output = self.base / "review"
        pipeline.review_export(self.context("review_export", output=str(output)))
        manifest = pipeline.read_json(output / "review.json")
        manifest["reviewer"] = "人工"
        pipeline.write_json(output / "review.json", manifest)
        with self.assertRaisesRegex(pipeline.PipelineError, "UNKNOWN"):
            pipeline.import_labels(self.context("import_labels", review_manifest=str(output / "review.json")))

    def test_missing_negative_label_rejected(self):
        review = self.approve()
        manifest = pipeline.read_json(review / "review.json")
        negative = next(i for i in manifest["samples"] if i["state"] == "VERIFIED_NEGATIVE")
        (review / negative["label"]).unlink()
        with self.assertRaisesRegex(pipeline.PipelineError, "缺少显式标签"):
            pipeline.import_labels(self.context("import_labels", review_manifest=str(review / "review.json")))

    def test_hash_dimension_class_and_path_rejected(self):
        record = self.root / "session-0" / "samples" / "1.json"
        original = pipeline.read_json(record)
        changes = [dict(image_sha256="0"*64), dict(width=99), dict(image="../escape.png"), dict(detections=[dict(class_id=1, x1=1, y1=1, x2=10, y2=10)]), dict(detections=[dict(class_id=0, x1=1, y1=1, x2=100, y2=10)])]
        for change in changes:
            with self.subTest(change=change):
                pipeline.write_json(record, dict(original, **change))
                with self.assertRaises(pipeline.PipelineError):
                    pipeline.load_samples(self.context("inspect"))
        pipeline.write_json(record, original)

    def test_yolo_bounds_and_nonfinite_rejected(self):
        label = self.base / "label.txt"
        for text in ("0 nan 0.5 0.2 0.2", "0 0.1 0.5 0.4 0.2", "0 0.5 0.5 0 0.1", "1 0.5 0.5 0.1 0.1"):
            label.write_text(text)
            with self.assertRaises(pipeline.PipelineError):
                pipeline.read_labels(label, 64, 48, self.names)

    def test_duplicate_cross_session_rejected(self):
        self.approve()
        original = self.root / "session-0" / "images" / "1.png"
        duplicate = self.root / "session-1" / "images" / "1.png"
        duplicate.write_bytes(original.read_bytes())
        record = self.root / "session-1" / "samples" / "1.json"
        data = pipeline.read_json(record)
        data["image_sha256"] = pipeline.sha256(duplicate)
        pipeline.write_json(record, data)
        # 即使原始记录同步更改，旧审核绑定仍能识别原图被替换。
        with self.assertRaisesRegex(pipeline.PipelineError, "身份不一致"):
            pipeline.export_dataset(self.context("export", output=str(self.base / "dataset")))
        a = dict(image_sha256="a", _thumb=((10, 10, 10),)*256)
        b = dict(image_sha256="b", _thumb=((11, 10, 10),)*256)
        with self.assertRaisesRegex(pipeline.PipelineError, "近重复"):
            pipeline.check_leakage([dict(sample=a, split="train"), dict(sample=b, split="test")])

    def test_frozen_whitelist_rejects_unreviewed_image(self):
        dataset = self.freeze()
        Image.new("RGB", (64, 48), "white").save(dataset / "images" / "train" / "unknown.png")
        with self.assertRaisesRegex(pipeline.PipelineError, "白名单"):
            pipeline.validate_dataset(dataset)

    def test_prelabels_never_overwrite_reviewed_truth(self):
        self.approve()
        originals = {p: p.read_bytes() for p in (self.root / "reviews").rglob("revision.json")}
        _, samples = pipeline.load_samples(self.context("inspect"))
        prelabels = self.base / "prelabels.json"
        pipeline.write_json(prelabels, dict(schema_version=1, class_names=self.names, samples=[dict(session_id=k[0], sample_id=k[1], image_sha256=s["image_sha256"], detections=[]) for k, s in samples.items()]))
        output = self.base / "review2"
        pipeline.review_export(self.context("review_export", output=str(output), prelabels=str(prelabels)))
        self.assertTrue((output / "obj_train_data" / "session-0__1.txt").read_text().strip())
        self.assertTrue(all(p.read_bytes() == content for p, content in originals.items()))

    def test_cancel_and_dependency_failure_status(self):
        flag, status = self.base / "cancel", self.base / "status.json"
        flag.touch()
        code = pipeline.execute(self.job("inspect"), status, flag)
        self.assertEqual(code, 2)
        self.assertEqual(pipeline.read_json(status)["state"], "CANCELLED")
        weight = self.base / "local.pt"
        weight.write_bytes(b"fake")
        with patch.dict(sys.modules, {"torch": None}):
            with self.assertRaisesRegex(pipeline.PipelineError, "DEPENDENCY_MISSING"):
                pipeline.load_yolo(weight, training=True, job=self.trust(weight))
        with self.assertRaisesRegex(pipeline.PipelineError, "本地"):
            pipeline.load_yolo(self.base / "missing.pt", training=True)

    def test_user_pt_trust_is_enforced_before_framework_loading(self):
        weight = self.base / "confirmed.pt"
        other = self.base / "other.pt"
        onnx = self.base / "candidate.onnx"
        weight.write_bytes(b"trusted-identity-fixture")
        other.write_bytes(weight.read_bytes())
        onnx.write_bytes(b"onnx-identity-fixture")
        authorization = self.trust(weight)
        changed = dict(authorization, expected_weights_sha256="0" * 64)
        cases = (
            ("unconfirmed", {}, weight),
            ("different_file", authorization, other),
            ("changed_hash", changed, weight),
            ("missing_hash", dict(trusted_weights=True, trusted_weights_path=str(weight.resolve())), weight),
        )
        for reason, trust, target in cases:
            for operation, inputs in (
                ("train", dict(weights=str(target))),
                ("prelabel", dict(model=str(target))),
                ("evaluate", dict(model=str(target))),
                ("evaluate", dict(model=str(onnx), baseline_model=str(target))),
            ):
                with self.subTest(reason=reason, operation=operation, inputs=inputs):
                    status = self.base / "refused.json"
                    with patch.object(pipeline, "load_yolo", side_effect=AssertionError("不得加载任何框架模型")) as loader:
                        code = pipeline.execute(self.job(operation, **inputs, **trust), status)
                    self.assertEqual(code, 1)
                    self.assertEqual(pipeline.read_json(status)["state"], "FAILED")
                    self.assertIn("PT", pipeline.read_json(status)["message"])
                    loader.assert_not_called()
        # 明确确认后的文件替换必须在真正加载入口重新校验，不能复用旧检查结果。
        weight.write_bytes(b"replaced-after-confirmation")
        with patch.dict(sys.modules, {"torch": None}):
            with self.assertRaisesRegex(pipeline.PipelineError, "SHA-256"):
                pipeline.load_yolo(weight, training=True, job=authorization)

    def test_output_never_overwrites_existing(self):
        self.approve()
        with self.assertRaisesRegex(pipeline.PipelineError, "已存在"):
            pipeline.review_export(self.context("review_export", output=str(self.base / "review")))

    def test_split_registry_prevents_holdout_return_to_training(self):
        dataset = self.freeze()
        original = pipeline.read_json(dataset / "dataset.json")["split_groups"]
        changed = {sid: "train" if split == "test" else "test" if split == "train" else split for sid, split in original.items()}
        with self.assertRaisesRegex(pipeline.PipelineError, "不可改变"):
            pipeline.export_dataset(self.context("export", output=str(self.base / "dataset2"), split_groups=changed))
        pipeline.export_dataset(self.context("export", output=str(self.base / "dataset3")))
        self.assertEqual(pipeline.read_json(self.base / "dataset3" / "dataset.json")["split_groups"], original)

    def test_successful_training_and_baseline_evaluation_orchestration(self):
        dataset = self.freeze()
        frozen_files = {p: p.read_bytes() for p in dataset.rglob("*") if p.is_file()}
        weight = self.base / "local.pt"
        weight.write_bytes(b"trusted-test-fixture")
        captured = {}

        class FakeModel:
            names = ["person"]
            model = types.SimpleNamespace(end2end=False, model=[types.SimpleNamespace(end2end=False)])

            def __init__(self, path):
                self.path = Path(path)
                self.callbacks = {}

            def add_callback(self, name, callback):
                self.callbacks[name] = callback

            def train(self, **kwargs):
                captured["train"] = kwargs
                data_root = Path(pipeline.read_json(kwargs["data"])["path"])
                (data_root / "labels" / "train.cache").write_bytes(b"trainer-cache")
                run = Path(kwargs["project"]) / "run"
                (run / "weights").mkdir(parents=True)
                self.trainer = types.SimpleNamespace(epoch=0, stop=False, save_dir=run)
                for name in ("best.pt", "last.pt"):
                    (run / "weights" / name).write_bytes(b"completed-checkpoint")
                self.callbacks["on_train_batch_end"](self.trainer)
                self.callbacks["on_train_epoch_end"](self.trainer)
                self.callbacks["on_model_save"](self.trainer)
                return types.SimpleNamespace(results_dict={"metrics/mAP50-95(B)": 0.75})

            def export(self, **kwargs):
                captured["export"] = kwargs
                result = self.path.with_suffix(".onnx")
                result.write_bytes(b"fake-onnx-contract-tested-separately")
                return str(result)

            def val(self, **kwargs):
                captured.setdefault("evaluations", []).append(kwargs)
                data_root = Path(pipeline.read_json(kwargs["data"])["path"])
                (data_root / "labels" / "test.cache").write_bytes(b"validator-cache")
                self.callbacks["on_val_batch_end"](None)
                return types.SimpleNamespace(results_dict={"metrics/mAP50-95(B)": np.float32(0.75)}, summary=lambda: [dict(Class=np.str_("person"), Instances=np.int64(2), Recall=np.float32(0.9), nested=[np.uint64(3), np.bool_(True)])])

            def predict(self, **kwargs):
                captured.setdefault("predictions", []).append(kwargs)
                rows = [[3, 4, 28, 38, 0.9, 0]] if Path(kwargs["source"]).stem.endswith("__1") else []
                return [types.SimpleNamespace(boxes=types.SimpleNamespace(data=types.SimpleNamespace(cpu=lambda: types.SimpleNamespace(tolist=lambda: rows))))]

        status = self.base / "status.json"
        output = self.base / "training"
        with patch.object(pipeline, "load_yolo", side_effect=lambda path, training=False, **kwargs: FakeModel(path)), patch.object(pipeline, "onnx_contract", return_value=dict(input_shape=[1, 3, 640, 640], output_shape=[1, 5, 8400], detector_runtime="NOT_EXECUTED")), patch.object(pipeline, "load_embedded_onnx"):
            self.assertEqual(pipeline.execute(self.job("train", dataset=str(dataset), weights=str(weight), output=str(output), workers=8, **self.trust(weight)), status), 0)
            result = pipeline.read_json(status)["result"]
            self.assertTrue(Path(result["candidate"]).is_file())
            card = pipeline.read_json(result["candidate_card"])
            self.assertEqual(card["state"], "CANDIDATE_NOT_ACTIVATED")
            self.assertEqual(card["test_evaluation"], "NOT_EXECUTED")
            pipeline.validate_dataset(dataset)
            evaluation = self.base / "evaluation"
            self.assertEqual(pipeline.execute(self.job("evaluate", model=result["candidate"], baseline_model=result["candidate"], dataset=str(dataset), output=str(evaluation), workers=2), status), 0)
            measured = pipeline.read_json(status)["result"]
            self.assertTrue(measured["passed_compatibility"])
            self.assertFalse(measured["comparison"]["improvement_claim"])
            self.assertEqual(measured["comparison"]["state"], "MEASURED_REVIEW_REQUIRED")
            self.assertEqual(measured["model_sha256"], pipeline.sha256(result["candidate"]))
            report = pipeline.read_json(measured["evaluation"])
            self.assertEqual(report["fixed_threshold_per_class"][0]["recall"], 1.0)
            self.assertEqual(report["fixed_threshold_per_class"][0]["tp"], 1)
            self.assertEqual(report["background_fp_per_frame"], 0)
            self.assertEqual(report["per_class"][0]["Instances"], 2)
            self.assertEqual(report["per_class"][0]["nested"], [3, True])
            self.assertEqual(report["baseline"]["per_class"][0]["Instances"], 2)
        self.assertFalse(captured["export"]["nms"])
        self.assertEqual(captured["train"]["workers"], 8)
        self.assertEqual(pipeline.read_json(output / "config.json")["effective_training"]["workers"], 8)
        self.assertTrue(all(v["workers"] == 2 for v in captured["evaluations"]))
        self.assertFalse(captured["export"]["dynamic"])
        self.assertEqual(captured["export"]["opset"], 17)
        self.assertEqual(len(captured["evaluations"]), 2)
        self.assertTrue(all(v["split"] == "test" and v["conf"] == 0.001 for v in captured["evaluations"]))
        self.assertTrue(all(v["conf"] == 0.25 for v in captured["predictions"]))
        self.assertEqual({p: p.read_bytes() for p in dataset.rglob("*") if p.is_file()}, frozen_files)
        pipeline.validate_dataset(dataset)
        self.assertTrue(Path(captured["train"]["data"]).is_relative_to(output))
        self.assertTrue(all(Path(v["data"]).is_relative_to(evaluation) for v in captured["evaluations"]))

    def test_evaluation_rejects_model_replacement_during_each_measurement(self):
        dataset = self.freeze()
        for role in ("candidate", "baseline"):
            for phase in ("val", "predict"):
                with self.subTest(role=role, phase=phase):
                    candidate, baseline = self.base / "candidate.onnx", self.base / "baseline.onnx"
                    candidate.write_bytes(b"candidate-original")
                    baseline.write_bytes(b"baseline-original")
                    target = candidate if role == "candidate" else baseline
                    original = target.read_bytes()
                    observed = []
                    contracts = []
                    def contract(path, *_):
                        path = Path(path)
                        self.assertNotIn(path, (candidate, baseline))
                        contracts.append(path.read_bytes())
                        return {"fixture": True}
                    class Model:
                        names = ["person"]
                        def __init__(self, path):
                            self.path = Path(path)
                            self.selected = self.path.read_bytes() == original
                        def add_callback(self, *_): pass
                        def alter(self, stage):
                            if self.selected and stage == phase:
                                target.write_bytes(b"replacement-not-evaluated")
                                observed.append(self.path.read_bytes())
                        def val(self, **kwargs):
                            self.alter("val")
                            return types.SimpleNamespace(results_dict={"mAP": 0.75}, summary=lambda: [])
                        def predict(self, **kwargs):
                            self.alter("predict")
                            return [types.SimpleNamespace(boxes=types.SimpleNamespace(data=types.SimpleNamespace(cpu=lambda: types.SimpleNamespace(tolist=lambda: []))))]
                    status = self.base / f"{role}-{phase}-status.json"
                    output = self.base / f"{role}-{phase}-evaluation"
                    with patch.object(pipeline, "load_yolo", side_effect=lambda path, **kwargs: Model(path)), patch.object(pipeline, "onnx_contract", side_effect=contract), patch.object(pipeline, "load_embedded_onnx", side_effect=contract):
                        code = pipeline.execute(self.job("evaluate", model=str(candidate), baseline_model=str(baseline), dataset=str(dataset), output=str(output)), status)
                    self.assertEqual(code, 1, "评价期间替换源模型必须失败，不能授予新文件导入资格")
                    self.assertEqual(pipeline.read_json(status)["state"], "FAILED")
                    self.assertNotIn("passed_compatibility", pipeline.read_json(status)["result"])
                    self.assertFalse((output / "evaluation.json").exists())
                    self.assertTrue(observed)
                    self.assertEqual(contracts, [b"candidate-original", b"baseline-original"])
                    self.assertTrue(all(value == original for value in observed), "运行中的评价始终读取本次冻结字节")

    def test_evaluation_cache_stays_outside_frozen_dataset_even_on_failure(self):
        dataset = self.freeze()
        frozen_files = {p: p.read_bytes() for p in dataset.rglob("*") if p.is_file()}
        model_path = self.base / "model.onnx"
        model_path.write_bytes(b"fixture")
        output = self.base / "evaluation"
        class Validator:
            names = ["person"]
            def add_callback(self, *_): pass
            def val(self, **kwargs):
                data_root = Path(pipeline.read_json(kwargs["data"])["path"])
                (data_root / "labels" / "test.cache").write_bytes(b"cache-before-failure")
                raise RuntimeError("validator fixture stopped")
        with patch.object(pipeline, "load_yolo", return_value=Validator()), patch.object(pipeline, "onnx_contract", return_value={"fixture": True}):
            with self.assertRaisesRegex(RuntimeError, "fixture stopped"):
                pipeline.evaluate(self.context("evaluate", model=str(model_path), dataset=str(dataset), output=str(output)))
        self.assertEqual({p: p.read_bytes() for p in dataset.rglob("*") if p.is_file()}, frozen_files)
        pipeline.validate_dataset(dataset)

    def test_fixed_threshold_uses_frozen_truth_after_source_label_changes(self):
        dataset = self.freeze()
        manifest = pipeline.read_json(dataset / "dataset.json")
        positive = next(item for item in manifest["samples"]
                        if item["split"] == "test" and item["state"] == "VERIFIED_POSITIVE")
        weights = self.base / "truth.onnx"
        weights.write_bytes(b"fixture")
        output = self.base / "truth-evaluation"
        class Model:
            names = ["person"]
            def add_callback(self, *_): pass
            def val(self, **kwargs):
                work_data = Path(pipeline.read_json(kwargs["data"])["path"])
                self.frozen_label = work_data / positive["label"]
                assert self.frozen_label.read_text()
                (dataset / positive["label"]).write_text("")
                return types.SimpleNamespace(results_dict={"mAP": 1.0}, summary=lambda: [])
            def predict(self, **kwargs):
                rows = [[3, 4, 28, 38, 0.9, 0]] if Path(kwargs["source"]).stem == Path(positive["image"]).stem else []
                return [types.SimpleNamespace(boxes=types.SimpleNamespace(data=types.SimpleNamespace(cpu=lambda: types.SimpleNamespace(tolist=lambda: rows))))]
        with patch.object(pipeline, "load_yolo", return_value=Model()), patch.object(pipeline, "onnx_contract", return_value={"fixture": True}):
            pipeline.evaluate(self.context("evaluate", model=str(weights), dataset=str(dataset), output=str(output)))
        counts = pipeline.read_json(output / "evaluation.json")["fixed_threshold_per_class"][0]
        self.assertEqual((counts["tp"], counts["fp"], counts["fn"]), (1, 0, 0))

    def test_evaluation_pt_snapshot_retains_original_trust(self):
        dataset = self.freeze()
        weights = self.base / "trusted.pt"
        weights.write_bytes(b"explicitly-trusted-weight")
        paths = []
        class Model:
            names = ["person"]
            def add_callback(self, *_): pass
            def val(self, **kwargs):
                return types.SimpleNamespace(results_dict={"mAP": 0.5}, summary=lambda: [])
            def predict(self, **kwargs):
                return [types.SimpleNamespace(boxes=types.SimpleNamespace(data=types.SimpleNamespace(cpu=lambda: types.SimpleNamespace(tolist=lambda: []))))]
        def load(path, **kwargs):
            pipeline.check_pt_trust(path, kwargs["job"])
            self.assertFalse(kwargs.get("internal_training_output", False))
            self.assertEqual(Path(path).read_bytes(), b"explicitly-trusted-weight")
            self.assertNotEqual(Path(path), weights)
            paths.append(Path(path))
            return Model()
        output = self.base / "pt-evaluation"
        with patch.object(pipeline, "load_yolo", side_effect=load):
            result = pipeline.evaluate(self.context("evaluate", model=str(weights), dataset=str(dataset), output=str(output), **self.trust(weights)))
        self.assertEqual(len(paths), 1)
        self.assertTrue(paths[0].is_relative_to(output))
        self.assertEqual(result["model"], str(weights))
        self.assertEqual(result["model_sha256"], pipeline.sha256(weights))
        self.assertFalse(result["passed_compatibility"], "可信 PT 评价不能冒充可导入 ONNX")

    def test_onnx_contract_rejects_nested_external_weights_before_loading_data(self):
        dataset = self.freeze()
        model = self.base / "external.onnx"
        model.write_bytes(b"protobuf-fixture")
        trusted = self.base / "candidate.pt"
        trusted.write_bytes(b"trusted-candidate")
        tensor = types.SimpleNamespace(DESCRIPTOR=types.SimpleNamespace(full_name="onnx.TensorProto"), data_location=1, external_data=[])
        # ONNX 1.19 允许 protobuf 4.25；该版 FieldDescriptor 通过 label 表示 repeated。
        repeated = types.SimpleNamespace(type=11, TYPE_MESSAGE=11, label=3, LABEL_REPEATED=3)
        child = types.SimpleNamespace(DESCRIPTOR=types.SimpleNamespace(full_name="onnx.GraphProto"), ListFields=lambda: [(repeated, [tensor])])
        single = types.SimpleNamespace(type=11, TYPE_MESSAGE=11, label=1, LABEL_REPEATED=3)
        graph = types.SimpleNamespace(DESCRIPTOR=types.SimpleNamespace(full_name="onnx.ModelProto"), ListFields=lambda: [(single, child)])
        observed = []
        def load(path, **kwargs):
            observed.append(kwargs)
            return graph
        def unexpected_checker(_graph):
            self.fail("未冻结外部权重必须在 checker 或框架有机会读取前拒绝")
        onnx = types.SimpleNamespace(load=load, TensorProto=types.SimpleNamespace(EXTERNAL=1), checker=types.SimpleNamespace(check_model=unexpected_checker))
        for descriptors in ("legacy", "current"):
            if descriptors == "current":
                repeated.is_repeated, single.is_repeated = True, False
            for role in ("candidate", "baseline"):
                with self.subTest(descriptors=descriptors, role=role):
                    inputs = dict(model=str(model)) if role == "candidate" else dict(model=str(trusted), baseline_model=str(model), **self.trust(trusted))
                    with patch.dict(sys.modules, {"onnx": onnx}), patch.object(pipeline, "load_yolo") as load_model:
                        with self.assertRaisesRegex(pipeline.PipelineError, "外部权重"):
                            pipeline.evaluate(self.context("evaluate", dataset=str(dataset), output=str(self.base / f"external-{descriptors}-{role}"), **inputs))
                        load_model.assert_not_called()
        self.assertEqual(observed, [{"load_external_data": False}] * 4)

    def test_evaluation_metrics_reject_nonfinite_and_unknown_objects(self):
        for value in (float("nan"), float("inf"), np.float32("nan"), np.float64("-inf"),
                      np.complex64(1+2j), np.array([1]), object(), {1: "invalid key"}):
            with self.subTest(value=type(value).__name__):
                with self.assertRaises(pipeline.PipelineError):
                    pipeline.evaluation_json_value({"nested": [value]})
        normalized = pipeline.evaluation_json_value({"count": np.int64(2**60), "ratio": np.float32(0.25)})
        self.assertIs(type(normalized["count"]), int)
        self.assertEqual(normalized["count"], 2**60)
        self.assertIs(type(normalized["ratio"]), float)

    def test_workers_bounds_reject_invalid_jobs_before_model_loading(self):
        dataset = self.freeze()
        weight = self.base / "trusted.pt"
        weight.write_bytes(b"fixture")
        self.assertEqual(pipeline.training_workers(), 0)
        for count in (0, 1, 8):
            self.assertEqual(pipeline.training_workers(count), count)
        for value in (-1, 9, True, False, 1.5, "2", None):
            for operation in ("train", "evaluate"):
                with self.subTest(workers=value, operation=operation), patch.object(pipeline, "load_yolo") as load:
                    with self.assertRaisesRegex(pipeline.PipelineError, "workers"):
                        getattr(pipeline, operation)(self.context(operation, dataset=str(dataset), weights=str(weight), model=str(weight), workers=value, **self.trust(weight)))
                    load.assert_not_called()

    def test_train_orchestration_and_checkpoint_cancellation(self):
        dataset = self.freeze()
        weight = self.base / "local.pt"
        weight.write_bytes(b"trusted-test-fixture")
        flag = self.base / "cancel"
        captured = {}

        class FakeModel:
            names = ["person"]
            model = types.SimpleNamespace(end2end=False, model=[types.SimpleNamespace(end2end=False)])

            def __init__(self):
                self.callbacks = {}

            def add_callback(self, name, callback):
                self.callbacks[name] = callback

            def train(self, **kwargs):
                captured.update(kwargs)
                run = Path(kwargs["project"]) / "run"
                (run / "weights").mkdir(parents=True)
                self.trainer = types.SimpleNamespace(epoch=0, stop=False, save_dir=run)
                flag.touch()
                self.callbacks["on_train_epoch_end"](self.trainer)
                (run / "weights" / "last.pt").write_bytes(b"optimizer-checkpoint")
                (run / "weights" / "best.pt").write_bytes(b"best-checkpoint")
                self.callbacks["on_model_save"](self.trainer)

        status = self.base / "status.json"
        output = self.base / "training"
        with patch.object(pipeline, "load_yolo", return_value=FakeModel()):
            code = pipeline.execute(self.job("train", dataset=str(dataset), weights=str(weight), output=str(output), **self.trust(weight)), status, flag)
        self.assertEqual(code, 2)
        self.assertEqual(pipeline.read_json(status)["state"], "CANCELLED")
        self.assertTrue((output / "run" / "weights" / "last.pt").is_file())
        self.assertFalse((output / "candidate.json").exists())
        self.assertFalse(captured["amp"])
        self.assertFalse(captured["pretrained"])
        self.assertEqual(captured["workers"], 0)
        self.assertEqual(pipeline.read_json(output / "checkpoint.json")["last_sha256"], pipeline.sha256(output / "run" / "weights" / "last.pt"))

    def test_resume_restores_native_state_in_new_directory(self):
        dataset = self.freeze()
        original = self.base / "interrupted"
        weights = original / "run" / "weights"
        weights.mkdir(parents=True)
        last, best = weights / "last.pt", weights / "best.pt"
        last.write_bytes(b"optimizer-state-epoch-0")
        best.write_bytes(b"historical-best")
        config = dict(schema_version=1, dataset_sha256=pipeline.sha256(dataset / "dataset.json"), class_names=self.names, effective_training=dict(epochs=3, imgsz=640, batch=8, device="cpu", workers=2))
        pipeline.write_json(original / "config.json", config)
        pipeline.write_json(original / "checkpoint.json", dict(dataset_sha256=config["dataset_sha256"], config_sha256=pipeline.sha256(original / "config.json"), last_sha256=pipeline.sha256(last), best_sha256=pipeline.sha256(best), epoch=0))
        originals = {p: p.read_bytes() for p in original.rglob("*") if p.is_file()}
        observed = {}

        class NativeTrainer:
            def __init__(self, overrides):
                self.check_resume(overrides)
                self.save_dir = Path(self.args.save_dir)
                self.best = self.save_dir / "weights" / "best.pt"
                self.best.parent.mkdir(parents=True)
                self.epoch = 1

            def check_resume(self, overrides):
                observed["native_check_resume"] = True
                self.args = types.SimpleNamespace(save_dir=str(original / "run"), project=str(original), name="run", data=str(dataset / "data.yaml"))

            def resume_training(self, checkpoint):
                observed["optimizer"] = checkpoint["optimizer"]
                observed["start_epoch"] = checkpoint["epoch"] + 1

        class Model:
            names = ["person"]
            model = types.SimpleNamespace(end2end=False, model=[types.SimpleNamespace(end2end=False)])
            ckpt = dict(epoch=0, optimizer={"state": "kept"}, scaler={"scale": 1})

            def __init__(self, path):
                self.path, self.callbacks = Path(path), {}

            def add_callback(self, name, callback):
                self.callbacks[name] = callback

            def train(self, **kwargs):
                observed["resume"] = kwargs["resume"]
                self.trainer = kwargs["trainer"](kwargs)
                observed["data"] = self.trainer.args.data
                observed["workers"] = (kwargs["workers"], self.trainer.args.workers)
                data_root = Path(pipeline.read_json(self.trainer.args.data)["path"])
                (data_root / "labels" / "train.cache").write_bytes(b"resumed-trainer-cache")
                self.trainer.resume_training(self.ckpt)
                observed["seed_best"] = self.trainer.best.read_bytes()
                (self.trainer.best.parent / "last.pt").write_bytes(b"epoch-1")
                self.callbacks["on_train_epoch_end"](self.trainer)
                self.callbacks["on_model_save"](self.trainer)
                return types.SimpleNamespace(results_dict={"metrics/mAP50-95(B)": 0.7})

            def export(self, **kwargs):
                path = self.path.with_suffix(".onnx")
                path.write_bytes(b"onnx")
                return path

        output = self.base / "resumed"
        with patch.object(pipeline, "load_yolo", side_effect=lambda path, training=False, **kwargs: Model(path)), patch.object(pipeline, "detection_trainer_class", return_value=NativeTrainer), patch.object(pipeline, "onnx_contract", return_value={}):
            result = pipeline.train(self.context("train", weights=str(last), dataset=str(dataset), output=str(output), resume=True, epochs=3, **self.trust(last)))
        self.assertTrue(observed["resume"])
        self.assertTrue(observed["native_check_resume"])
        self.assertEqual(observed["workers"], (2, 2))
        self.assertEqual(pipeline.read_json(output / "config.json")["effective_training"]["workers"], 2)
        self.assertTrue(Path(observed["data"]).is_relative_to(output))
        pipeline.validate_dataset(dataset)
        self.assertEqual(observed["optimizer"], {"state": "kept"})
        self.assertEqual(observed["start_epoch"], 1)
        self.assertEqual(observed["seed_best"], b"historical-best")
        self.assertTrue(Path(result["candidate"]).is_relative_to(output))
        self.assertTrue(all(p.read_bytes() == data for p, data in originals.items()))
        for alteration, message in ((dict(epochs=4), "epochs=3"), (dict(imgsz=320), "imgsz=640"), (dict(workers=0), "workers=2")):
            with self.assertRaisesRegex(pipeline.PipelineError, message):
                pipeline.resume_source(self.job("train", weights=str(last), **alteration), dataset, pipeline.read_json(dataset / "dataset.json"))
        changed = dict(pipeline.read_json(dataset / "dataset.json"), class_names=["different"])
        with self.assertRaisesRegex(pipeline.PipelineError, "类别不同"):
            pipeline.resume_source(self.job("train", weights=str(last)), dataset, changed)
        config["effective_training"].pop("workers")
        pipeline.write_json(original / "config.json", config)
        checkpoint = pipeline.read_json(original / "checkpoint.json")
        checkpoint["config_sha256"] = pipeline.sha256(original / "config.json")
        pipeline.write_json(original / "checkpoint.json", checkpoint)
        legacy = pipeline.resume_source(self.job("train", weights=str(last), workers=0), dataset, pipeline.read_json(dataset / "dataset.json"))
        self.assertEqual(legacy["effective"]["workers"], 0)
        with self.assertRaisesRegex(pipeline.PipelineError, "workers=0"):
            pipeline.resume_source(self.job("train", weights=str(last), workers=2), dataset, pipeline.read_json(dataset / "dataset.json"))
        config["dataset_sha256"] = "0"*64
        pipeline.write_json(original / "config.json", config)
        with self.assertRaisesRegex(pipeline.PipelineError, "数据版本"):
            pipeline.resume_source(self.job("train", weights=str(last)), dataset, pipeline.read_json(dataset / "dataset.json"))

    def test_inspect_safely_parses_metadata_names(self):
        model = self.base / "model.onnx"
        model.write_bytes(b"metadata-fixture")
        for encoded, expected in (("{0: 'person', 1: 'head'}", ["person", "head"]), ('{"0":"person"}', ["person"]), ("{1: 'person'}", []), ("__import__('os').getcwd()", [])):
            metadata = types.SimpleNamespace(custom_metadata_map={"names": encoded})
            session = types.SimpleNamespace(get_modelmeta=lambda: metadata, get_inputs=lambda: [], get_outputs=lambda: [])
            ort = types.SimpleNamespace(InferenceSession=lambda *args, **kwargs: session)
            with patch.dict(sys.modules, {"onnxruntime": ort}):
                result = pipeline.inspect(pipeline.Context(dict(operation="inspect", model=str(model))))
            self.assertEqual(result["model_class_names"], expected)


if __name__ == "__main__":
    unittest.main()
