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
    # 由实际CLI生成两块合法档案，第一块已有可评分前缀；不是只拒绝坏清单。
    many = fixture("two-chunks", [
        {"at_ms": index * 5, "held_mask": {10: 2, 12: 8}.get(index, 0)}
        for index in range(4100)])
    many_report = run(many, root / "two-chunks-report")
    assert many_report["reference"]["assessments"]
    two_chunks = root / "two-chunks-report" / "input-copy"
    assert sorted(p.name for p in two_chunks.glob("events-*.csv")) == ["events-0.csv", "events-1.csv"]
    last_chunk = two_chunks / "events-1.csv"
    original_last = last_chunk.read_bytes()
    for fault in ("bad-last-row", "unfinished-last-line", "missing-last-chunk"):
        if fault == "bad-last-row":
            # 保持文件长度和清单一致，仅最后一条raw_report的十六进制非法。
            last_chunk.write_bytes(original_last[:-2] + b"z\n")
        elif fault == "unfinished-last-line":
            last_chunk.write_bytes(original_last[:-1])
        else:
            last_chunk.unlink()
        before = {p.name: p.read_bytes() for p in two_chunks.iterdir()}
        destination = root / (fault + "-report")
        run(two_chunks, destination, archive=True, ok=False)
        assert not destination.exists(), "late failure must discard the useful prefix"
        assert before == {p.name: p.read_bytes() for p in two_chunks.iterdir()}
        last_chunk.write_bytes(original_last)
        recovered = run(two_chunks, root / (fault + "-recovered"), archive=True)
        assert recovered["reference"] == many_report["reference"]
        assert recovered["xen_input_training"] == many_report["xen_input_training"]
        assert recovered["physical_output"] is False and recovered["game_stop_verified"] is False
        print(f"late-archive {fault}: rejected without output, recovered {recovered['event_count']} events")
    # 完整归档校验失败时禁止部分成功报告。
    archive = root / "example" / "input-copy"
    manifest = archive / "manifest.txt"
    manifest.write_text(manifest.read_text().replace(" 0 262144", " 1 262144"))
    run(archive, root / "truncated-report", archive=True, ok=False)
    assert not (root / "truncated-report").exists()
    # 整数合同必须在缩窄转换前校验；收集全部反例，避免首个失败遮住其他字段。
    bad_integer_cases = []
    for key, values in {
        "held_mask": [2.5, 4294967298, -4294967294, True, "2", 16],
        "epoch": [-1, 1.5, 18446744073709551616, True],
        "sequence": [-1, 2.5, 18446744073709551616, True],
        "timing_uncertainty_ns": [-2, -1.5, 18446744073709551615, 9223372036854775808, False],
    }.items():
        for value in values:
            bad_integer_cases.append(("event", key, value))
    for key, values in {
        "held_mask": [2.5, 4294967298, -4294967294, 16, True],
        "ack_delay_ms": [1.5, 4294967297, 101, 0],
        "cancel_after_ms": [1.5, 4294967295, -4294967297, -2, 501],
    }.items():
        for value in values:
            bad_integer_cases.append(("plan", key, value))
    incorrect = []
    for index, (kind, key, value) in enumerate(bad_integer_cases):
        document = {"schema": 1, "events": [{"at_ms": 0, "held_mask": 0}]}
        if kind == "event":
            document["events"][0][key] = value
        else:
            document["h40_plans"] = [{"held_mask": 2, "ack_delay_ms": 10, "cancel_after_ms": -1}]
            document["h40_plans"][0][key] = value
        source = root / f"invalid-integer-{index}.json"
        source.write_text(json.dumps(document), encoding="utf-8")
        destination = root / f"invalid-integer-{index}-report"
        result = subprocess.run([str(exe), "--events", str(source), "--output", str(destination)],
                                capture_output=True, timeout=15)
        if result.returncode == 0 or destination.exists():
            incorrect.append((kind, key, value, result.returncode, destination.exists()))
    print(f"integer preflight: {len(bad_integer_cases)} cases, {len(incorrect)} incorrect results", flush=True)
    for case in incorrect:
        print("FAIL integer preflight", case, flush=True)
    # 原格式的边界与默认值保持；at_ms仍允许合法小数。
    boundaries = fixture("integer-boundaries", [
        {"at_ms": 0, "held_mask": 0, "epoch": 18446744073709551615,
         "sequence": 18446744073709551614, "timing_uncertainty_ns": -1},
        {"at_ms": 0.25, "held_mask": 15, "epoch": 18446744073709551615,
         "sequence": 18446744073709551615, "timing_uncertainty_ns": 0},
        {"at_ms": 1, "held_mask": 0, "epoch": 0, "sequence": 0,
         "timing_uncertainty_ns": 9223372036854775807}])
    boundary_document = json.loads(boundaries.read_text(encoding="utf-8"))
    boundary_document["h40_plans"] = [
        {"held_mask": 1, "ack_delay_ms": 1, "cancel_after_ms": -1},
        {"held_mask": 4, "ack_delay_ms": 100, "cancel_after_ms": 500},
        {"held_mask": 2, "ack_delay_ms": 10, "cancel_after_ms": 0}]
    boundaries.write_text(json.dumps(boundary_document), encoding="utf-8")
    boundary_report = run(boundaries, root / "integer-boundaries-report")
    assert boundary_report["event_count"] == 3
    boundary_rows = (root / "integer-boundaries-report" / "input-copy" / "events-0.csv").read_text().splitlines()
    assert boundary_rows[1].split(",")[:2] == ["18446744073709551615", "18446744073709551614"]
    assert boundary_rows[-1].split(",")[12] == "9223372036854775807"
    assert not boundary_report["physical_output"] and not boundary_report["game_stop_verified"]
    assert [plan["simulated_ack_delay_ms"] for plan in boundary_report["h40_simulated_plans"]] == [1, 100, 10]
    assert all(not plan["fire_permission"] for plan in boundary_report["h40_simulated_plans"])
    assert not incorrect, "integer input must be rejected before report directory creation"
print("独立比较CLI：同输入回放、歧义传播、精度、断点、假ACK及错误边界通过")
