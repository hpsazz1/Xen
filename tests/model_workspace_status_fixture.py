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
missing_fields = {"missing-ready": "ready", "missing-python-field": "python_executable",
                  "missing-device": "device_requested", "missing-output": "output"}
wrong_fields = {"wrong-python-type": "python_executable", "wrong-device-type": "device_requested",
                "wrong-output-type": "output"}
assert mode in {"missing-python", "not-ready", "bad-type", "healthy", "wait", "hold-success",
                "null-result", "array-result", "missing-result", "old-result", "foreign-operation",
                "wrong-interpreter", "wrong-device", "unrelated-fields"} | missing_fields.keys() | wrong_fields.keys()


def publish(state, result):
    status = Path(args.status)
    pending = status.with_suffix(".pending")
    value = {"schema_version": 1, "operation": "pt_check" if mode == "foreign-operation" else "env_check",
             "state": state, "message": "ENV_CHECK协议fixture", "result": result}
    if mode == "missing-result":
        del value["result"]
    pending.write_text(json.dumps(value), encoding="utf-8")
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

if mode == "hold-success":
    publish("RUNNING", {})
    directory = Path(args.job).parent
    (directory / "started.flag").write_text("fixture-read-request", encoding="utf-8")
    deadline = time.monotonic() + 10
    while not (directory / "release.flag").exists():
        if Path(args.cancel).exists() or time.monotonic() >= deadline:
            raise SystemExit(2)
        time.sleep(0.01)

# 与正式env_check的解释器规范化、output和device_requested字段一致；仅返回协议，不执行解释器。
result = {"ready": True, "python_executable": str(Path(spec["python_executable"]).resolve()),
          "device_requested": spec["device"], "output": spec["output"]}
if mode == "missing-python":
    missing = Path(args.job).parent / "missing-interpreter.exe"
    assert not missing.exists()
    result["python_executable"] = str(missing)
elif mode == "not-ready":
    result["ready"] = False
elif mode == "bad-type":
    result["ready"] = "invalid-boolean"
elif mode in missing_fields:
    del result[missing_fields[mode]]
elif mode in wrong_fields:
    result[wrong_fields[mode]] = ["invalid-string"]
elif mode == "null-result":
    result = None
elif mode == "array-result":
    result = []
elif mode == "old-result":
    result = json.loads((Path(spec["environment_root"]).parent / "old-status.json").read_text(encoding="utf-8"))["result"]
elif mode == "wrong-interpreter":
    result["python_executable"] = str(Path(spec["environment_root"]).parent / "other-interpreter.exe")
elif mode == "wrong-device":
    result["device_requested"] = "unrequested-device"
elif mode == "unrelated-fields":
    result.update(dataset="must-not-apply", review_manifest=["not-an-environment-field"],
                  prelabels="must-not-apply", candidate=["not-a-model-result"])
publish("SUCCEEDED", result)
