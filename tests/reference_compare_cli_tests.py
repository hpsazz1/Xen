"""独立离线入口的行为回归，不访问设备。"""
import json
import pathlib
import subprocess
import sys
import tempfile

exe, example = map(pathlib.Path, sys.argv[1:3])

with tempfile.TemporaryDirectory(prefix="xen-reference-cli-") as temporary:
    root = pathlib.Path(temporary)

    def run(source, destination, archive=False, ok=True):
        result = subprocess.run(
            [str(exe), "--archive" if archive else "--events", str(source),
             "--output", str(destination)], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=15)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        return json.loads((destination / "report.json").read_text(encoding="utf-8")) if ok else None

    report = run(example, root / "example")
    unicode_report = run(example, root / "中文 报告")
    assert unicode_report["reference"] == report["reference"]
    assert report["physical_output"] is False and report["game_stop_verified"] is False
    assessments = report["reference"]["assessments"]
    assert [row["delta_ms"] for row in assessments] == [5, -15, 0]
    assert assessments[-1]["packet_order_assumed"]
    assert any(t["atomic_ambiguous"] for t in report["xen_input_training"]["timings"])
    assert len(report["reference"]["discontinuities"]) == 1
    assert not any(2500 <= shot["at_ms"] < 2700 for shot in report["reference"]["shots"])
    assert all(not plan["fire_permission"] for plan in report["h40_simulated_plans"])
    assert report["h40_simulated_plans"][0]["ready_after_request_ms"] == 61
    assert report["h40_simulated_plans"][2]["trace"][-1]["cancelled"]
    replay = run(root / "example" / "input-copy", root / "replay", archive=True)
    assert replay["reference"] == report["reference"]
    assert replay["xen_input_training"] == report["xen_input_training"]
    assert not replay["modifiers_available"]
    run(example, root / "example", ok=False)  # 已有报告不得覆盖。

    def fixture(name, events):
        path = root / (name + ".json")
        path.write_text(json.dumps({"schema": 1, "events": events}), encoding="utf-8")
        return path

    atomic = fixture("atomic", [
        {"at_ms": 0, "held_mask": 0},
        {"at_ms": 100, "held_mask": 10},
        {"at_ms": 200, "held_mask": 10, "left_down": True},
        {"at_ms": 250, "held_mask": 10, "left_down": True},
        {"at_ms": 300, "held_mask": 0}])
    atomic_report = run(atomic, root / "atomic-report")
    assert all(s["packet_order_assumed"] for s in atomic_report["reference"]["shots"])
    fractional = fixture("fractional", [
        {"at_ms": 0, "held_mask": 0}, {"at_ms": 100, "held_mask": 2},
        {"at_ms": 500, "held_mask": 0}, {"at_ms": 502.1, "held_mask": 8}])
    fraction_report = run(fractional, root / "fraction-report")
    assert fraction_report["reference"]["assessments"][0]["delta_ms"] == 2.1
    assert not fraction_report["reference"]["assessments"][0]["perfect"]
    held = fixture("missing-start", [
        {"at_ms": 0, "held_mask": 2, "left_down": True},
        {"at_ms": 500, "held_mask": 8, "left_down": True}])
    assert not run(held, root / "held-report")["reference"]["shots"]
    backwards = fixture("backwards", [{"at_ms": 10, "held_mask": 0}, {"at_ms": 9, "held_mask": 0}])
    run(backwards, root / "backwards-report", ok=False)
    assert not (root / "backwards-report").exists()
    empty = fixture("empty", [])
    run(empty, root / "empty-report", ok=False)
    # 完整归档校验失败时禁止部分成功报告。
    archive = root / "example" / "input-copy"
    manifest = archive / "manifest.txt"
    manifest.write_text(manifest.read_text().replace(" 0 262144", " 1 262144"))
    run(archive, root / "truncated-report", archive=True, ok=False)
    assert not (root / "truncated-report").exists()
print("独立比较CLI：同输入回放、歧义传播、精度、断点、假ACK及错误边界通过")
