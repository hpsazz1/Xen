"""D25 real CPU ORT inspection of generated local models; no hardware or downloads."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import uuid
from unittest.mock import patch

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper

spec = importlib.util.spec_from_file_location("inspection_pipeline", Path(__file__).resolve().parents[1] / "scripts/model_data_pipeline.py")
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


def model(path, marker="A", names="{0: 'person'}", external=False, attribute=False):
    tensor = numpy_helper.from_array(np.full((1, 5, 21), 1 if marker == "A" else 2, dtype=np.float32), "weights")
    if attribute:
        graph = helper.make_graph([helper.make_node("Constant", [], ["output0"], value=tensor)], "attribute", [], [helper.make_tensor_value_info("output0", TensorProto.FLOAT, [1, 5, 21])])
    else:
        graph = helper.make_graph([helper.make_node("Identity", ["weights"], ["output0"])], "initializer", [], [helper.make_tensor_value_info("output0", TensorProto.FLOAT, [1, 5, 21])], [tensor])
    value = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
    for key, text in {"marker": marker, "names": names}.items():
        item = value.metadata_props.add(); item.key = key; item.value = text
    onnx.checker.check_model(value)
    if external:
        onnx.save_model(value, path, save_as_external_data=True, all_tensors_to_one_file=True, location=path.stem + ".weights", size_threshold=0, convert_attribute=attribute)
    else:
        onnx.save_model(value, path)


def inspect(path):
    return pipeline.inspect(pipeline.Context(dict(operation="inspect", model=str(path))))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("model-inspection-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    results = []
    original = ort.InferenceSession
    for replace in (False, True):
        path = root / ("replace.onnx" if replace else "healthy.onnx")
        model(path)
        expected = hashlib.sha256(path.read_bytes()).hexdigest()
        observed = []
        def create(*args, **kwargs):
            session = original(*args, **kwargs)
            observed.append(dict(providers=session.get_providers(), value=float(session.run(None, {})[0][0, 0, 0])))
            if replace:
                model(path, "B")
            return session
        with patch.object(ort, "InferenceSession", create):
            result = inspect(path)
        good = result["model_sha256"] == expected and result["metadata"]["marker"] == "A" and result["model_class_names"] == ["person"] and observed == [dict(providers=["CPUExecutionProvider"], value=1.0)]
        results.append(dict(case="model-replaced-after-real-session" if replace else "healthy-real-session", passed=good, expected=expected, reported=result["model_sha256"], current=pipeline.sha256(path), session=observed))

    for attribute in (False, True):
        path = root / ("external-attribute.onnx" if attribute else "external-initializer.onnx")
        model(path, external=True, attribute=attribute)
        calls = []
        def create(*args, **kwargs):
            calls.append(True)
            return original(*args, **kwargs)
        rejected = False
        error = ""
        with patch.object(ort, "InferenceSession", create):
            try:
                inspect(path)
            except pipeline.PipelineError as exc:
                rejected = True; error = str(exc)
            except Exception as exc:
                error = str(exc)
        results.append(dict(case="external-attribute" if attribute else "external-initializer", passed=rejected and not calls, ort_calls=len(calls), error=error))

    for index, (encoded, expected) in enumerate((("{0: 'person', 1: 'head'}", ["person", "head"]), ('{"0":"person"}', ["person"]), ("{1: 'person'}", []), ("__import__('os').getcwd()", []))):
        path = root / f"names-{index}.onnx"
        model(path, names=encoded)
        result = inspect(path)
        results.append(dict(case=f"names-{index}", passed=result["model_class_names"] == expected and bool(result.get("metadata_warning")) == (not expected)))
    path = root / "missing-dependency.onnx"
    model(path)
    with patch.dict(sys.modules, {"onnx": None}), patch.object(ort, "InferenceSession", wraps=original) as constructor:
        rejected = False
        try:
            inspect(path)
        except pipeline.PipelineError as exc:
            rejected = "DEPENDENCY_MISSING" in str(exc)
        results.append(dict(case="missing-onnx-before-ort", passed=rejected and constructor.call_count == 0))
    path = root / "invalid.onnx"
    path.write_bytes(b"invalid protobuf")
    status = root / "invalid-status.json"
    with patch.object(ort, "InferenceSession", wraps=original) as constructor:
        exit_code = pipeline.execute(dict(operation="inspect", model=str(path)), status=status)
        state = pipeline.read_json(status)
        results.append(dict(case="invalid-protobuf-failed-job", passed=exit_code == 1 and state["state"] == "FAILED" and constructor.call_count == 0))
    pipeline.write_json(root / "results.json", results)
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
