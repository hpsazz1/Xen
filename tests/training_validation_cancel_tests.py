"""Actual Ultralytics CPU training cancellation at validation/epoch boundaries.

Uses only locally generated weights and synthetic reviewed data; no downloads.
Run in the pinned training environment; retained output root must be explicit.
"""
import argparse
import json
import os
from pathlib import Path
import uuid
from unittest.mock import patch

from dataset_snapshot_tests import fixture, pipeline


def run_case(root, mode):
    import torch
    from ultralytics import YOLO
    dataset, context = fixture(root)
    architecture = root / "tiny.yaml"
    architecture.write_text("nc: 1\nbackbone:\n  - [-1, 1, Conv, [8, 3, 2]]\n  - [-1, 1, Conv, [16, 3, 2]]\n  - [-1, 1, Conv, [32, 3, 2]]\nhead:\n  - [[-1], 1, Detect, [nc]]\n", encoding="utf-8")
    weights = root / "synthetic-random.pt"
    YOLO(str(architecture), task="detect").save(weights)
    output, cancel, status = root / "training", root / "cancel.flag", root / "status.json"
    batch_epochs, requested = set(), []
    actual_load = pipeline.load_yolo
    def load(*args, **kwargs):
        model = actual_load(*args, **kwargs)
        if kwargs.get("internal_training_output"):
            return model
        def batch(trainer):
            batch_epochs.add(int(trainer.epoch))
        def request(_runner):
            if requested or (mode.startswith("last_") and model.trainer.epoch != 2):
                return
            cancel.write_text("cancel at actual callback", encoding="utf-8")
            requested.append(mode)
        model.add_callback("on_train_batch_start", batch)
        if mode in ("validation", "last_validation"):
            model.add_callback("on_val_end", request)
        elif mode in ("after_save", "last_after_save"):
            model.add_callback("on_fit_epoch_end", request)
        return model
    job = context("train", dataset=str(dataset), weights=str(weights), trusted_weights=True, trusted_weights_path=str(weights), expected_weights_sha256=pipeline.sha256(weights), output=str(output), epochs=3, imgsz=64, batch=2, workers=0, device="cpu").job
    with patch.object(pipeline, "load_yolo", load):
        exit_code = pipeline.execute(job, status=status, cancel=cancel)
    state = pipeline.read_json(status)["state"]
    metadata = pipeline.read_json(output / "checkpoint.json")
    last = output / "run" / "weights" / "last.pt"
    # The only deserialized checkpoint is this test's own just-created model.
    checkpoint = torch.load(last, map_location="cpu", weights_only=False)
    optimizer_retained = checkpoint.get("optimizer") is not None
    if mode == "healthy":
        passed = exit_code == 0 and state == "SUCCEEDED" and batch_epochs == {0, 1, 2} and (output / "candidate.json").is_file()
        hashes_match = None
    else:
        hashes_match = metadata["last_sha256"] == pipeline.sha256(last) and metadata["config_sha256"] == pipeline.sha256(output / "config.json")
        expected_epoch = 2 if mode.startswith("last_") else 0
        passed = exit_code == 2 and state == "CANCELLED" and bool(requested) and batch_epochs == set(range(expected_epoch + 1)) and metadata["epoch"] == expected_epoch and checkpoint["epoch"] == expected_epoch and optimizer_retained and hashes_match and not (output / "candidate.json").exists()
    return dict(case=mode, passed=passed, exit_code=exit_code, state=state, epochs_with_batches=sorted(batch_epochs), checkpoint_epoch=metadata["epoch"], optimizer_retained=optimizer_retained, hashes_match=hashes_match)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("training-cancel-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    for key, value in {"YOLO_CONFIG_DIR": root / "yolo-config", "MPLCONFIGDIR": root / "matplotlib", "TORCH_HOME": root / "torch"}.items():
        os.environ[key] = str(value)
    os.environ.update(YOLO_OFFLINE="true", YOLO_AUTOINSTALL="false", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    results = [run_case(root / mode, mode) for mode in ("validation", "after_save", "last_validation", "last_after_save", "healthy")]
    pipeline.write_json(root / "results.json", results)
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
