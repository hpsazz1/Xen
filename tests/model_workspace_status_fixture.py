"""仅ENV_CHECK状态协议夹具；不安装环境、不导入训练框架或模型。"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--job", required=True)
parser.add_argument("--status", required=True)
parser.add_argument("--cancel", required=True)
args = parser.parse_args()
spec = json.loads(Path(args.job).read_text(encoding="utf-8"))
assert spec["operation"] == "env_check"
mode = Path(spec["environment_root"]).name
assert mode in {"missing-python", "not-ready", "bad-type", "healthy", "wait"}


def publish(state, result):
    status = Path(args.status)
    pending = status.with_suffix(".pending")
    pending.write_text(json.dumps({"schema_version": 1, "operation": "env_check",
                                  "state": state, "message": "ENV_CHECK协议fixture",
                                  "result": result}), encoding="utf-8")
    os.replace(pending, status)


if mode == "wait":
    publish("RUNNING", {})
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if Path(args.cancel).exists():
            publish("CANCELLED", {})
            raise SystemExit(2)
        time.sleep(0.01)
    raise SystemExit("fixture未收到取消")

result = {"ready": True, "python_executable": sys.executable,
          "device_requested": spec["device"]}
if mode == "missing-python":
    missing = Path(args.job).parent / "missing-interpreter.exe"
    assert not missing.exists()
    result["python_executable"] = str(missing)
elif mode == "not-ready":
    result["ready"] = False
elif mode == "bad-type":
    result["ready"] = "invalid-boolean"
publish("SUCCEEDED", result)
