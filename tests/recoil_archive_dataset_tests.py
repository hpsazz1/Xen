"""纯合成C++生产归档到Python Dataset读取的交叉验证，不连接设备。"""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("recoil_dataset_builder", ROOT / "scripts/build_recoil_dataset.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


def check(value, message):
    if not value:
        raise AssertionError(message)


def rejects(action, message):
    try:
        action()
    except (ValueError, KeyError, TypeError):
        return
    raise AssertionError(message)


def main():
    check(len(sys.argv) == 2, "需要recoil_archive_tests可执行文件绝对路径")
    executable = Path(sys.argv[1])
    check(executable.is_absolute() and executable.is_file(), "归档测试程序必须存在")
    with tempfile.TemporaryDirectory(prefix="xen-recoil-archive-cross-") as temporary:
        directory = Path(temporary)
        output = directory / "new-archive"
        command = [str(executable), "--export-test-batch", str(output)]
        result = subprocess.run(command, capture_output=True, timeout=60)
        check(result.returncode == 0, f"C++合成归档失败: {result.stdout!r} {result.stderr!r}")
        files = list(output.glob("*.json"))
        check(len(files) == 1 and not list(output.glob("*.tmp")), "应只原子发布一个完成批次")
        source = files[0]
        data = source.read_bytes()
        builder = BUILDER.Builder(directory)
        root, recoil, profiles, digest = builder.debug(str(source.relative_to(directory)))
        check(root["session_id"] == root["acquisition_run_id"] == "synthetic-cpp-archive-test-only",
              "必须保留C++实际归档的合成采集Run身份")
        check(len(recoil["execution"]["records"]) == 4000 and len(profiles) == 1 and len(digest) == 64,
              "超过2048条的生产序列化结果必须被新schema读取")
        measurement = directory / "synthetic-measurement.txt"
        measurement.write_text("synthetic test only; no game or physical observation", encoding="utf-8")
        entry = {"id": "synthetic-fit", "debug_report": str(source.relative_to(directory)),
                 "source_run": root["session_id"], "firing_id": 1, "completed": True,
                 "independent_recoil": True, "timing_valid": True, "previously_used_generation": 0,
                 "use": "fit", "measurement": {"source": measurement.name, "clock_domain": "local_steady",
                 "at_steady_ns": 6_000_000_000, "residual": [2, -3], "noise": [0.1, 0.1],
                 "timing_uncertainty_ms": 0.5, "reference_confirmed": True}}
        trial, profile = builder.trial(entry, "fit")
        check(trial["executed_counts"] == [0, 4000] and len(trial["receipts"]) == 4000 and
              trial["environment_fingerprint"] == profile["environment_fingerprint"],
              "真实跨语言字段必须通过累计曲线、相位与身份核验")
        duplicate = directory / "copy.json"
        duplicate.write_bytes(data)
        copied = copy.deepcopy(entry)
        copied.update(id="not-a-new-run", debug_report=duplicate.name)
        rejects(lambda: builder.trial(copied, "fit"), "复制文件不能把同一Run当独立样本")
        broken = json.loads(data)
        broken["recoil"]["execution"]["records"][10]["event_sequence"] += 1
        damaged = directory / "damaged.json"
        damaged.write_text(json.dumps(broken), encoding="utf-8")
        rejects(lambda: BUILDER.Builder(directory).debug(damaged.name), "实际归档损坏序号必须拒绝")
        repeated = subprocess.run(command, capture_output=True, timeout=60)
        check(repeated.returncode != 0 and source.read_bytes() == data,
              "再次导出同目录不得覆盖原始归档")
    print("recoil_archive_dataset_tests passed")


if __name__ == "__main__":
    main()
