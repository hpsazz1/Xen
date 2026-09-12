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

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "model_data_pipeline.py"
spec = importlib.util.spec_from_file_location("model_data_pipeline", SCRIPT)
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
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

    def context(self, operation, **kwargs):
        return pipeline.Context(self.job(operation, **kwargs))

    def approve(self):
        output = self.base / "review"
        pipeline.review_export(self.context("review_export", output=str(output)))
        manifest = pipeline.read_json(output / "review.json")
        manifest["reviewer"] = "专项测试审核员"
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
                pipeline.load_yolo(weight, training=True)
        with self.assertRaisesRegex(pipeline.PipelineError, "本地"):
            pipeline.load_yolo(self.base / "missing.pt", training=True)

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
                self.callbacks["on_val_batch_end"](None)
                return types.SimpleNamespace(results_dict={"metrics/mAP50-95(B)": 0.75}, summary=lambda: [dict(Class="person", Recall=0.9)])

            def predict(self, **kwargs):
                captured.setdefault("predictions", []).append(kwargs)
                rows = [[3, 4, 28, 38, 0.9, 0]] if Path(kwargs["source"]).stem.endswith("__1") else []
                return [types.SimpleNamespace(boxes=types.SimpleNamespace(data=types.SimpleNamespace(cpu=lambda: types.SimpleNamespace(tolist=lambda: rows))))]

        status = self.base / "status.json"
        output = self.base / "training"
        with patch.object(pipeline, "load_yolo", side_effect=lambda path, training=False: FakeModel(path)), patch.object(pipeline, "onnx_contract", return_value=dict(input_shape=[1, 3, 640, 640], output_shape=[1, 5, 8400], detector_runtime="NOT_EXECUTED")):
            self.assertEqual(pipeline.execute(self.job("train", dataset=str(dataset), weights=str(weight), output=str(output)), status), 0)
            result = pipeline.read_json(status)["result"]
            self.assertTrue(Path(result["candidate"]).is_file())
            card = pipeline.read_json(result["candidate_card"])
            self.assertEqual(card["state"], "CANDIDATE_NOT_ACTIVATED")
            self.assertEqual(card["test_evaluation"], "NOT_EXECUTED")
            evaluation = self.base / "evaluation"
            self.assertEqual(pipeline.execute(self.job("evaluate", model=result["candidate"], baseline_model=result["candidate"], dataset=str(dataset), output=str(evaluation)), status), 0)
            measured = pipeline.read_json(status)["result"]
            self.assertTrue(measured["passed_compatibility"])
            self.assertFalse(measured["comparison"]["improvement_claim"])
            self.assertEqual(measured["comparison"]["state"], "MEASURED_REVIEW_REQUIRED")
            self.assertEqual(measured["model_sha256"], pipeline.sha256(result["candidate"]))
            report = pipeline.read_json(measured["evaluation"])
            self.assertEqual(report["fixed_threshold_per_class"][0]["recall"], 1.0)
            self.assertEqual(report["fixed_threshold_per_class"][0]["tp"], 1)
            self.assertEqual(report["background_fp_per_frame"], 0)
        self.assertFalse(captured["export"]["nms"])
        self.assertFalse(captured["export"]["dynamic"])
        self.assertEqual(captured["export"]["opset"], 17)
        self.assertEqual(len(captured["evaluations"]), 2)
        self.assertTrue(all(v["split"] == "test" and v["conf"] == 0.001 for v in captured["evaluations"]))
        self.assertTrue(all(v["conf"] == 0.25 for v in captured["predictions"]))

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
            code = pipeline.execute(self.job("train", dataset=str(dataset), weights=str(weight), output=str(output)), status, flag)
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
        config = dict(schema_version=1, dataset_sha256=pipeline.sha256(dataset / "dataset.json"), class_names=self.names, effective_training=dict(epochs=3, imgsz=640, batch=8, device="cpu"))
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
        with patch.object(pipeline, "load_yolo", side_effect=lambda path, training=False: Model(path)), patch.object(pipeline, "detection_trainer_class", return_value=NativeTrainer), patch.object(pipeline, "onnx_contract", return_value={}):
            result = pipeline.train(self.context("train", weights=str(last), dataset=str(dataset), output=str(output), resume=True, epochs=3))
        self.assertTrue(observed["resume"])
        self.assertTrue(observed["native_check_resume"])
        self.assertEqual(observed["optimizer"], {"state": "kept"})
        self.assertEqual(observed["start_epoch"], 1)
        self.assertEqual(observed["seed_best"], b"historical-best")
        self.assertTrue(Path(result["candidate"]).is_relative_to(output))
        self.assertTrue(all(p.read_bytes() == data for p, data in originals.items()))
        for alteration, message in ((dict(epochs=4), "epochs=3"), (dict(imgsz=320), "imgsz=640")):
            with self.assertRaisesRegex(pipeline.PipelineError, message):
                pipeline.resume_source(self.job("train", weights=str(last), **alteration), dataset, pipeline.read_json(dataset / "dataset.json"))
        changed = dict(pipeline.read_json(dataset / "dataset.json"), class_names=["different"])
        with self.assertRaisesRegex(pipeline.PipelineError, "类别不同"):
            pipeline.resume_source(self.job("train", weights=str(last)), dataset, changed)
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
