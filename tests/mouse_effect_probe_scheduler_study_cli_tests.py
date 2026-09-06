#!/usr/bin/env python3
"""一次有界 output-off study 的 CLI/持久化合同；原始证据全部保留。"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import uuid
from typing import Any

from mouse_effect_probe_b_composite_phase_plan_tests import (
    validate_scheduler_raw_counts,
)


PROTOCOL = {
    "schema_version": 1,
    "guard_grid_ns": [300_000, 325_000, 350_000],
    "round_count": 10,
    "validation_batch_count": 10,
    "event_count": 42,
    "interval_ns": 5_000_000,
    "max_wake_lateness_ns": 150_000,
    "max_event_interval_width_ns": 100_000,
    "max_active_wait_ns_per_event": 350_000,
    "max_active_wait_ns_total": 14_700_000,
    "campaign_timeout_ns": 30_000_000_000,
    "wait_mode": "WAIT_FOR_MULTIPLE_OBJECTS_STOP_FIRST",
    "wait_timeout_ms": 1000,
    "active_stop_poll_per_iteration": True,
    "campaign_max_batches": 40,
    "campaign_max_active_wait_ns": 588_000_000,
    "round_order": "guard_index=(round_index+slot)%3",
    "warmup_excluded": False,
    "characterization_quality_tail_retained": True,
    "validation_stop_on_first_failure": True,
    "validation_not_used_for_selection": True,
    "statistical_independence_claimed": False,
}
FORMAL_KEYS = {
    "run_uuid", "plan_seed", "plan_seed_semantic_sha256",
    "sequence_semantic_sha256", "preflight_semantic_sha256",
    "plan_semantic_sha256", "activation_epoch",
}


def expect(condition: bool, reason: str) -> None:
    if not condition:
        raise AssertionError(reason)


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        expect(name not in result, f"JSON 不得含重复字段：{name}")
        result[name] = value
    return result


def reject_constant(value: str) -> None:
    raise AssertionError(f"JSON 不得含非有限数：{value}")


def decode_json(raw: bytes) -> dict[str, Any]:
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=strict_object,
                       parse_constant=reject_constant)
    expect(isinstance(value, dict), "产物顶层必须为 JSON 对象")
    return value


def read_json(path: pathlib.Path) -> dict[str, Any]:
    expect(path.is_file(), f"缺少应保留的产物：{path.name}")
    return decode_json(path.read_bytes())


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integer(value: Any, reason: str) -> int:
    expect(type(value) is int, reason)
    return value


def reject_formal_keys(value: Any) -> None:
    if isinstance(value, dict):
        expect(not (set(value) & FORMAL_KEYS), "study 不得携带正式 Run/plan 身份")
        for child in value.values():
            reject_formal_keys(child)
    elif isinstance(value, list):
        for child in value:
            reject_formal_keys(child)


def output_boundary(value: dict[str, Any], *, protocol: bool = False) -> None:
    expect(value.get("diagnostic_only") is True and
           value.get("physical_output_capability") is False and
           type(value.get("physical_dispatch_count")) is int and
           value["physical_dispatch_count"] == 0 and
           value.get("final_plan_published") is False,
           "study 必须保持无物理输出能力、零 dispatch 和无 final plan")
    if not protocol:
        expect(value.get("formal_preflight_published") is False,
               "study 不得发布正式 preflight")
    reject_formal_keys(value)


def snapshot(root: pathlib.Path) -> dict[str, str | None]:
    """只读本测试新建的小目录，用于确认参数负例没有发布任何文件。"""
    return {str(path.relative_to(root)): sha256(path) if path.is_file() else None
            for path in root.rglob("*")}


def invoke(executable: pathlib.Path, arguments: list[str], cwd: pathlib.Path,
           log_prefix: pathlib.Path, timeout: int) -> subprocess.CompletedProcess[bytes]:
    # subprocess.run 使用 communicate 同时排空双流；超时会终止子进程并收集残余输出。
    try:
        result = subprocess.run([str(executable), *arguments], cwd=cwd,
                                capture_output=True, check=False, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log_prefix.with_suffix(".stdout.log").write_bytes(error.stdout or b"")
        log_prefix.with_suffix(".stderr.log").write_bytes(error.stderr or b"")
        raise AssertionError(f"原生命令超过 {timeout} 秒；证据保留，不重试") from error
    log_prefix.with_suffix(".stdout.log").write_bytes(result.stdout)
    log_prefix.with_suffix(".stderr.log").write_bytes(result.stderr)
    # 明确检查 UTF-8，而不以替换字符掩盖 caller 编码错误。
    result.stdout.decode("utf-8")
    result.stderr.decode("utf-8")
    return result


def check_negative_arguments(seal: pathlib.Path, root: pathlib.Path,
                             logs: pathlib.Path) -> int:
    existing = root / "existing"
    existing.mkdir()
    (existing / "sentinel.txt").write_bytes(b"preserve-existing-evidence\n")
    new_directory = root / "must-not-be-created"
    other = root / "must-not-be-written.json"
    commands = [
        ["--study-scheduler"],
        ["--study-scheduler", "relative-study"],
        ["--study-scheduler", str(existing)],
        ["--study-scheduler", str(new_directory), "--study-scheduler", str(new_directory)],
        ["--study-scheduler", str(new_directory), "--diagnose-scheduler", str(other)],
        ["--diagnose-scheduler", str(other), "--study-scheduler", str(new_directory)],
        ["--study-scheduler", str(new_directory), "--plan-output", str(other)],
        ["--plan-seed", str(other), "--study-scheduler", str(new_directory)],
        ["--study-scheduler", str(new_directory), "--verify-report", str(other)],
        ["--report-semantic-sha256", str(other), "--study-scheduler", str(new_directory)],
    ]
    before = snapshot(root)
    for index, arguments in enumerate(commands):
        rejected = invoke(seal, arguments, root, logs / f"negative-{index:02d}", 10)
        expect(rejected.returncode == 2 and snapshot(root) == before,
               f"负例 {index} 必须在计时前返回 2，且不新建/修改任何产物")
    return len(commands)


def timing_policy(guard: int) -> dict[str, Any]:
    return {
        "event_capacity": 42, "preflight_interval_ns": 5_000_000,
        "active_guard_ns": guard, "max_wake_lateness_ns": 150_000,
        "max_event_interval_width_ns": 100_000,
        "max_active_wait_ns_per_event": 350_000,
        "max_active_wait_ns_total": 14_700_000,
        "timer_mode": "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL",
    }


def check_raw_counts(batch: dict[str, Any]) -> None:
    try:
        validate_scheduler_raw_counts(batch)
    except AssertionError as error:
        # QPC 倒退本身就是必须保存的拒绝证据；不能要求失败原值也满足成功时的单调性。
        events = batch["events"]
        expect(bool(events) and batch["status"] == "ABORTED" and
               batch["failure_event"] == len(events) - 1 and
               str(error) == "已取得的同线程 QPC 不得倒退",
               f"原始整数 oracle 拒绝：{error}")
        last = events[-1]
        expect(last["completed"] is False and batch["failure_stage"] == last["last_stage"],
               "只有末个未完成事件允许保存显式 QPC-order 失败")
        pairs = {
            "SET_TIMER_BEFORE_QPC": [("set_timer_before_qpc", "due_base_qpc")],
            "SET_TIMER_AFTER_QPC": [("set_timer_after_qpc", "set_timer_before_qpc")],
            "STUDY_QPC_ORDER": [("wait_return_qpc", "set_timer_after_qpc"),
                                ("active_last_qpc", "active_enter_qpc")],
            "MARKER_BEFORE_QPC": [("marker_before_qpc", "active_last_qpc")],
            "MARKER_ORDER": [("marker_after_qpc", "marker_before_qpc")],
        }
        expect(any(last[after] is not None and last[before] is not None and
                   last[after] < last[before]
                   for after, before in pairs.get(last["last_stage"], [])),
               "QPC-order 拒绝必须对应实际保存的倒退原值")
        expect(all(last[field] is None for field in
                   ("deadline_lateness_ns", "marker_width_ns", "active_wait_ns")),
               "QPC 倒退之后不得伪造有效质量指标")
        prefix = dict(batch, events=events[:-1])
        validate_scheduler_raw_counts(prefix)
        expect(last["active_total_before_ns"] == sum(
            event["active_wait_ns"] for event in events[:-1]),
            "QPC-order 失败前的 active 累计仍必须精确")


def check_batch(batch: dict[str, Any], context: dict[str, Any], phase: str,
                guard: int) -> tuple[int, int]:
    output_boundary(batch)
    expect(batch["evidence_type"] == "mouse_effect_probe_scheduler_study_batch" and
           batch["schema_version"] == 1 and batch["study_phase"] == phase and
           batch["context"] == context and batch["timing_policy"] == timing_policy(guard),
           "批次类型、阶段、身份或 guard 发生漂移")
    expect(batch["completed_means"] == "RAW_SAMPLE_COMPLETE_NOT_QUALITY_PASS" and
           batch["instrumented"] is True and batch["timing_perturbed"] is True and
           batch["timing_loop_json_bookkeeping"] is False and
           batch["study_stop_polling"] is True and batch["study_wait_timeout_ms"] == 1000,
           "study 的完整性语义和可取消观测方式必须明确")
    events = batch["events"]
    expect(isinstance(events, list) and len(events) == batch["reached_event_count"] and
           0 <= len(events) <= 42, "固定容量只允许保存实际到达的事件")
    completed = 0
    observed_active = 0
    last_observed = batch["anchor_qpc"] or 0
    for ordinal, event in enumerate(events):
        expect(event["event_ordinal"] == ordinal and type(event["completed"]) is bool,
               "原始事件 ordinal 或 completed 类型错误")
        for field, valid in event["valid"].items():
            expect(type(valid) is bool and (event[field] is not None) == valid,
                   f"未取得原始字段必须为 null：{field}")
            if valid:
                integer(event[field], f"原始字段必须保留整数：{field}")
        for field in ("due_base_qpc", "set_timer_before_qpc", "set_timer_after_qpc",
                      "wait_return_qpc", "active_last_qpc", "marker_before_qpc", "marker_after_qpc"):
            if event[field] is not None:
                last_observed = max(last_observed, event[field])
        if event["completed"]:
            expect(completed == ordinal and all(event["valid"].values()) and
                   event["set_timer_result"] == 1 and event["wait_result"] == 1,
                   "完整样本必须保留全部字段及原生 WaitMultiple 的 timer 返回 1")
            expect(event["active_wait_ns"] <= 350_000 and
                   event["active_total_before_ns"] + event["active_wait_ns"] <= 14_700_000,
                   "完整样本不得跨过 active 硬上限")
            if phase == "VALIDATION":
                expect(event["deadline_lateness_ns"] <= 150_000 and
                       event["marker_width_ns"] <= 100_000,
                       "验证阶段质量失败不得记成完成后继续采样")
            # 表征 completed 只表示原始采样完整，故允许质量失败进入后续批次和 selector。
            completed += 1
        else:
            expect(ordinal == len(events) - 1, "首次中止之后不得再有事件")
        if event["active_wait_ns"] is not None:
            observed_active += event["active_wait_ns"]
        elif event["active_enter_qpc"] is not None and event["active_last_qpc"] is not None:
            ticks = event["active_last_qpc"] - event["active_enter_qpc"]
            frequency = batch["qpc_frequency_hz"]
            observed_active += 0 if ticks <= 0 else (
                ticks * 1_000_000_000 + frequency - 1) // frequency
    check_raw_counts(batch)
    expect(completed == batch["completed_event_count"], "completed 数量必须来自已保存原始事件")
    if batch["status"] == "COMPLETE":
        expect(completed == len(events) == 42 and batch["failure_event"] is None and
               batch["failure_reason"] == "", "完整批次必须采满原定 42 项")
    else:
        expect(batch["status"] == "ABORTED" and bool(batch["failure_reason"]),
               "不完整批次必须显式保留中止原因")
    return last_observed, observed_active


def check_artifacts(study: pathlib.Path, seal: pathlib.Path, selector: pathlib.Path,
                    logs: pathlib.Path, returncode: int) -> dict[str, Any]:
    protocol = read_json(study / "protocol.json")
    characterization = read_json(study / "characterization.json")
    result = read_json(study / "result.json")
    output_boundary(protocol, protocol=True)
    output_boundary(characterization)
    output_boundary(result)
    expect(protocol["protocol"] == PROTOCOL and
           protocol["status"] == "FROZEN_BEFORE_MEASUREMENT",
           "原生公开协议必须与预注册采样、质量及总时限一致")
    context = protocol["context"]
    expect(context["executable_sha256"] == sha256(seal) and
           context["process_priority_class"] == 32 and context["thread_priority"] == 0 and
           context["same_process_same_boot_required"] is True and
           context["process_id"] > 0 and context["thread_id"] > 0 and
           context["qpc_frequency_hz"] > 0 and context["campaign_start_qpc"] > 0,
           "实际执行文件/进程/线程/时基必须与冻结上下文一致")
    expect(characterization["protocol"] == PROTOCOL and characterization["context"] == context and
           characterization["evidence_type"] == "mouse_effect_probe_scheduler_study_characterization",
           "characterization 不得更换协议、身份或类型")
    protocol_hash = sha256(study / "protocol.json")
    characterization_hash = sha256(study / "characterization.json")
    expect(characterization["protocol_file_sha256"] == protocol_hash and
           result["protocol_file_sha256"] == protocol_hash and
           result["characterization_file_sha256"] == characterization_hash,
           "protocol/characterization/result 必须绑定实际文件字节 hash")
    blocks = characterization["blocks"]
    expect(1 <= len(blocks) <= 30, "表征只能尝试固定顺序的至多 30 批")
    last_observed = context["campaign_start_qpc"]
    active_sum = 0
    for index, block in enumerate(blocks):
        round_index = index // 3
        guard_index = (round_index + index % 3) % 3
        expect(block["round_index"] == round_index and block["guard_index"] == guard_index,
               "表征批次必须遵循预注册轮转顺序")
        batch = block["diagnostic"]
        if batch["anchor_qpc"] is not None:
            expect(batch["anchor_qpc"] > last_observed, "新批 anchor 必须晚于前一批原始观测")
        last_observed, active = check_batch(batch, context, "CHARACTERIZATION",
                                           PROTOCOL["guard_grid_ns"][guard_index])
        active_sum += active
        expect(index == len(blocks) - 1 or batch["status"] == "COMPLETE",
               "表征中止之后不得再采下一个批次")
    candidate_path = study / "candidate.json"
    validation_path = study / "validation.json"
    candidate = read_json(candidate_path) if candidate_path.exists() else None
    validation = read_json(validation_path) if validation_path.exists() else None
    expected_files = {"protocol.json", "characterization.json", "result.json"}
    for name, value, key in (("candidate.json", candidate, "candidate_file_sha256"),
                             ("validation.json", validation, "validation_file_sha256")):
        if value is None:
            expect(result[key] is None, "不存在文件的 hash 必须为 null")
        else:
            expected_files.add(name)
            output_boundary(value)
            expect(result[key] == sha256(study / name), f"{name} 必须绑定实际文件 hash")
    expect({path.name for path in study.iterdir()} == expected_files and
           all(path.is_file() for path in study.iterdir()),
           "study 目录只允许诊断协议/raw/candidate/result，不得有正式 plan 或 pending")
    selected_guard = None
    if characterization["status"] == "ABORTED":
        expect(result["status"] == "ABORTED" and candidate is None and validation is None,
               "表征中止不得形成可用候选或启动验证")
    else:
        expect(characterization["status"] == "COMPLETE" and len(blocks) == 30 and
               all(block["diagnostic"]["status"] == "COMPLETE" for block in blocks),
               "完整 characterization 必须保持 30×42 个完整原始样本")
        offline = invoke(selector, ["--characterization", str(study / "characterization.json")],
                         study, logs / "selector", 10)
        expect(offline.returncode == 0, "独立 selector 必须接受完整真实 characterization")
        decision = decode_json(offline.stdout)
        if candidate is None:
            expect(result["status"] == "ABORTED" and validation is None,
                   "选择前中止不得启动验证")
        else:
            expect(all(candidate[key] == decision[key] for key in
                       ("status", "selected_guard_ns", "guard_results")),
                   "候选必须与独立 selector 对真实 raw 的确定性决定完全相同")
            expect(candidate["protocol_file_sha256"] == protocol_hash and
                   candidate["characterization_file_sha256"] == characterization_hash and
                   candidate["context"] == context,
                   "候选必须绑定已冻结的输入和实际上下文")
            selected_guard = candidate["selected_guard_ns"]
            if candidate["status"] == "NO_CANDIDATE":
                expect(selected_guard is None and validation is None and
                       result["status"] in ("NO_CANDIDATE", "ABORTED"),
                       "无候选不得开始验证或暗中追加 guard")
            else:
                expect(candidate["status"] == "CANDIDATE_SELECTED" and
                       selected_guard in PROTOCOL["guard_grid_ns"], "只能选择预注册采样点")
    validation_count = 0
    if validation is not None:
        expect(candidate is not None and candidate["status"] == "CANDIDATE_SELECTED" and
               validation["context"] == context and validation["protocol"] == PROTOCOL and
               validation["evidence_type"] == "mouse_effect_probe_scheduler_study_validation" and
               validation["candidate_file_sha256"] == sha256(candidate_path) and
               validation["characterization_file_sha256"] == characterization_hash and
               validation["protocol_file_sha256"] == protocol_hash and
               validation["not_used_for_candidate_selection"] is True and
               validation["statistical_independence_claimed"] is False,
               "验证必须固定同一候选/输入/路径并声明不反向选择")
        frozen = integer(result["candidate_frozen_qpc"], "candidate 冻结时间必须是原始 QPC")
        expect(validation["candidate_frozen_qpc"] == frozen and frozen > last_observed,
               "candidate 冻结必须在完整表征之后且先于验证")
        last_observed = frozen
        validation_count = len(validation["blocks"])
        expect(1 <= validation_count <= 10, "验证不能超过固定 10 批或空报告通过")
        for index, block in enumerate(validation["blocks"]):
            expect(block["round_index"] == index and
                   PROTOCOL["guard_grid_ns"][block["guard_index"]] == selected_guard,
                   "验证失败后不得改 guard 或补跑不同配置")
            batch = block["diagnostic"]
            if batch["anchor_qpc"] is not None:
                expect(batch["anchor_qpc"] > last_observed,
                       "验证 anchor 必须晚于 candidate 冻结及前一验证批")
            last_observed, active = check_batch(batch, context, "VALIDATION", selected_guard)
            active_sum += active
            expect(index == validation_count - 1 or batch["status"] == "COMPLETE",
                   "首次验证失败必须终止，不得重试挑 PASS")
        if validation["status"] == "COMPLETE":
            expect(validation_count == 10 and
                   all(block["diagnostic"]["status"] == "COMPLETE" for block in validation["blocks"]),
                   "验证通过必须来自全部 10 批完整观测")
        else:
            expect(validation["status"] == "ABORTED" and
                   result["status"] in ("VALIDATION_REJECTED", "ABORTED"),
                   "验证中止/失败不能成为 study 通过")
    expect(result["status"] in ("ABORTED", "NO_CANDIDATE", "VALIDATION_REJECTED", "STUDY_VALIDATED"),
           "study 必须给出有限的诊断结果状态")
    expect((returncode == 0) == (result["status"] == "STUDY_VALIDATED"),
           "只有完整经验验证通过可返回 0；拒绝返回 3")
    if result["status"] == "STUDY_VALIDATED":
        expect(validation is not None and validation["status"] == "COMPLETE" and validation_count == 10,
               "整体通过必须有同一候选的全部验证证据")
    elif result["status"] == "VALIDATION_REJECTED":
        expect(validation is not None and validation["status"] == "ABORTED",
               "验证拒绝必须保留实际失败的验证 raw")
    expect(result["attempted_batch_count"] == len(blocks) + validation_count and
           result["recorded_active_wait_ns"] == active_sum and
           result["campaign_max_active_wait_ns"] == 588_000_000,
           "campaign 总 active 必须逐条计入部分事件的已观察下界")
    frequency = context["qpc_frequency_hz"]
    elapsed_ticks = result["finished_qpc"] - context["campaign_start_qpc"]
    expect(elapsed_ticks >= 0 and result["elapsed_ns"] ==
           (elapsed_ticks * 1_000_000_000 + frequency - 1) // frequency,
           "campaign 经过时间必须用同一个 start/finish QPC 重算")
    if result["status"] != "ABORTED":
        expect(result["elapsed_ns"] < 30_000_000_000 and active_sum <= 588_000_000,
               "未中止状态不得超出整个 campaign 的时限或 active 预算")
        expect(all(context.get(key) == value for key, value in result["context_after"].items()),
               "未中止结果的实际 session/priority 等上下文不得漂移")
    return {"status": result["status"], "returncode": returncode,
            "characterization_batches": len(blocks), "validation_batches": validation_count,
            "selected_guard_ns": selected_guard, "elapsed_ns": result["elapsed_ns"],
            "recorded_active_wait_ns": active_sum, "study_directory": str(study)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seal-executable", required=True, type=pathlib.Path)
    parser.add_argument("--selector-executable", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-root", required=True, type=pathlib.Path)
    options = parser.parse_args()
    for path in (options.seal_executable, options.selector_executable):
        expect(path.is_absolute() and path.is_file(), "必须传入真实可执行文件的绝对路径")
    expect(options.evidence_root.is_absolute(), "evidence-root 必须是绝对路径")
    options.evidence_root.mkdir(parents=True, exist_ok=True)
    evidence = options.evidence_root / ("scheduler-study-cli-" + uuid.uuid4().hex)
    evidence.mkdir()
    logs = evidence / "logs"
    logs.mkdir()
    negative_root = evidence / "negative"
    negative_root.mkdir()
    study = evidence / ("study-" + uuid.uuid4().hex)
    print("SCHEDULER_STUDY_CLI_EVIDENCE=" + str(evidence), flush=True)
    negatives = check_negative_arguments(options.seal_executable, negative_root, logs)
    # 这是脚本唯一允许进入计时内核的调用。无论 0、3、超时或断言失败都保留目录。
    measured = invoke(options.seal_executable, ["--study-scheduler", str(study)],
                      evidence, logs / "study", 40)
    expect(measured.returncode in (0, 3),
           f"唯一 study 必须返回测量结果 0/3，实际 {measured.returncode}；原始双流见 logs")
    summary = check_artifacts(study, options.seal_executable,
                              options.selector_executable, logs, measured.returncode)
    summary["negative_argument_cases"] = negatives
    summary["actual_timing_invocations"] = 1
    (evidence / "test-summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print("scheduler study CLI 合同通过：" + json.dumps(
        summary, ensure_ascii=False, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"scheduler study CLI 合同失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
