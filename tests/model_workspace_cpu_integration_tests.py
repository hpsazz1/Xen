"""Actual Workspace -> production CPU evaluator -> ordinary ONNX model import.

Explicit pinned environment and native host are required. All inputs are generated
locally; no Runtime, device, UI, PT file or model activation is involved.
"""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from dataset_snapshot_tests import create_onnx, fixture, pipeline


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("workspace-cpu-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    for key, name in {"YOLO_CONFIG_DIR": "yolo-config", "MPLCONFIGDIR": "matplotlib", "TORCH_HOME": "torch"}.items():
        os.environ[key] = str(root / name)
    os.environ.update(YOLO_OFFLINE="true", YOLO_AUTOINSTALL="false", CUDA_VISIBLE_DEVICES="-1",
                      OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", PYTHONDONTWRITEBYTECODE="1")
    packages = {name: importlib.metadata.version(name) for name in ("ultralytics", "torch", "onnxruntime", "onnx")}
    if packages != {"ultralytics": "8.3.203", "torch": "2.8.0+cu128", "onnxruntime": "1.22.1", "onnx": "1.19.0"}:
        raise RuntimeError("Use the existing pinned environment; do not install dependencies here")
    dataset, _ = fixture(root / "inputs")
    model = root / "inputs/synthetic.onnx"
    create_onnx(model)
    before = {str(path.relative_to(root)): pipeline.sha256(path) for path in (root / "inputs").rglob("*") if path.is_file()}
    script = Path(__file__).resolve().parents[1] / "scripts/model_data_pipeline.py"
    command = [str(args.host.resolve()), sys.executable, str(script), str(dataset), str(model), str(root / "workspace")]
    with (root / "native-host.log").open("xb") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=210, cwd=root)
    after = {str(path.relative_to(root)): pipeline.sha256(path) for path in (root / "inputs").rglob("*") if path.is_file()}
    record = root / "workspace/integration-result.json"
    integration = pipeline.read_json(record) if record.is_file() else None
    passed = result.returncode == 0 and before == after and integration and integration["failures"] == 0
    report = dict(root=str(root), passed=bool(passed), returncode=result.returncode, packages=packages,
                  command=command, input_files_unchanged=before == after, input_hashes=before, integration=integration)
    pipeline.write_json(root / "results.json", report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
