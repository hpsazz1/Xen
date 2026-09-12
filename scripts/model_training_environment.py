"""Xen 独立训练环境配置与兼容检查；基础入口只使用标准库。

普通入口：--job job.json --status status.json --cancel cancel.flag
环境创建不覆盖旧版本；只有 GPU 自检完整成功才返回 ready。
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
import uuid


SCHEMA = 1
VERSIONS = {"torch": "2.8.0", "torchvision": "0.23.0", "ultralytics": "8.3.203", "onnx": "1.19.0", "onnxruntime": "1.22.1", "Pillow": "11.3.0"}
TORCH_INDEX = "https://download.pytorch.org/whl/cu128"
PYPI_INDEX = "https://pypi.org/simple"
CUDA_WHEELS = {
    "torch": "https://download.pytorch.org/whl/cu128/torch-2.8.0%2Bcu128-cp312-cp312-win_amd64.whl#sha256=0ad925202387f4e7314302a1b4f8860fa824357f9b1466d7992bf276370ebcff",
    "torchvision": "https://download.pytorch.org/whl/cu128/torchvision-0.23.0%2Bcu128-cp312-cp312-win_amd64.whl#sha256=20fa9c7362a006776630b00b8a01919fedcf504a202b81358d32c5aef39956fe",
}


class EnvironmentError(Exception):
    pass


class Cancelled(EnvironmentError):
    pass


def now():
    return datetime.now(timezone.utc).isoformat()


def read_json(path):
    with Path(path).open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def local_path(value, *, file=False):
    if not isinstance(value, str) or not value.strip():
        raise EnvironmentError("必须填写本地绝对路径")
    original = Path(value)
    if not original.is_absolute():
        raise EnvironmentError("路径必须为绝对路径")
    for component in (original, *original.parents):
        if component.is_symlink() or (hasattr(component, "is_junction") and component.is_junction()):
            raise EnvironmentError("训练环境路径不接受链接或重解析目录")
    path = original.resolve()
    if file and not path.is_file():
        raise EnvironmentError(f"本地文件不存在：{path}")
    return path


def clean_environment():
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PIP_") and key.upper() not in {"PYTHONPATH", "PYTHONHOME"}}
    # 不读取用户 pip 配置，避免用户私有源或凭据进入依赖记录。
    environment.update(PIP_CONFIG_FILE=os.devnull, PIP_DISABLE_PIP_VERSION_CHECK="1", PYTHONNOUSERSITE="1", PYTHONUTF8="1", YOLO_AUTOINSTALL="false", YOLO_OFFLINE="true", WANDB_MODE="disabled")
    return environment


def stop_process(process):
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW, timeout=10, check=False)
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=5)


class Context:
    def __init__(self, job, status=None, cancel=None):
        self.job = job
        self.status = Path(status) if status else None
        self.cancel = Path(cancel) if cancel else None

    def check(self):
        if self.cancel and self.cancel.exists():
            raise Cancelled("已取消；旧环境和原始权重未修改，未就绪的新环境保留用于排查")

    def report(self, state, message, result=None):
        if self.status:
            write_json(self.status, dict(schema_version=SCHEMA, state=state, operation=self.job.get("operation"), message=message, result=result or {"ready": False}, updated_at=now()))

    def output(self):
        output = local_path(self.job.get("output"))
        if output.exists():
            raise EnvironmentError("output 已存在，请使用新的独立作业目录")
        output.mkdir(parents=True, exist_ok=False)
        return output

    def run(self, arguments, log, stage):
        self.check()
        log = Path(log)
        self.report("RUNNING", stage, dict(ready=False, log=str(log)))
        options = dict(env=clean_environment(), cwd=str(log.parent), stdin=subprocess.DEVNULL)
        if os.name == "nt":
            options["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options["start_new_session"] = True
        with log.open("xb") as stream:
            process = subprocess.Popen([str(value) for value in arguments], stdout=stream, stderr=subprocess.STDOUT, **options)
            last_update = time.monotonic()
            try:
                while process.poll() is None:
                    self.check()
                    if time.monotonic() - last_update >= 1:
                        self.report("RUNNING", stage, dict(ready=False, log=str(log)))
                        last_update = time.monotonic()
                    time.sleep(0.2)
                self.check()
                if process.returncode != 0:
                    raise EnvironmentError(f"{stage}失败（退出码 {process.returncode}）；查看 {log.name}")
            except BaseException:
                stop_process(process)
                raise


def interpreter(value):
    path = local_path(value, file=True)
    if os.name == "nt" and path.suffix.lower() != ".exe":
        raise EnvironmentError("Python 解释器必须为本地 .exe 文件")
    return path


def worker_check(ctx, python, output, operation, extra=None):
    specification = dict(operation=operation, **(extra or {}))
    job_path = output / f"{operation}-worker.json"
    result_path = output / f"{operation}-report.json"
    write_json(job_path, specification)
    ctx.run([python, "-B", "-X", "utf8", Path(__file__).resolve(), "--worker-job", job_path, "--worker-result", result_path], output / f"{operation}-worker.log", "检查 GPU 训练环境" if operation == "env_check" else "检查用户权重前向与反向传播")
    report = read_json(result_path)
    if report.get("schema_version") != SCHEMA or report.get("operation") != operation or report.get("ready") is not True:
        raise EnvironmentError("自检没有返回完整就绪报告")
    ctx.check()
    return report, result_path


def env_install(ctx):
    if os.name != "nt" or sys.version_info[:2] != (3, 12) or struct.calcsize("P") != 8 or platform.machine().lower() not in {"amd64", "x86_64"}:
        raise EnvironmentError("环境配置入口需要 Windows 基础 Python 3.12 x64")
    root = local_path(ctx.job.get("environment_root"))
    requirements = Path(__file__).resolve().with_name("model_training_requirements.txt")
    if not requirements.is_file():
        raise EnvironmentError("脚本同目录缺少 model_training_requirements.txt")
    output = ctx.output()
    root.mkdir(parents=True, exist_ok=True)
    identity = "py312-torch280-cu128-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:10]
    directory = root / identity
    directory.mkdir(exist_ok=False)
    environment_record = dict(schema_version=SCHEMA, state="INSTALLING", ready=False, created_at=now(), environment_directory=str(directory), base_python=str(Path(sys.executable).resolve()), base_version=platform.python_version(), sources=dict(torch=TORCH_INDEX, packages=PYPI_INDEX, cuda_wheels=CUDA_WHEELS), requirements_sha256=sha256(requirements), versions_requested=VERSIONS)
    write_json(directory / "environment.json", environment_record)
    try:
        venv = directory / "venv"
        ctx.run([sys.executable, "-m", "venv", str(venv)], output / "create-venv.log", "创建独立 Python 环境")
        python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if not python.is_file():
            raise EnvironmentError("新环境缺少 Python 解释器")
        # 固定官方主域与索引给出的 SHA，避免索引镜像域不可用时反复下载失败。
        # pip 校验 URL 的 sha256 fragment；其余依赖仍只从官方 CUDA 索引解析。
        ctx.run([python, "-m", "pip", "install", "--no-input", "--index-url", TORCH_INDEX, "--report", output / "torch-install-report.json", CUDA_WHEELS["torch"], CUDA_WHEELS["torchvision"]], output / "torch-install.log", "安装 PyTorch CUDA 12.8 与 torchvision")
        ctx.run([python, "-m", "pip", "install", "--no-input", "--index-url", PYPI_INDEX, "--report", output / "requirements-install-report.json", "-r", requirements], output / "requirements-install.log", "安装固定训练工具依赖")
        ctx.run([python, "-m", "pip", "check"], output / "pip-check.log", "核对依赖一致性")
        ctx.run([python, "-m", "pip", "freeze", "--all"], output / "pip-freeze.txt", "记录完整依赖版本")
        report, report_path = worker_check(ctx, python, output, "env_check", dict(device=ctx.job.get("device", "0")))
        for filename in ("torch-install-report.json", "requirements-install-report.json"):
            if not (output / filename).is_file():
                raise EnvironmentError("pip 未生成依赖来源报告")
        ctx.check()
        environment_record.update(state="READY", ready=True, python_executable=str(python), environment_report=str(report_path), installation_output=str(output), completed_at=now(), report=report)
        write_json(directory / "environment.json", environment_record)
        return dict(ready=True, output=str(output), environment_directory=str(directory), python_executable=str(python), environment_report=str(report_path), device_requested=str(ctx.job.get("device", "0")))
    except BaseException as exc:
        environment_record.update(state="CANCELLED" if isinstance(exc, Cancelled) else "FAILED", ready=False, completed_at=now())
        write_json(directory / "environment.json", environment_record)
        raise


def env_check(ctx):
    python = interpreter(ctx.job.get("python_executable"))
    output = ctx.output()
    ctx.run([python, "-m", "pip", "check"], output / "pip-check.log", "核对依赖一致性")
    report, report_path = worker_check(ctx, python, output, "env_check", dict(device=ctx.job.get("device", "0")))
    return dict(ready=True, output=str(output), python_executable=str(python), environment_report=str(report_path), device=report.get("device"), device_requested=str(ctx.job.get("device", "0")))


def trusted_weights(job):
    if job.get("trusted_weights") is not True:
        raise EnvironmentError("请先明确确认该 PT 权重来自可信来源；未确认时不会加载")
    path = local_path(job.get("weights"), file=True)
    if path.suffix.lower() != ".pt":
        raise EnvironmentError("权重检查只接受本地 .pt 文件")
    digest = sha256(path)
    expected = job.get("expected_sha256")
    if expected and (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected) or digest != expected.lower()):
        raise EnvironmentError("权重 SHA-256 与用户指定身份不一致")
    return path, digest


def pt_check(ctx):
    weights, digest = trusted_weights(ctx.job)
    python = interpreter(ctx.job.get("python_executable"))
    imgsz = image_size(ctx.job.get("imgsz", 320))
    output = ctx.output()
    report, report_path = worker_check(ctx, python, output, "pt_check", dict(trusted_weights=True, weights=str(weights), expected_sha256=digest, device=ctx.job.get("device", "0"), imgsz=imgsz))
    if sha256(weights) != digest or report.get("weights_sha256") != digest or report.get("synthetic_compatibility_only") is not True or report.get("input_shape") != [2, 3, imgsz, imgsz]:
        raise EnvironmentError("权重检查后身份变化或结果契约不完整")
    return dict(ready=True, output=str(output), weights=str(weights), weights_sha256=digest, model_class_names=report["model_class_names"], compatibility_report=str(report_path), synthetic_compatibility_only=True, task="detect", input_size=imgsz, python_executable=str(python), device_requested=str(ctx.job.get("device", "0")))


def image_size(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 64 <= value <= 1280 or value % 32:
        raise EnvironmentError("权重检查 imgsz 必须为 64..1280 的 32 倍数")
    return value


def package_versions():
    result = {}
    for name, expected in VERSIONS.items():
        try:
            actual = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError as exc:
            raise EnvironmentError(f"DEPENDENCY_MISSING：缺少 {name}") from exc
        if actual.split("+")[0] != expected:
            raise EnvironmentError(f"DEPENDENCY_VERSION：{name} 要求 {expected}，实际 {actual}")
        result[name] = actual
    return result


def cuda_device(torch, value):
    value = str(value)
    if not re.fullmatch(r"\d+", value):
        raise EnvironmentError("GPU 自检需要单个 CUDA 设备编号，例如 0")
    index = int(value)
    if not torch.cuda.is_available() or index >= torch.cuda.device_count():
        raise EnvironmentError("CUDA 不可用或设备编号不存在")
    if torch.version.cuda != "12.8":
        raise EnvironmentError(f"要求 PyTorch CUDA 12.8 构建，实际为 {torch.version.cuda}")
    torch.cuda.set_device(index)
    return index, dict(index=index, name=torch.cuda.get_device_name(index), compute_capability=list(torch.cuda.get_device_capability(index)), compiled_architectures=torch.cuda.get_arch_list(), cuda_runtime=torch.version.cuda, cudnn_version=torch.backends.cudnn.version())


def gpu_environment_worker(job):
    if sys.version_info[:2] != (3, 12) or struct.calcsize("P") != 8:
        raise EnvironmentError("目标环境需要 Python 3.12 x64")
    packages = package_versions()
    import torch
    import torchvision
    import ultralytics  # noqa: F401
    import onnx  # noqa: F401
    import onnxruntime  # noqa: F401
    import PIL  # noqa: F401
    index, device = cuda_device(torch, job.get("device", "0"))
    torch.manual_seed(0)
    layer = torch.nn.Conv2d(3, 8, 3, padding=1).to(f"cuda:{index}")
    images = torch.randn(2, 3, 32, 32, device=f"cuda:{index}", requires_grad=True)
    output = layer(images)
    loss = output.square().mean()
    loss.backward()
    torch.cuda.synchronize(index)
    if not bool(torch.isfinite(output).all()) or not bool(torch.isfinite(loss)) or layer.weight.grad is None or not bool(torch.isfinite(layer.weight.grad).all()) or float(layer.weight.grad.abs().max()) <= 0:
        raise EnvironmentError("CUDA 卷积前向或反向梯度未通过有限非零检查")
    boxes = torch.tensor([[0, 0, 10, 10], [1, 1, 9, 9]], dtype=torch.float32, device=f"cuda:{index}")
    scores = torch.tensor([0.9, 0.8], device=f"cuda:{index}")
    retained = torchvision.ops.nms(boxes, scores, 0.5)
    torch.cuda.synchronize(index)
    if retained.numel() != 1:
        raise EnvironmentError("torchvision CUDA NMS 未通过检查")
    return dict(schema_version=SCHEMA, operation="env_check", ready=True, checked_at=now(), python_version=platform.python_version(), python_executable=str(Path(sys.executable).resolve()), packages=packages, device=device, checks=dict(cuda_convolution_forward="PASSED", cuda_convolution_backward="PASSED", torchvision_cuda_nms="PASSED"), actual_model_training="NOT_EXECUTED")


def names_checked(names):
    if isinstance(names, dict):
        if set(names) != set(range(len(names))):
            raise EnvironmentError("模型类别 ID 不连续")
        names = [names[index] for index in range(len(names))]
    if not isinstance(names, list) or not names or len(set(names)) != len(names) or any(not isinstance(name, str) or not name.strip() for name in names):
        raise EnvironmentError("模型类别名称不完整")
    return names


def pt_compatibility_worker(job):
    weights, digest = trusted_weights(job)
    imgsz = image_size(job.get("imgsz", 320))
    packages = package_versions()
    import torch
    from ultralytics import YOLO
    from ultralytics.utils import DEFAULT_CFG_DICT, callbacks
    callbacks.add_integration_callbacks = lambda _instance: None
    index, device = cuda_device(torch, job.get("device", "0"))
    # 用户已明确确认来源且哈希再次核对；只通过固定版本的模型入口加载本地文件。
    model = YOLO(str(weights), task="detect")
    if model.task != "detect":
        raise EnvironmentError("权重不是二维目标检测模型")
    names = names_checked(model.names)
    network = model.model
    if getattr(network, "end2end", False) or getattr(network.model[-1], "end2end", False):
        raise EnvironmentError("当前 Xen 候选导出不支持 end-to-end 检测头")
    if getattr(network.model[-1], "nc", None) != len(names):
        raise EnvironmentError("检测头类别数与 names 不一致")
    arguments = network.args if isinstance(network.args, dict) else vars(network.args)
    network.args = SimpleNamespace(**dict(DEFAULT_CFG_DICT, **arguments))
    network.to(f"cuda:{index}").float().train()
    # 旧 checkpoint 可能携带旧版本 criterion；由当前固定训练器按同一模型重建。
    network.criterion = None
    for name, parameter in network.named_parameters():
        parameter.requires_grad_(not name.endswith("dfl.conv.weight"))
    network.zero_grad(set_to_none=True)
    torch.manual_seed(0)
    images = torch.rand(2, 3, imgsz, imgsz, device=f"cuda:{index}")
    predictions = network(images)

    def tensors(value):
        if torch.is_tensor(value):
            yield value
        elif isinstance(value, (tuple, list)):
            for child in value:
                yield from tensors(child)
        elif isinstance(value, dict):
            for child in value.values():
                yield from tensors(child)
    forward_tensors = list(tensors(predictions))
    if not forward_tensors or not all(bool(torch.isfinite(value).all()) for value in forward_tensors):
        raise EnvironmentError("模型前向输出缺失或出现非有限值")
    batch = dict(img=images, batch_idx=torch.tensor([0, 1], device=f"cuda:{index}"), cls=torch.tensor([[0], [len(names)-1]], dtype=torch.float32, device=f"cuda:{index}"), bboxes=torch.tensor([[0.5, 0.5, 0.25, 0.4], [0.4, 0.45, 0.2, 0.35]], device=f"cuda:{index}"))
    losses = network.loss(batch, preds=predictions)
    loss = (losses[0] if isinstance(losses, tuple) else losses).sum()
    if not bool(torch.isfinite(loss)):
        raise EnvironmentError("检测 loss 出现非有限值")
    loss.backward()
    torch.cuda.synchronize(index)
    gradients = [parameter.grad for parameter in network.parameters() if parameter.requires_grad and parameter.grad is not None]
    if not gradients or not all(bool(torch.isfinite(gradient).all()) for gradient in gradients):
        raise EnvironmentError("模型梯度缺失或出现非有限值")
    maximum = max(float(gradient.abs().max()) for gradient in gradients)
    if not math.isfinite(maximum) or maximum <= 0:
        raise EnvironmentError("模型未产生非零有限梯度")
    if sha256(weights) != digest:
        raise EnvironmentError("原始权重在检查期间发生变化")
    return dict(schema_version=SCHEMA, operation="pt_check", ready=True, checked_at=now(), weights=str(weights), weights_sha256=digest, model_class_names=names, task="detect", input_shape=[2, 3, imgsz, imgsz], input_size=imgsz, python_executable=str(Path(sys.executable).resolve()), device_requested=str(job.get("device", "0")), packages=packages, device=device, loss=float(loss.detach().cpu()), gradient_tensors=len(gradients), gradient_max_abs=maximum, synthetic_compatibility_only=True, optimizer_step="NOT_EXECUTED", weights_saved="NOT_EXECUTED", real_dataset_training="NOT_EXECUTED", model_quality="NOT_EVALUATED")


def worker_main(job_path, result_path):
    job = read_json(job_path)
    os.environ.update(YOLO_AUTOINSTALL="false", YOLO_OFFLINE="true", WANDB_MODE="disabled")
    try:
        operation = job.get("operation")
        if operation == "env_check":
            result = gpu_environment_worker(job)
        elif operation == "pt_check":
            result = pt_compatibility_worker(job)
        else:
            raise EnvironmentError("未知内部检查操作")
        write_json(result_path, result)
        return 0
    except Exception as exc:
        write_json(result_path, dict(schema_version=SCHEMA, operation=job.get("operation"), ready=False, error=f"{type(exc).__name__}: {exc}", checked_at=now()))
        print(f"自检失败：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


OPERATIONS = dict(env_install=env_install, env_check=env_check, pt_check=pt_check)


def execute(job, status=None, cancel=None):
    ctx = Context(job, status, cancel)
    try:
        ctx.check()
        operation = job.get("operation")
        if operation not in OPERATIONS:
            raise EnvironmentError("未知环境作业 operation")
        ctx.report("RUNNING", "正在检查本地环境作业输入")
        result = OPERATIONS[operation](ctx)
        ctx.check()
        ctx.report("SUCCEEDED", "环境／兼容检查完成；不代表真实数据训练或模型质量通过", result)
        return 0
    except Cancelled as exc:
        ctx.report("CANCELLED", str(exc), dict(ready=False))
        return 2
    except Exception as exc:
        ctx.report("FAILED", f"{type(exc).__name__}: {exc}", dict(ready=False))
        print(f"环境作业失败：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job")
    parser.add_argument("--status")
    parser.add_argument("--cancel")
    parser.add_argument("--worker-job")
    parser.add_argument("--worker-result")
    args = parser.parse_args()
    if args.worker_job and args.worker_result:
        return worker_main(args.worker_job, args.worker_result)
    if not args.job or not args.status:
        parser.error("需要 --job 与 --status")
    try:
        job = read_json(args.job)
    except Exception as exc:
        write_json(args.status, dict(schema_version=SCHEMA, state="FAILED", ready=False, message=f"作业 JSON 无法读取：{exc}", updated_at=now()))
        return 1
    return execute(job, args.status, args.cancel)


if __name__ == "__main__":
    raise SystemExit(main())
