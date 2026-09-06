#!/usr/bin/env python3
"""验证 composite-phase plan seed/final freeze 的公开文件合同。"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode("utf-8")).hexdigest()


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(
        value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")


def load_fixture_module(path: pathlib.Path) -> Any:
    spec = importlib.util.spec_from_file_location("binder_fixture", path)
    if spec is None or spec.loader is None:
        raise AssertionError("无法加载 binder fixture")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_scheduler_raw_counts(diagnostic: dict[str, Any]) -> None:
    """用原始整数计数独立重算记录，不启动进程或重复采样。"""
    frequency = diagnostic["qpc_frequency_hz"]
    policy = diagnostic["timing_policy"]

    def ns_to_ticks(nanoseconds: int) -> int:
        return (nanoseconds * frequency + 999_999_999) // 1_000_000_000

    def ticks_to_ns(ticks: int) -> int:
        return 0 if ticks <= 0 else (
            ticks * 1_000_000_000 + frequency - 1) // frequency

    active_total = 0
    for event in diagnostic["events"]:
        ordinal = event["event_ordinal"]
        deadline = event["deadline_qpc"]
        coarse = event["coarse_target_qpc"]
        due_base = event["due_base_qpc"]
        due = event["relative_due_100ns"]
        if deadline is not None:
            expect(deadline == diagnostic["anchor_qpc"] +
                   (ordinal + 1) * ns_to_ticks(policy["preflight_interval_ns"]),
                   "记录的 deadline 必须来自原始 anchor 与 ordinal")
        if coarse is not None:
            expect(coarse == deadline - ns_to_ticks(policy["active_guard_ns"]),
                   "记录的 coarse target 必须保持冻结 guard")
        if due is not None:
            expect(due_base < coarse and
                   due == -((ticks_to_ns(coarse - due_base) + 99) // 100),
                   "记录的 relative due 必须与原始差值及双层向上取整一致")
        expect(event["wait_return_qpc"] == event["active_enter_qpc"],
               "Wait 后第一次 QPC 必须复用为 active enter")
        ordered = [event[field] for field in (
            "due_base_qpc", "set_timer_before_qpc", "set_timer_after_qpc",
            "wait_return_qpc", "active_last_qpc", "marker_before_qpc",
            "marker_after_qpc") if event[field] is not None]
        expect(ordered == sorted(ordered), "已取得的同线程 QPC 不得倒退")
        before = event["marker_before_qpc"]
        after = event["marker_after_qpc"]
        if event["deadline_lateness_ns"] is not None:
            expect(event["deadline_lateness_ns"] == ticks_to_ns(before - deadline),
                   "lateness 必须由保存的 marker/deadline 原始计数重算")
        if event["marker_width_ns"] is not None:
            expect(event["marker_width_ns"] == ticks_to_ns(after - before),
                   "marker width 必须由保存的 bracket 原始计数重算")
        if event["active_wait_ns"] is not None:
            active_end = before if before is not None else event["active_last_qpc"]
            expect(event["active_wait_ns"] == ticks_to_ns(
                active_end - event["active_enter_qpc"]),
                "active wait 必须由有效终点和同次 active enter 重算")
        expect(event["active_total_before_ns"] == active_total,
               "累计 active 仅包含先前已完成事件")
        if event["completed"]:
            active_total += event["active_wait_ns"]


def test_scheduler_diagnostic(seal_executable: pathlib.Path,
                              root: pathlib.Path) -> pathlib.Path:
    """只进行一次有界诊断；其余负例必须在进入计时内核前拒绝。"""
    diagnostic_root = root / "scheduler diagnostic 中文"
    diagnostic_root.mkdir()
    diagnostic_path = diagnostic_root / "diagnostic.json"
    extra_path = diagnostic_root / "not-a-plan.json"
    invalid_commands = [
        ["--diagnose-scheduler"],
        ["--diagnose-scheduler", "relative-diagnostic.json"],
        ["--diagnose-scheduler", str(diagnostic_path),
         "--plan-output", str(extra_path)],
        ["--plan-seed", str(extra_path),
         "--diagnose-scheduler", str(diagnostic_path)],
        ["--diagnose-scheduler", str(diagnostic_path),
         "--diagnose-scheduler", str(diagnostic_path)],
        ["--diagnose-scheduler", str(diagnostic_path),
         "--verify-report", str(extra_path)],
        ["--report-semantic-sha256", str(extra_path),
         "--diagnose-scheduler", str(diagnostic_path)],
        ["--help", "--diagnose-scheduler", str(diagnostic_path)],
    ]
    for arguments in invalid_commands:
        rejected = subprocess.run(
            [str(seal_executable), *arguments], cwd=diagnostic_root,
            check=False, capture_output=True, text=True, encoding="utf-8")
        expect(rejected.returncode == 2 and
               not list(diagnostic_root.iterdir()),
               "诊断必须在计时前拒绝相对路径、混合模式和重复参数")

    measured = subprocess.run([
        str(seal_executable), "--diagnose-scheduler", str(diagnostic_path),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    expect(measured.returncode in (0, 3) and diagnostic_path.is_file(),
           "独立诊断必须对本次完整测量或拒绝都保存原始阶段证据："
           f"exit={measured.returncode}; stderr={measured.stderr}")
    diagnostic_bytes = diagnostic_path.read_bytes()
    diagnostic = json.loads(diagnostic_bytes)
    validate_scheduler_raw_counts(diagnostic)
    expect(diagnostic["evidence_type"] ==
               "mouse_effect_probe_scheduler_phase_diagnostic" and
           diagnostic["diagnostic_only"] is True and
           diagnostic["instrumented"] is True and
           diagnostic["physical_output_capability"] is False and
           diagnostic["physical_dispatch_count"] == 0 and
           diagnostic["formal_preflight_published"] is False and
           diagnostic["final_plan_published"] is False and
           diagnostic["status"] != "PASS" and
           "run_uuid" not in diagnostic and
           "preflight_semantic_sha256" not in diagnostic and
           "plan_semantic_sha256" not in diagnostic,
           "阶段诊断必须独立于可消费的正式 preflight/plan 合同")
    context = diagnostic["context"]
    expect(context["process_id"] > 0 and context["thread_id"] > 0 and
           isinstance(context["session_id"], int) and
           context["session_id"] >= 0 and
           context["process_priority_class"] > 0 and
           isinstance(context["thread_priority"], int) and
           diagnostic["qpc_frequency_hz"] > 0,
           "诊断必须记录实际进程、线程、session、优先级及 QPC frequency")
    policy = diagnostic["timing_policy"]
    expect(policy == {
        "event_capacity": 42, "preflight_interval_ns": 5_000_000,
        "active_guard_ns": 300_000, "max_wake_lateness_ns": 150_000,
        "max_event_interval_width_ns": 100_000,
        "max_active_wait_ns_per_event": 350_000,
        "max_active_wait_ns_total": 14_700_000,
        "timer_mode": "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL",
    }, "独立诊断不得更改冻结计时常量或 timer 模式")
    events = diagnostic["events"]
    expect(len(events) == diagnostic["reached_event_count"] and
           0 <= len(events) <= 42,
           "诊断只保存固定容量内实际到达的事件")
    raw_fields = [
        "deadline_qpc", "coarse_target_qpc", "due_base_qpc",
        "relative_due_100ns", "set_timer_before_qpc",
        "set_timer_after_qpc", "wait_return_qpc", "active_last_qpc",
        "marker_before_qpc", "marker_after_qpc", "active_read_count",
        "deadline_lateness_ns", "marker_width_ns", "active_wait_ns",
    ]
    for ordinal, event in enumerate(events):
        expect(event["event_ordinal"] == ordinal,
               "诊断事件 ordinal 必须连续且从零开始")
        for field in raw_fields:
            expect((event[field] is not None) == event["valid"][field],
                   f"未到达阶段必须显式为空而非伪造零值：{field}")
        if event["relative_due_100ns"] is not None:
            expect(event["relative_due_100ns"] < 0,
                   "one-shot 相对 due 必须保留原始负 100ns 单位")
        if event["completed"]:
            expect(all(event["valid"].values()) and
                   event["deadline_lateness_ns"] <= 150_000 and
                   event["marker_width_ns"] <= 100_000 and
                   event["active_wait_ns"] <= 350_000,
                   "已完成事件必须保留全部阶段且满足原计时预算")
    completed = sum(event["completed"] for event in events)
    expect(completed == diagnostic["completed_event_count"],
           "完整事件计数必须来自保存的原始事件")
    if measured.returncode == 0:
        expect(diagnostic["status"] == "MEASURED_WITHIN_BUDGETS" and
               completed == 42 and diagnostic["failure_event"] is None and
               diagnostic["failure_reason"] == "",
               "退出零仅表示本次有界诊断测量满足预算")
    else:
        expect(diagnostic["status"] == "REJECTED" and
               bool(diagnostic["failure_reason"]) and completed < 42 and
               (diagnostic["failure_event"] is None or
                diagnostic["failure_event"] == len(events) - 1),
               "拒绝必须保留失败原因与实际到达的失败事件")
    expect(list(diagnostic_root.iterdir()) == [diagnostic_path],
           "诊断不得发布正式 preflight、final plan 或遗留 pending")
    collided = subprocess.run([
        str(seal_executable), "--diagnose-scheduler", str(diagnostic_path),
    ], check=False, capture_output=True, text=True, encoding="utf-8")
    expect(collided.returncode == 2 and
           diagnostic_path.read_bytes() == diagnostic_bytes,
           "诊断必须在计时前拒绝覆盖既有结果")
    print("独立阶段诊断合同通过：" + diagnostic["status"], flush=True)
    print("SCHEDULER_DIAGNOSTIC_JSON=" + json.dumps(
        diagnostic, ensure_ascii=False, separators=(",", ":")), flush=True)
    return diagnostic_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True, type=pathlib.Path)
    parser.add_argument("--sequence-executable", required=True,
                        type=pathlib.Path)
    parser.add_argument("--binder", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--seal-executable", type=pathlib.Path)
    options = parser.parse_args()
    generator = options.generator.resolve()
    sequence_executable = options.sequence_executable.resolve()
    binder = options.binder.resolve()
    evaluator = options.evaluator.resolve()
    producer = options.producer.resolve()
    seal_executable = (options.seal_executable.resolve()
                       if options.seal_executable is not None else None)
    expect(all(path.is_file() for path in (
        generator, sequence_executable, binder, evaluator, producer)) and
        seal_executable is not None and seal_executable.is_file(),
        "plan generator 的公开依赖缺失")
    fixture = load_fixture_module(
        pathlib.Path(__file__).with_name(
            "mouse_effect_probe_b_composite_phase_binder_tests.py"))

    with tempfile.TemporaryDirectory(
            prefix="xen-composite-phase-plan-") as temporary:
        root = pathlib.Path(temporary)
        diagnostic_path = test_scheduler_diagnostic(seal_executable, root)
        sequence_path = root / "sequence.json"
        generated = subprocess.run([
            str(sequence_executable), "--output", str(sequence_path),
            "--profile", "physical-b-composite-phase-calibration",
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(generated.returncode == 0 and sequence_path.is_file(),
               f"冻结 sequence 生成失败: {generated.stderr}")

        fixture_plan, capture, commands = fixture.build_inputs(
            binder, evaluator)
        policy_path = root / "capture-policy.json"
        write_json(policy_path, fixture_plan["capture_policy"])
        common = [
            sys.executable, str(generator),
            "--sequence", str(sequence_path),
            "--capture-policy", str(policy_path),
            "--binder", str(binder),
            "--evaluator", str(evaluator),
            "--producer", str(producer),
            "--report-verifier", str(seal_executable),
            "--run-uuid", fixture_plan["run_uuid"],
            "--activation-epoch", str(fixture_plan["activation_epoch"]),
            "--scope-id", fixture_plan["scope_id"],
        ]
        seed_path = root / "plan-seed.json"
        seed_result = subprocess.run(
            common + ["--output", str(seed_path)], check=False,
            capture_output=True, text=True, encoding="utf-8")
        expect(seed_result.returncode == 0 and seed_path.is_file(),
               f"plan seed 生成失败: {seed_result.stderr}")
        seed = json.loads(seed_path.read_text(encoding="utf-8"))
        rejected_diagnostic = subprocess.run(common + [
            "--preflight", str(diagnostic_path),
            "--frozen-at-utc-unix-ns", "9000000000000",
            "--output", str(root / "diagnostic-cannot-freeze-plan.json"),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(rejected_diagnostic.returncode == 2 and
               not (root / "diagnostic-cannot-freeze-plan.json").exists(),
               "正式 plan freezer 必须拒绝把诊断结果消费为 preflight")
        semantic_input = dict(seed)
        claimed_seed = semantic_input.pop("plan_seed_semantic_sha256")
        expect(seed["status"] == "AWAITING_AUXILIARY_PREFLIGHT" and
               seed["frozen_at_utc_unix_ns"] is None and
               seed["scheduler_policy"]["preflight_file_sha256"] is None and
               seed["scheduler_policy"]["issue_lead_applies_to"] ==
                   "NONZERO_PULSE_ONLY" and
               seed["scheduler_policy"][
                   "negative_control_marker_lead_ns"] == 0 and
               claimed_seed == canonical_sha256(semantic_input),
               "Prepare 只能生成未揭示 response 的 plan seed")

        preflight_path = root / "scheduler-preflight.json"
        preflight = {
            "schema_version": 1,
            "evidence_type":
                "mouse_effect_probe_b_composite_phase_scheduler_preflight",
            "status": "PASS",
            "physical_output_capability": False,
            "physical_dispatch_count": 0,
            "run_uuid": fixture_plan["run_uuid"],
            "activation_epoch": fixture_plan["activation_epoch"],
            "sequence_semantic_sha256":
                json.loads(sequence_path.read_text(encoding="utf-8"))[
                    "sequence_sha256"],
        }
        write_json(preflight_path, preflight)
        final_path = root / "plan.json"
        final_result = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "9000000000000",
            "--output", str(final_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        final = json.loads(final_path.read_text(encoding="utf-8"))
        semantic_input = dict(final)
        claimed = semantic_input.pop("plan_semantic_sha256")
        expect(final_result.returncode == 0 and
               final["status"] == "FROZEN_BEFORE_CAPTURE" and
               final["scheduler_policy"]["preflight_file_sha256"] ==
                   fixture.file_sha256(preflight_path) and
               claimed == canonical_sha256(semantic_input) and
               final["sequence_binding"]["sample_count"] == 295 and
               final["sequence_binding"]["window_count"] == 42,
               "同机 preflight 后最终 plan 必须完整封存 sequence/scheduler")

        capture["plan_semantic_sha256"] = claimed
        capture["report_verifier_file_sha256"] = final["seal"][
            "report_verifier_file_sha256"]
        commands["plan_semantic_sha256"] = claimed
        fixture.seal_semantic(capture, "capture_semantic_sha256")
        fixture.seal_semantic(commands, "command_semantic_sha256")
        capture_path = root / "capture.json"
        command_path = root / "commands.json"
        evidence_path = root / "evidence.json"
        write_json(capture_path, capture)
        write_json(command_path, commands)
        bound = subprocess.run([
            sys.executable, str(binder), "--plan", str(final_path),
            "--capture", str(capture_path), "--commands", str(command_path),
            "--output", str(evidence_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(bound.returncode == 0 and evidence_path.is_file(),
               f"generator 最终 plan 必须被冻结 binder 接受: {bound.stderr}")

        if seal_executable is not None:
            expect(seal_executable.is_file(), "composite seal executable 缺失")
            duplicate_preflight_path = root / "duplicate-preflight.json"
            duplicate_plan_path = root / "duplicate-plan.json"
            duplicate = subprocess.run([
                str(seal_executable),
                "--plan-seed", str(seed_path.resolve()),
                "--sequence", str(sequence_path.resolve()),
                "--preflight-output", str(duplicate_preflight_path.resolve()),
                "--plan-output", str(duplicate_plan_path.resolve()),
                "--run-uuid", fixture_plan["run_uuid"],
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
            ], check=False, capture_output=True, text=True, encoding="utf-8")
            expect(duplicate.returncode == 2 and
                   not duplicate_preflight_path.exists() and
                   not duplicate_plan_path.exists(),
                   "composite seal 必须在 preflight 前拒绝重复参数")
            actual_preflight_path = root / "actual-preflight.json"
            actual_plan_path = root / "actual-plan.json"
            sealed = subprocess.run([
                str(seal_executable),
                "--plan-seed", str(seed_path.resolve()),
                "--sequence", str(sequence_path.resolve()),
                "--preflight-output", str(actual_preflight_path.resolve()),
                "--plan-output", str(actual_plan_path.resolve()),
                "--run-uuid", fixture_plan["run_uuid"],
                "--activation-epoch", str(fixture_plan["activation_epoch"]),
            ], check=False, capture_output=True, text=True, encoding="utf-8")
            if sealed.returncode == 0:
                expect(actual_preflight_path.is_file() and
                       actual_plan_path.is_file(),
                       "preflight 通过后必须原子发布两份输出")
                actual_preflight = json.loads(
                    actual_preflight_path.read_text(encoding="utf-8"))
                actual_plan = json.loads(
                    actual_plan_path.read_text(encoding="utf-8"))
                actual_semantic_input = dict(actual_plan)
                actual_semantic = actual_semantic_input.pop(
                    "plan_semantic_sha256")
                expect(actual_preflight["status"] == "PASS" and
                       actual_preflight["physical_dispatch_count"] == 0 and
                       actual_preflight["event_count"] == 42 and
                       actual_preflight["observed_max_wake_lateness_ns"] <=
                           actual_preflight["max_wake_lateness_ns"] and
                       actual_plan["status"] == "FROZEN_BEFORE_CAPTURE" and
                       actual_plan["scheduler_policy"][
                           "preflight_file_sha256"] ==
                           fixture.file_sha256(actual_preflight_path) and
                       actual_semantic == canonical_sha256(
                           actual_semantic_input),
                       "seal 必须绑定真实 preflight 且保持 output-off/semantic")
            else:
                expect(sealed.returncode == 3 and
                       "scheduler preflight" in sealed.stderr and
                       not actual_preflight_path.exists() and
                       not actual_plan_path.exists(),
                       f"当前调度竞争超限时必须在输出前 fail closed: {sealed.stderr}")

        collided = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "9000000000000",
            "--output", str(final_path),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(collided.returncode == 2,
               "plan generator 不得覆盖既有最终 plan")

        bad_preflight = dict(preflight)
        bad_preflight["physical_dispatch_count"] = 1
        bad_path = root / "bad-preflight.json"
        write_json(bad_path, bad_preflight)
        rejected = subprocess.run(common + [
            "--preflight", str(bad_path),
            "--frozen-at-utc-unix-ns", "9000000000001",
            "--output", str(root / "bad-plan.json"),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(rejected.returncode == 2,
               "非 output-off preflight 必须 fail closed")

        zero_freeze = subprocess.run(common + [
            "--preflight", str(preflight_path),
            "--frozen-at-utc-unix-ns", "0",
            "--output", str(root / "zero-freeze-plan.json"),
        ], check=False, capture_output=True, text=True, encoding="utf-8")
        expect(zero_freeze.returncode == 2,
               "最终 plan 的 freeze UTC 必须为正整数")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
