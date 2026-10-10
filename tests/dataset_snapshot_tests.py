"""D24 real dataset readers/copies and optional local CPU model evaluation.

Run with the existing training environment and --real for actual Ultralytics/ORT.
Fixtures are generated under the explicit output root and retained for evidence.
No downloads, capture, devices, model activation, or external input.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import uuid
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/model_data_pipeline.py"
spec = importlib.util.spec_from_file_location("dataset_snapshot_pipeline", SCRIPT)
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


def fixture(base):
    from PIL import Image
    raw = base / "raw"
    for index, color in enumerate(((200, 10, 10), (10, 200, 10), (10, 10, 200))):
        sid = f"session-{index}"
        session = raw / sid
        (session / "images").mkdir(parents=True)
        (session / "samples").mkdir()
        pipeline.write_json(session / "session.json", dict(schema_version=1, session_id=sid, class_names=["person"]))
        for sample_id in ("1", "2"):
            image = session / "images" / f"{sample_id}.png"
            picture = Image.new("RGB", (64, 48), color)
            if sample_id == "2":
                picture.putpixel((0, 0), (255, 255, 255))
            picture.save(image)
            boxes = [dict(class_id=0, x1=3, y1=4, x2=28, y2=38, confidence=0.8)] if sample_id == "1" else []
            pipeline.write_json(session / "samples" / f"{sample_id}.json", dict(schema_version=1, session_id=sid, sample_id=sample_id, image=f"images/{sample_id}.png", image_sha256=pipeline.sha256(image), width=64, height=48, detections=boxes, detection_status="SUCCESS", review_state="PRELABELED"))
    def context(operation, **kwargs):
        return pipeline.Context(dict(operation=operation, root=str(raw), class_names=["person"], **kwargs))
    review = base / "review"
    pipeline.review_export(context("review_export", output=str(review)))
    manifest = pipeline.read_json(review / "review.json")
    for item in manifest["samples"]:
        item["state"] = "VERIFIED_POSITIVE" if item["sample_id"] == "1" else "VERIFIED_NEGATIVE"
    pipeline.write_json(review / "review.json", manifest)
    pipeline.import_labels(context("import_labels", review_manifest=str(review / "review.json")))
    dataset = base / "dataset"
    pipeline.export_dataset(context("export", output=str(dataset)))
    return dataset, context


def replace_manifest(dataset):
    value = pipeline.read_json(dataset / "dataset.json")
    value["samples"][0]["revision"] += "-different-source-version"
    pipeline.write_json(dataset / "dataset.json", value)
    pipeline.write_json(dataset / "dataset_identity.json", dict(dataset_sha256=pipeline.sha256(dataset / "dataset.json")))


def after_read(path, action):
    """Pause only at a real file close; preserve the actual bytes read/parser/hash."""
    original = Path.open
    changed = []
    class FileBoundary:
        def __init__(self, stream): self.stream = stream
        def __enter__(self): return self.stream.__enter__()
        def __exit__(self, *args):
            result = self.stream.__exit__(*args)
            if not changed:
                changed.append(True)
                action()
            return result
    def open_file(self, *args, **kwargs):
        stream = original(self, *args, **kwargs)
        mode = args[0] if args else kwargs.get("mode", "r")
        return FileBoundary(stream) if self == path and "r" in mode and not changed else stream
    return patch.object(Path, "open", open_file), changed


def validate_cases(root):
    results = []
    dataset, _ = fixture(root / "read-race")
    boundary, changed = after_read(dataset / "dataset.json", lambda: replace_manifest(dataset))
    rejected = False
    with boundary:
        try:
            pipeline.validate_dataset(dataset)
        except pipeline.PipelineError:
            rejected = True
    results.append(dict(case="manifest-read-then-replace", passed=rejected and bool(changed)))

    dataset, _ = fixture(root / "yaml-race")
    yaml = dataset / "data.yaml"
    benign = yaml.read_bytes()
    value = pipeline.read_json(yaml)
    value["download"] = "not executed"
    pipeline.write_json(yaml, value)
    manifest = pipeline.read_json(dataset / "dataset.json")
    manifest["data_yaml_sha256"] = pipeline.sha256(yaml)
    pipeline.write_json(dataset / "dataset.json", manifest)
    pipeline.write_json(dataset / "dataset_identity.json", dict(dataset_sha256=pipeline.sha256(dataset / "dataset.json")))
    def change_yaml():
        yaml.write_bytes(benign)
    boundary, changed = after_read(yaml, change_yaml)
    rejected = False
    with boundary:
        try:
            pipeline.validate_dataset(dataset)
        except pipeline.PipelineError:
            rejected = True
    results.append(dict(case="yaml-hash-then-replace", passed=rejected and bool(changed)))

    dataset, _ = fixture(root / "bom-control")
    path = dataset / "dataset.json"
    path.write_bytes(b"\xef\xbb\xbf" + path.read_bytes())
    pipeline.write_json(dataset / "dataset_identity.json", dict(dataset_sha256=pipeline.sha256(path)))
    manifest = pipeline.validate_dataset(dataset)
    results.append(dict(case="healthy-bom-exact-byte-identity", passed=manifest["counts"] == dict(train=2, val=2, test=2)))
    return results


def create_onnx(path):
    import numpy as np
    import onnx
    from onnx import TensorProto, helper, numpy_helper
    predictions = np.zeros((1, 5, 21), dtype=np.float32)
    graph = helper.make_graph([helper.make_node("Constant", [], ["output0"], value=numpy_helper.from_array(predictions))], "synthetic-no-detections", [helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 64, 64])], [helper.make_tensor_value_info("output0", TensorProto.FLOAT, [1, 5, 21])])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
    for key, value in {"names": "{0: 'person'}", "task": "detect", "stride": "32", "batch": "1", "imgsz": "[64, 64]"}.items():
        item = model.metadata_props.add(); item.key = key; item.value = value
    onnx.checker.check_model(model)
    onnx.save(model, path)


def real_evaluation(root, mutate):
    dataset, context = fixture(root)
    expected_bytes = (dataset / "dataset.json").read_bytes()
    expected_hash = hashlib.sha256(expected_bytes).hexdigest()
    model = root / "synthetic.onnx"
    create_onnx(model)
    output = root / "evaluation"
    original = pipeline.working_dataset
    boundaries = []
    def working(*args, **kwargs):
        actual = original(*args, **kwargs)
        boundaries.append(True)
        if mutate:
            replace_manifest(dataset)
        return actual
    with patch.object(pipeline, "working_dataset", working):
        result = pipeline.evaluate(context("evaluate", dataset=str(dataset), model=str(model), output=str(output), imgsz=64, batch=1, workers=0, device="cpu"))
    report = pipeline.read_json(output / "evaluation.json")
    # Real ORT/Ultralytics zero detections on one positive and one negative frame.
    counts = report["fixed_threshold_per_class"][0]
    passed = bool(boundaries) and result["dataset_sha256"] == expected_hash and report["dataset_sha256"] == expected_hash and (output / "dataset.snapshot.json").read_bytes() == expected_bytes and (counts["tp"], counts["fp"], counts["fn"]) == (0, 0, 1)
    return dict(case="real-cpu-evaluation-" + ("source-replaced" if mutate else "healthy"), passed=passed, expected=expected_hash, report=report["dataset_sha256"], source=pipeline.sha256(dataset / "dataset.json"), counts=counts)


def real_training(root, mutate):
    from ultralytics import YOLO
    dataset, context = fixture(root)
    expected_bytes = (dataset / "dataset.json").read_bytes()
    expected = pipeline.sha256(dataset / "dataset.json")
    architecture = root / "tiny.yaml"
    architecture.write_text("nc: 1\nbackbone:\n  - [-1, 1, Conv, [8, 3, 2]]\n  - [-1, 1, Conv, [16, 3, 2]]\n  - [-1, 1, Conv, [32, 3, 2]]\nhead:\n  - [[-1], 1, Detect, [nc]]\n", encoding="utf-8")
    weights = root / "synthetic-random.pt"
    YOLO(str(architecture), task="detect").save(weights)
    output = root / "training"
    original = pipeline.working_dataset
    def working(*args, **kwargs):
        actual = original(*args, **kwargs)
        if mutate:
            replace_manifest(dataset)
        return actual
    with patch.object(pipeline, "working_dataset", working):
        pipeline.train(context("train", dataset=str(dataset), weights=str(weights), trusted_weights=True, trusted_weights_path=str(weights), expected_weights_sha256=pipeline.sha256(weights), output=str(output), epochs=1, imgsz=64, batch=2, workers=0, device="cpu"))
    identities = {name: pipeline.read_json(output / name)["dataset_sha256"] for name in ("config.json", "checkpoint.json", "candidate.json")}
    return dict(case="real-cpu-training-" + ("source-replaced" if mutate else "healthy"), passed=all(value == expected for value in identities.values()) and (output / "dataset.snapshot.json").read_bytes() == expected_bytes, expected=expected, artifacts=identities, source=pipeline.sha256(dataset / "dataset.json"))


def resume_case(root):
    dataset, context = fixture(root)
    snapshot = pipeline.validate_dataset_snapshot(dataset)
    original = root / "incomplete-job"
    weights = original / "run" / "weights"
    weights.mkdir(parents=True)
    last, best = weights / "last.pt", weights / "best.pt"
    last.write_bytes(b"checkpoint identity fixture; never deserialized")
    best.write_bytes(b"best identity fixture; never deserialized")
    pipeline.write_json(original / "config.json", dict(dataset_sha256=snapshot.sha256, class_names=["person"], effective_training=dict(epochs=3, imgsz=64, workers=0)))
    pipeline.write_json(original / "checkpoint.json", dict(dataset_sha256=snapshot.sha256, config_sha256=pipeline.sha256(original / "config.json"), last_sha256=pipeline.sha256(last), best_sha256=pipeline.sha256(best)))
    replace_manifest(dataset)
    job = dict(weights=str(last))
    result = pipeline.resume_source(job, snapshot)
    rejected = False
    try:
        pipeline.resume_source(job, pipeline.validate_dataset_snapshot(dataset))
    except pipeline.PipelineError:
        rejected = True
    return dict(case="resume-metadata-uses-validated-snapshot", passed=result["config"]["dataset_sha256"] == snapshot.sha256 and rejected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--real", action="store_true")
    parser.add_argument("--train", action="store_true")
    args = parser.parse_args()
    root = args.output_root.resolve() / ("dataset-snapshot-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    for key, value in {"YOLO_CONFIG_DIR": root / "yolo-config", "MPLCONFIGDIR": root / "matplotlib", "TORCH_HOME": root / "torch"}.items():
        os.environ[key] = str(value)
    os.environ.update(YOLO_OFFLINE="true", YOLO_AUTOINSTALL="false", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    results = validate_cases(root)
    results.append(resume_case(root / "resume"))
    if args.real:
        results.extend(real_evaluation(root / name, mutate) for name, mutate in (("eval-healthy", False), ("eval-replaced", True)))
    if args.train:
        results.extend(real_training(root / name, mutate) for name, mutate in (("train-healthy", False), ("train-replaced", True)))
    pipeline.write_json(root / "results.json", results)
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
