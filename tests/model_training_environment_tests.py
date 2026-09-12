"""标准库编排测试；替身不等于真实依赖安装或 GPU 验收。"""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "model_training_environment.py"
spec = importlib.util.spec_from_file_location("model_training_environment", SCRIPT)
environment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(environment)


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / "environments"
        self.output = self.base / "output"
        self.status = self.base / "status.json"
        self.weights = self.base / "trusted.pt"
        self.weights.write_bytes(b"trusted-test-fixture-never-unpickled")
        self.commands = []

    def tearDown(self):
        self.temp.cleanup()

    def job(self, operation, **kwargs):
        return dict(operation=operation, output=str(self.output), environment_root=str(self.root), python_executable=sys.executable, **kwargs)

    def fake_run(self, ctx, arguments, log, stage):
        ctx.check()
        args = [str(value) for value in arguments]
        self.commands.append(args)
        Path(log).write_text("测试日志\n", encoding="utf-8")
        if "venv" in args:
            python = Path(args[-1]) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
            python.parent.mkdir(parents=True)
            python.write_bytes(b"fake-python")
        if "--report" in args:
            environment.write_json(args[args.index("--report")+1], dict(install=[]))
        if "--worker-job" in args:
            worker_job = environment.read_json(args[args.index("--worker-job")+1])
            result_path = args[args.index("--worker-result")+1]
            result = dict(schema_version=1, ready=True, operation=worker_job["operation"], device={"name": "替身 GPU"})
            if worker_job["operation"] == "pt_check":
                imgsz = worker_job.get("imgsz", 320)
                result.update(weights_sha256=environment.sha256(worker_job["weights"]), model_class_names=["person", "head"], synthetic_compatibility_only=True, input_shape=[2, 3, imgsz, imgsz])
            environment.write_json(result_path, result)

    def runner(self):
        owner = self
        return lambda ctx, args, log, stage: owner.fake_run(ctx, args, log, stage)

    def test_install_success_uses_explicit_cuda_source_and_preserves_old_environment(self):
        old = self.root / "old" / "keep.txt"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"old-environment")
        with patch.object(environment.Context, "run", self.runner()):
            code = environment.execute(self.job("env_install"), self.status)
        self.assertEqual(code, 0)
        status = environment.read_json(self.status)
        self.assertEqual(status["state"], "SUCCEEDED")
        self.assertTrue(status["result"]["ready"])
        self.assertTrue(Path(status["result"]["python_executable"]).is_file())
        self.assertEqual(old.read_bytes(), b"old-environment")
        directory = Path(status["result"]["environment_directory"])
        record = environment.read_json(directory / "environment.json")
        self.assertEqual(record["state"], "READY")
        torch_command = next(cmd for cmd in self.commands if environment.CUDA_WHEELS["torch"] in cmd)
        self.assertIn(environment.TORCH_INDEX, torch_command)
        self.assertIn(environment.CUDA_WHEELS["torchvision"], torch_command)
        self.assertEqual(record["sources"]["cuda_wheels"], environment.CUDA_WHEELS)
        for url in environment.CUDA_WHEELS.values():
            self.assertTrue(url.startswith("https://download.pytorch.org/whl/cu128/"))
            self.assertNotIn("download-r2", url)
            self.assertIn("cp312-cp312-win_amd64.whl#sha256=", url)
            self.assertEqual(len(url.split("#sha256=")[1]), 64)
        self.assertTrue(any("--all" in cmd and "freeze" in cmd for cmd in self.commands))
        self.assertTrue((self.output / "torch-install-report.json").is_file())
        self.assertTrue((self.output / "requirements-install-report.json").is_file())

    def test_pip_failure_cannot_publish_ready(self):
        owner = self
        def fail_requirements(ctx, arguments, log, stage):
            if "-r" in [str(arg) for arg in arguments]:
                raise environment.EnvironmentError("安装失败替身")
            owner.fake_run(ctx, arguments, log, stage)
        with patch.object(environment.Context, "run", fail_requirements):
            code = environment.execute(self.job("env_install"), self.status)
        self.assertEqual(code, 1)
        result = environment.read_json(self.status)
        self.assertFalse(result["result"]["ready"])
        self.assertNotIn("python_executable", result["result"])
        record = environment.read_json(next(self.root.glob("*/environment.json")))
        self.assertEqual(record["state"], "FAILED")

    def test_gpu_failure_after_pip_success_cannot_publish_ready(self):
        owner = self
        def fail_gpu(ctx, arguments, log, stage):
            owner.fake_run(ctx, arguments, log, stage)
            args = [str(arg) for arg in arguments]
            if "--worker-result" in args:
                environment.write_json(args[args.index("--worker-result")+1], dict(schema_version=1, operation="env_check", ready=False, error="CUDA unavailable"))
        with patch.object(environment.Context, "run", fail_gpu):
            self.assertEqual(environment.execute(self.job("env_install"), self.status), 1)
        self.assertEqual(environment.read_json(next(self.root.glob("*/environment.json")))["state"], "FAILED")

    def test_cancel_keeps_incomplete_environment_without_ready(self):
        flag = self.base / "cancel.flag"
        owner = self
        def cancelled(ctx, arguments, log, stage):
            owner.fake_run(ctx, arguments, log, stage)
            if environment.CUDA_WHEELS["torch"] in [str(arg) for arg in arguments]:
                flag.touch()
                ctx.check()
        with patch.object(environment.Context, "run", cancelled):
            self.assertEqual(environment.execute(self.job("env_install"), self.status, flag), 2)
        self.assertEqual(environment.read_json(self.status)["state"], "CANCELLED")
        record = environment.read_json(next(self.root.glob("*/environment.json")))
        self.assertEqual(record["state"], "CANCELLED")
        self.assertFalse(record["ready"])

    def test_existing_output_is_never_overwritten(self):
        self.output.mkdir()
        kept = self.output / "keep"
        kept.write_bytes(b"unchanged")
        with patch.object(environment.Context, "run") as run:
            self.assertEqual(environment.execute(self.job("env_install"), self.status), 1)
        run.assert_not_called()
        self.assertEqual(kept.read_bytes(), b"unchanged")

    def test_environment_check_requires_real_worker_ready(self):
        with patch.object(environment.Context, "run", self.runner()):
            self.assertEqual(environment.execute(self.job("env_check"), self.status), 0)
        result = environment.read_json(self.status)["result"]
        self.assertEqual(result["python_executable"], str(Path(sys.executable).resolve()))
        self.assertTrue(result["ready"])

    def test_untrusted_or_changed_pt_never_starts_worker(self):
        for kwargs in (dict(weights=str(self.weights)), dict(weights=str(self.weights), trusted_weights=True, expected_sha256="0"*64)):
            with patch.object(environment.Context, "run") as run:
                self.assertEqual(environment.execute(self.job("pt_check", **kwargs), self.status), 1)
            run.assert_not_called()

    def test_pt_success_is_only_synthetic_compatibility(self):
        original = self.weights.read_bytes()
        with patch.object(environment.Context, "run", self.runner()):
            self.assertEqual(environment.execute(self.job("pt_check", weights=str(self.weights), trusted_weights=True, expected_sha256=environment.sha256(self.weights), imgsz=640), self.status), 0)
        result = environment.read_json(self.status)["result"]
        self.assertEqual(result["model_class_names"], ["person", "head"])
        self.assertTrue(result["synthetic_compatibility_only"])
        self.assertEqual(result["input_size"], 640)
        self.assertEqual(result["device_requested"], "0")
        self.assertEqual(result["python_executable"], str(Path(sys.executable).resolve()))
        self.assertEqual(self.weights.read_bytes(), original)
        self.assertNotIn("candidate", result)

    def test_pt_mutation_during_check_rejected(self):
        owner = self
        def mutate(ctx, arguments, log, stage):
            owner.fake_run(ctx, arguments, log, stage)
            owner.weights.write_bytes(b"changed")
        with patch.object(environment.Context, "run", mutate):
            self.assertEqual(environment.execute(self.job("pt_check", weights=str(self.weights), trusted_weights=True), self.status), 1)
        self.assertFalse(environment.read_json(self.status)["result"]["ready"])

    def test_cancel_terminates_spawned_child(self):
        flag = self.base / "cancel.flag"
        class Process:
            def poll(self):
                return None
        process = Process()
        def spawn(*args, **kwargs):
            flag.touch()
            return process
        ctx = environment.Context(self.job("env_check"), cancel=flag)
        with patch.object(environment.subprocess, "Popen", side_effect=spawn), patch.object(environment, "stop_process") as stop:
            with self.assertRaises(environment.Cancelled):
                ctx.run([sys.executable, "-V"], self.base / "child.log", "检查")
        stop.assert_called_once_with(process)

    def test_private_pip_configuration_is_not_inherited(self):
        with patch.dict(os.environ, {"PIP_INDEX_URL": "https://user:secret@example.invalid", "PIP_EXTRA_INDEX_URL": "https://private.invalid", "PYTHONPATH": "unrelated"}):
            cleaned = environment.clean_environment()
        self.assertNotIn("PIP_INDEX_URL", cleaned)
        self.assertNotIn("PIP_EXTRA_INDEX_URL", cleaned)
        self.assertNotIn("PYTHONPATH", cleaned)
        self.assertEqual(cleaned["PIP_CONFIG_FILE"], os.devnull)

    def test_pt_image_size_validation_happens_before_worker(self):
        for imgsz in (0, 63, 319, 1312, True, "320"):
            with patch.object(environment.Context, "run") as run:
                self.assertEqual(environment.execute(self.job("pt_check", weights=str(self.weights), trusted_weights=True, imgsz=imgsz), self.status), 1)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
