"""Actual CPU resume after cancellation, using only self-generated trusted PT files.

Run explicitly in the pinned training environment; no downloads or model activation.
Retain the required output root for checkpoint and identity inspection.
"""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import uuid
from unittest.mock import patch

from training_validation_cancel_tests import pipeline, run_case


def hashes(root):
    return {str(path.relative_to(root)): pipeline.sha256(path)
            for path in root.rglob("*") if path.is_file()}


def same_state(left, right):
    import torch
    if isinstance(left, torch.Tensor):
        return isinstance(right, torch.Tensor) and torch.equal(left.cpu(), right.cpu())
    if isinstance(left, dict):
        return isinstance(right, dict) and left.keys() == right.keys() and all(same_state(value, right[key]) for key, value in left.items())
    if isinstance(left, (list, tuple)):
        return isinstance(right, type(left)) and len(left) == len(right) and all(same_state(a, b) for a, b in zip(left, right))
    return left == right


def resume_case(root, mode):
    import torch
    cancelled = run_case(root, mode)
    if not cancelled["passed"]:
        return dict(case=mode, passed=False, cancelled=cancelled)
    source, output = root / "training", root / "resumed"
    dataset = root / "dataset"
    last = source / "run" / "weights" / "last.pt"
    # Never load user/external checkpoints: this file was just created by run_case.
    checkpoint = torch.load(last, map_location="cpu", weights_only=False)
    source_hashes, dataset_hashes = hashes(source), hashes(dataset)
    original = pipeline.read_json(source / "config.json")["job"]
    job = dict(original, resume=True, weights=str(last), trusted_weights_path=str(last),
               expected_weights_sha256=pipeline.sha256(last), output=str(output))
    observations, batch_epochs = [], []
    actual_load = pipeline.load_yolo

    def load(*args, **kwargs):
        model = actual_load(*args, **kwargs)
        if kwargs.get("internal_training_output"):
            return model

        def observe(trainer):
            state = trainer.optimizer.state_dict()
            # Tensor dtypes may be restored from the compact FP16 checkpoint to FP32.
            # torch.equal checks exact element values, so no tolerance hides state loss.
            observations.append(dict(
                start_epoch=trainer.start_epoch, total_epochs=trainer.epochs,
                device=str(trainer.device), workers=trainer.args.workers,
                output_redirected=Path(trainer.save_dir).resolve() == (output / "run").resolve(),
                data_redirected=Path(trainer.args.data).resolve() == (output / "working-dataset/data.yaml").resolve(),
                optimizer_nonempty=bool(state["state"]),
                optimizer_restored=same_state(checkpoint["optimizer"], state),
                ema_restored=same_state(checkpoint["ema"].float().state_dict(), trainer.ema.ema.state_dict()),
                ema_updates_restored=trainer.ema.updates == checkpoint["updates"],
                best_fitness_restored=trainer.best_fitness == checkpoint["best_fitness"],
                historical_best_copied=pipeline.sha256(trainer.best) == pipeline.sha256(last.with_name("best.pt"))))

        model.add_callback("on_train_start", observe)
        model.add_callback("on_train_batch_start", lambda trainer: batch_epochs.append(int(trainer.epoch)))
        return model

    status = root / "resume-status.json"
    with patch.object(pipeline, "load_yolo", load):
        exit_code = pipeline.execute(job, status=status, cancel=root / "resume-cancel.flag")
    final_status = pipeline.read_json(status)
    card = pipeline.read_json(output / "candidate.json") if (output / "candidate.json").is_file() else {}
    unchanged = source_hashes == hashes(source) and dataset_hashes == hashes(dataset)
    resume_from = pipeline.read_json(output / "config.json")["resume_from"] if (output / "config.json").is_file() else None
    good_start = len(observations) == 1 and all(
        value == ({"start_epoch": 1, "total_epochs": 3, "device": "cpu", "workers": 0}.get(key, True))
        for key, value in observations[0].items())
    candidate_valid = (card.get("state") == "CANDIDATE_NOT_ACTIVATED"
                       and card.get("test_evaluation") == "NOT_EXECUTED"
                       and card.get("physical_acceptance") == "NOT_EXECUTED"
                       and card.get("onnx_sha256") == pipeline.sha256(card["onnx"]))
    passed = (exit_code == 0 and final_status["state"] == "SUCCEEDED" and good_start
              and batch_epochs == [1, 2] and unchanged and candidate_valid
              and resume_from == str(source.resolve()) and not (source / "candidate.json").exists())
    return dict(case=mode, passed=passed, cancelled=cancelled, exit_code=exit_code,
                status=final_status, observations=observations, epochs_with_batches=batch_epochs,
                original_files_unchanged=unchanged, candidate_valid=candidate_valid,
                source_hashes=source_hashes, dataset_hashes=dataset_hashes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ("training-resume-" + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    for key, name in {"YOLO_CONFIG_DIR": "yolo-config", "MPLCONFIGDIR": "matplotlib", "TORCH_HOME": "torch"}.items():
        os.environ[key] = str(root / name)
    os.environ.update(YOLO_OFFLINE="true", YOLO_AUTOINSTALL="false", CUDA_VISIBLE_DEVICES="-1",
                      OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
    packages = {name: importlib.metadata.version(name) for name in ("ultralytics", "torch", "onnx")}
    if packages != {"ultralytics": "8.3.203", "torch": "2.8.0+cu128", "onnx": "1.19.0"}:
        raise RuntimeError("Run in the existing pinned training environment; do not install dependencies here")
    results = [resume_case(root / mode, mode) for mode in ("validation", "after_save")]
    report = dict(root=str(root), packages=packages, results=results)
    pipeline.write_json(root / "results.json", report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
