#!/usr/bin/env python3
"""固定目标迭代离线审核；命令完成量不是物理输出证明，不自动生成或激活曲线。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys
import uuid
from datetime import datetime, timezone
from contextlib import contextmanager


VERSION = "target-iteration-analysis-v1"
FILES = ("frames.jsonl", "commands.jsonl", "events.jsonl", "review.json")
SPLITS = {"fit", "holdout", "test", "calibration"}


class ContractError(ValueError):
    """数据合同拒绝，调用方应保留原始证据。"""


def require(condition, reason):
    if not condition:
        raise ContractError(reason)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"JSON 重复字段：{key}")
        result[key] = value
    return result


def decode(text):
    return json.loads(text, object_pairs_hook=_pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ContractError(f"非法数值：{value}")))


def read_json(path):
    return decode(Path(path).read_text(encoding="utf-8-sig"))


def rows(path):
    return [decode(line) for line in Path(path).read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def integer(value):
    return type(value) is int


def text_field(obj, key):
    value = obj.get(key)
    require(isinstance(value, str) and 0 < len(value) <= 512, f"缺少身份字段：{key}")
    return value


def safe_file(root, relative):
    require(isinstance(relative, str) and relative, "文件路径缺失")
    path = (root / relative).resolve()
    require(path.is_relative_to(root.resolve()) and path.is_file(), f"文件缺失或超出 Run：{relative}")
    return path


def normalize_capture(manifest, loaded):
    """适配 C++ 采集合同；缺失事实保持未知，不从接收时间制造曝光时间。"""
    if manifest.get("schema") != "recoil_target_iteration_v1":
        return manifest, loaded
    m = dict(manifest)
    start = m.get("software_fire_start_ns")
    def relative(value):
        return (value - start) // 1000 if integer(value) and integer(start) else None
    m.update(schema_version=1, parent_profile_sha256=m.get("baseline_file_sha256", m.get("baseline_sha256")),
             normalized_baseline_sha256=m.get("baseline_sha256"),
             target_id=m.get("template_sha256"), anchor_id=m.get("template_sha256"),
             firing_id="1" if m.get("fire") is True else "0", clock_domain="steady",
             fire_start_us=start // 1000 if integer(start) else None,
             window_end_us=relative(m.get("window_end_ns")), max_shots=m.get("target_shots"))
    # C++ 已按唯一武器目录校验运行时短名；序列化父曲线使用 GSI 持久化名称。
    if isinstance(m.get("profile"), dict):
        m["runtime_weapon_id"] = m.get("weapon_id")
        m["weapon_id"] = m["profile"].get("weapon_id")
    result = dict(loaded)
    result["commands.jsonl"] = []
    for row in loaded["commands.jsonl"]:
        command = dict(row)
        command.update(command_id=str(row.get("command_id", "")), firing_id=str(row.get("firing_id", "")),
                       t_us=relative(row.get("calculated_ns") if row.get("status") == "no_op" else row.get("backend_completed_ns")),
                       phase={"fire": "firing", "control": "align", "calibrate": "align"}.get(row.get("phase")),
                       status="backend_completed" if row.get("status") == "completed" else row.get("status"))
        for prefix, field in (("ff_d", "ff_completed"), ("fb_d", "fb_completed"), ("d", "total")):
            vector = row.get(field)
            for index, axis in enumerate(("x", "y")):
                command[prefix + axis] = vector[index] if isinstance(vector, list) and len(vector) == 2 else None
        result["commands.jsonl"].append(command)
    result["frames.jsonl"] = []
    for row in loaded["frames.jsonl"]:
        frame = dict(row)
        error = row.get("error_px")
        frame.update(frame_id=str(row.get("frame_id", "")), t_us=relative(row.get("received_ns")),
                     target_id=row.get("identity_sha256"), anchor_id=row.get("identity_sha256"),
                     valid=row.get("anchor_valid"),
                     error_x=error[0] if isinstance(error, list) and len(error) == 2 else None,
                     error_y=error[1] if isinstance(error, list) and len(error) == 2 else None)
        result["frames.jsonl"].append(frame)
    return m, result


def _load_run(directory):
    root = Path(directory).resolve()
    manifest = read_json(root / "manifest.json")
    require(isinstance(manifest, dict), "Run manifest 必须是对象")
    require(manifest.get("schema_version") == 1 or manifest.get("schema") == "recoil_target_iteration_v1",
            "不支持的 Run schema")
    hashes = manifest.get("files", {})
    require(isinstance(hashes, dict), "Run 文件摘要表必须是对象")
    for filename in FILES:
        recovery = "；可能审核被中断，请核对 review_history 的旧版副本与事务记录" if filename == "review.json" else ""
        require(hashes.get(filename) == sha256(safe_file(root, filename)), f"文件摘要不匹配：{filename}{recovery}")
    loaded = {name: read_json(root / name) if name.endswith(".json") else rows(root / name) for name in FILES}
    require(isinstance(loaded["review.json"], dict), "review 必须是对象")
    require(all(isinstance(row, dict) for name in FILES[:3] for row in loaded[name]), "逐行记录必须是对象")
    frozen_manifest = dict(manifest)
    frozen_manifest["files"] = {key: value for key, value in hashes.items() if key != "review.json"}
    metadata_id = hashlib.sha256(json.dumps(frozen_manifest, sort_keys=True).encode()).hexdigest()
    manifest, loaded = normalize_capture(manifest, loaded)
    for field in ("run_id", "parent_profile_sha256", "environment_fingerprint", "target_id", "anchor_id",
                  "firing_id", "device_epoch", "weapon_id"):
        text_field(manifest, field)
    require(manifest.get("split") in SPLITS, "未知数据用途")
    require(integer(manifest.get("generation")) and manifest["generation"] >= 0, "代次缺失")
    # 不包含 run_id/review/split，改名或改用途仍能识别同一原始记录。
    payload_id = hashlib.sha256("\n".join(hashes[name] for name in FILES[:3]).encode()).hexdigest()
    return {"root": root, "manifest": manifest, "hash": sha256(root / "manifest.json"),
            "payload_id": payload_id, "metadata_id": metadata_id, **loaded}


def _validate(run, *, require_review=True):
    m = run["manifest"]
    review = run["review.json"]
    require(review.get("split") == m["split"], "审核用途与 manifest 不一致")
    if require_review:
        for field in ("integrity_pass", "identity_pass"):
            require(review.get(field) is True, f"人工审核未完成：{field}")
    require(m.get("clock_domain") == "steady", "命令时间轴不是本机单调时钟")
    require(integer(m.get("fire_start_us")), "缺少软件开火参考时间")
    end = m.get("window_end_us")
    require(integer(end) and end > 0, "有效短时域缺失")
    require(integer(m.get("max_shots")) and 0 < m["max_shots"] <= 5, "本阶段最多五发")
    observed = m.get("observed_shots")
    require(integer(observed) and 0 < observed <= m["max_shots"], "GSI 观测弹数未知或超限")
    require(m.get("sensitivity") == 1.4 and m.get("x_strength") == 1 and m.get("y_strength") == 1,
            "环境合同要求灵敏度 1.4、XY100%")
    uncertainty = m.get("alignment_uncertainty_us")
    tolerance = m.get("max_alignment_uncertainty_us")
    require(number(uncertainty) and number(tolerance) and 0 <= uncertainty <= tolerance,
            "软件参考起点不确定度缺失或超限")
    ff_tolerance = m.get("ff_schedule_tolerance_us", 0)
    require(integer(ff_tolerance) and 0 <= ff_tolerance < end, "前馈软件时序容差非法")
    events = run["events.jsonl"]
    require(events and all(event.get("event_loss_count") == 0 for event in events), "事件缺失或丢失计数未知")
    require(sum(event.get("type") == "start" for event in events) == 1, "开火起点不唯一")
    ends = [event for event in events if event.get("type") == "end"]
    require(len(ends) == 1 and ends[0].get("reason") == "completed" and
            ends[0].get("cleanup_complete") is True, "终止或按钮清理不完整")
    require(all(event.get("type") not in {"cancel", "unknown", "safety_failure", "identity_changed"}
                for event in events), "本轮存在取消、安全或身份异常")
    commands = run["commands.jsonl"]
    require(commands, "命令记录为空")
    ids, previous = set(), -math.inf
    for command in commands:
        identity = text_field(command, "command_id")
        require(identity not in ids, "重复命令回执")
        ids.add(identity)
        require(command.get("firing_id") == m["firing_id"] and command.get("device_epoch") == m["device_epoch"],
                "命令发射会话或设备代次不匹配")
        t = command.get("t_us")
        require(integer(t) and t >= previous, "命令时钟跳变或缺失")
        previous = t
        require(command.get("status") in {"backend_completed", "no_op"} and command.get("saturated") is False,
                "存在拒绝、未知完成或饱和命令")
        require(command.get("phase") in {"align", "firing", "recovery"}, "命令阶段未知")
        if m.get("schema") == "recoil_target_iteration_v1":
            times = [command.get(key) for key in ("planned_ns", "calculated_ns", "call_started_ns", "returned_ns")]
            require(all(integer(value) for value in times) and times == sorted(times), "命令计划/调用/返回时间无效")
            completed = command.get("backend_completed_ns")
            if command["status"] == "backend_completed":
                require(integer(completed) and times[2] <= completed <= times[3], "后端完成时间超出调用区间")
            ack = command.get("ack_ns")
            require(ack is None or (integer(ack) and times[2] <= ack <= times[3]), "ACK 时间无效")
            require(command.get("queue_inventory") == 0, "在途输出库存未知或未清空")
        for axis in ("x", "y"):
            parts = [command.get(prefix + axis) for prefix in ("ff_d", "fb_d", "d")]
            require(all(integer(part) for part in parts) and parts[0] + parts[1] == parts[2],
                    "ff+fb 与后端完成总量不守恒")
        if command["status"] == "no_op":
            require(command.get("zero_command") is True and command["dx"] == command["dy"] == 0 and
                    command.get("backend_completed_ns") is None, "零总量账本行不能伪造实体完成回执")
        else:
            require(command.get("zero_command") is not True, "零命令不能冒充后端完成")
        if command["phase"] == "firing":
            require(0 <= t <= end + ff_tolerance, "开火命令超出短时域及已登记软件时序容差")
    frames = run["frames.jsonl"]
    require(len(frames) >= 2, "逐帧证据不足")
    previous, frame_ids, previous_drops, active_seen = -math.inf, set(), None, False
    for frame in frames:
        identity = text_field(frame, "frame_id")
        require(identity not in frame_ids, "重复源帧")
        frame_ids.add(identity)
        t = frame.get("t_us")
        require(integer(t) and t > previous, "帧时间缺失、重复或倒退")
        previous = t
        if m.get("schema") == "recoil_target_iteration_v1":
            phase = frame.get("phase")
            drops = tuple(frame.get(key) for key in ("source_dropped_frames", "transport_dropped_frames"))
            require(phase in {"prepare", "active", "tail"} and all(integer(value) and value >= 0 for value in drops),
                    "帧阶段或源端丢帧计数未知")
            if phase == "prepare":
                require(not active_seen, "活动窗口后不能重新定义预备期丢帧基准")
            else:
                active_seen = True
                require(previous_drops is None or drops == previous_drops, "活动窗口存在源端丢帧或计数器重置")
            previous_drops = drops
        require(frame.get("target_id") == m["target_id"] and frame.get("anchor_id") == m["anchor_id"],
                "目标或冻结锚点切换")
        require(frame.get("valid") is True and frame.get("background_consistent") is True,
                "观测失效或目标相对背景运动")
        require(all(number(frame.get(field)) for field in ("error_x", "error_y")), "锚点误差缺失")
        raw = safe_file(run["root"], frame.get("raw_path"))
        require(frame.get("raw_sha256") == sha256(raw), "原始帧摘要不匹配")
    require(frames[0]["t_us"] <= 0 and frames[-1]["t_us"] >= end, "原始帧未覆盖完整短时域")
    return run


def inspect_run(directory):
    """只读审核；拒绝原因留在报告，数据不足也不伪装成空曲线。"""
    try:
        run = _load_run(directory)
        _validate(run)
        return {"schema_version": 1, "algorithm": VERSION, "run_id": run["manifest"]["run_id"],
                "analysis_eligible": True, "executable_candidate_eligible": False,
                "physical_verified": False, "reasons": ["缺少可核验的独立响应/时序标定；本版仅分析后端完成命令残差"]}
    except (ValueError, KeyError, OSError, TypeError) as exc:
        return {"schema_version": 1, "algorithm": VERSION, "analysis_eligible": False,
                "executable_candidate_eligible": False, "physical_verified": False, "reasons": [str(exc)]}


@contextmanager
def exclusive_lock(path, reason):
    """进程句柄持有锁；强制退出由操作系统释放，遗留锁文件不代表仍占用。"""
    with Path(path).open("a+b") as handle:
        if handle.seek(0, os.SEEK_END) == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ContractError(reason) from exc
        try:
            yield
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def register_usage(registry_path, runs):
    """持久冻结用途；同时按 run 身份和原始记录识别重命名污染。"""
    path = Path(registry_path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = path.with_suffix(path.suffix + ".lock")
    with exclusive_lock(lock, "用途注册表正被使用，请稍后重试"):
        registry = read_json(path) if path.exists() else {"schema_version": 1, "runs": {}}
        require(isinstance(registry, dict) and registry.get("schema_version") == 1 and
                isinstance(registry.get("runs"), dict), "用途注册表损坏")
        entries = registry["runs"]
        require(all(isinstance(value, dict) and isinstance(value.get("payload_id"), str) and
                    isinstance(value.get("manifest_sha256s"), list) for value in entries.values()), "用途注册表条目损坏")
        for run in runs:
            m = run["manifest"]
            entry = {"split": m["split"], "payload_id": run["payload_id"],
                     "metadata_id": run["metadata_id"],
                     "parent_profile_sha256": m["parent_profile_sha256"],
                     "environment_fingerprint": m["environment_fingerprint"], "generation": m["generation"]}
            old = entries.get(m["run_id"])
            require(old is None or all(old.get(key) == value for key, value in entry.items()),
                    "已登记 Run 的内容或用途被改写")
            for key, value in entries.items():
                require(value["payload_id"] != entry["payload_id"] or key == m["run_id"],
                        "同一原始记录被重命名或跨用途复用")
            entry["manifest_sha256s"] = list(dict.fromkeys((old or {}).get("manifest_sha256s", []) + [run["hash"]]))
            entries[m["run_id"]] = entry
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(registry, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)


def review_run(directory, registry_path, observation, *, identity_confirmed=False):
    """人工仅确认身份并提供观察原话；完整性必须由原始记录自动重验。"""
    require(identity_confirmed is True, "必须明确确认固定目标和冻结锚点身份")
    require(isinstance(observation, str) and 0 < len(observation.strip()) <= 10000, "人工观察原话不能为空")
    run = _load_run(directory)
    register_usage(registry_path, [run])
    # 先完成全部自动合同核查，不能用人工勾选跳过终止/输出/时间异常。
    _validate(run, require_review=False)
    root = run["root"]
    lock = root / ".review.lock"
    with exclusive_lock(lock, "该 Run 正在审核，请稍后重试"):
        require(sha256(root / "manifest.json") == run["hash"], "审核期间 Run 已被更改")
        tag = uuid.uuid4().hex
        history = root / "review_history"
        history.mkdir(exist_ok=True)
        original_manifest = (root / "manifest.json").read_bytes()
        original_review = (root / "review.json").read_bytes()
        (history / f"{tag}.manifest.json").write_bytes(original_manifest)
        (history / f"{tag}.review.json").write_bytes(original_review)
        transaction = history / f"{tag}.transaction.json"
        transaction.write_text(json.dumps({"state": "prepared", "previous_manifest_sha256": run["hash"],
                                          "previous_review_sha256": hashlib.sha256(original_review).hexdigest()}), encoding="utf-8")
        review = dict(run["review.json"])
        review.update(split=run["manifest"]["split"], integrity_pass=True, identity_pass=True,
                      observation=observation, reviewed_at=datetime.now(timezone.utc).isoformat(),
                      physical_validation_passed=False, executable_candidate_eligible=False,
                      review_revision=tag, previous_review_sha256=hashlib.sha256(original_review).hexdigest())
        review_tmp = root / f".review-{tag}.tmp"
        manifest_tmp = root / f".manifest-{tag}.tmp"
        review_tmp.write_text(json.dumps(review, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        manifest = decode(original_manifest.decode("utf-8-sig"))
        manifest["files"]["review.json"] = sha256(review_tmp)
        manifest_tmp.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        try:
            os.replace(review_tmp, root / "review.json")
            os.replace(manifest_tmp, root / "manifest.json")
        except OSError:
            # 两文件提交若失败，恢复本次修改前的完整合同；历史副本保留。
            (root / "review.json").write_bytes(original_review)
            (root / "manifest.json").write_bytes(original_manifest)
            raise
        register_usage(registry_path, [_load_run(root)])
        transaction.write_text(json.dumps({"state": "committed", "manifest_sha256": sha256(root / "manifest.json"),
                                          "review_sha256": sha256(root / "review.json")}), encoding="utf-8")
        result = inspect_run(root)
        result.update(review_revision=tag, observation=observation)
        return result


def cumulative(commands, time_us, prefix):
    return [sum(command[prefix + axis] for command in commands
                if command["phase"] == "firing" and 0 <= command["t_us"] <= time_us) for axis in ("x", "y")]


def sample_parent(profile, time_us):
    points = profile["points"]
    t = time_us / 1000
    if t <= 0:
        return [0.0, 0.0]
    for left, right in zip(points, points[1:]):
        if t <= right[0]:
            fraction = (t - left[0]) / (right[0] - left[0])
            return [left[axis] + fraction * (right[axis] - left[axis]) for axis in (1, 2)]
    return list(points[-1][1:])


def parent_envelope(profile, time_us, tolerance_us):
    """仅按预先登记的软件调度时间带比较，包含带内曲线拐点。"""
    left, right = max(0, time_us - tolerance_us), time_us + tolerance_us
    times = [left, right, *(point[0] * 1000 for point in profile["points"] if left < point[0] * 1000 < right)]
    values = [sample_parent(profile, t) for t in times]
    return [(min(value[axis] for value in values), max(value[axis] for value in values)) for axis in (0, 1)]


def fit_runs(directories, parent_path, registry_path, *, grid_us=20000, alpha=0.25):
    require(integer(grid_us) and grid_us > 0, "时间网格须为正整数微秒")
    require(number(alpha) and 0 < alpha <= 1, "有限更新比例须在 (0,1]")
    require(len(directories) >= 5, "至少五个独立 fit Run")
    runs = [_validate(_load_run(directory)) for directory in directories]
    require(len({run["manifest"]["run_id"] for run in runs}) == len(runs), "重复 Run")
    require(len({run["payload_id"] for run in runs}) == len(runs), "重复原始记录")
    require(all(run["manifest"]["split"] == "fit" for run in runs), "holdout/test/calibration 禁止参与拟合")
    register_usage(registry_path, runs)
    identity_fields = ("parent_profile_sha256", "environment_fingerprint", "target_id", "anchor_id", "generation",
                       "weapon_id", "max_shots", "sensitivity", "x_strength", "y_strength")
    first = runs[0]["manifest"]
    require(all(all(run["manifest"][key] == first[key] for key in identity_fields) for run in runs),
            "同代拟合必须具有同一父基线、环境和冻结锚点")
    require(all(run["manifest"].get("normalized_baseline_sha256") == first.get("normalized_baseline_sha256") for run in runs),
            "规范化父曲线身份不一致")
    parent = read_json(parent_path)
    require(isinstance(parent, dict), "父曲线必须是对象")
    require(sha256(parent_path) == first["parent_profile_sha256"], "父曲线文件摘要不匹配")
    require(parent.get("schema_version") in (1, 2) and parent.get("weapon_id") == first["weapon_id"], "父曲线合同不匹配")
    points = parent.get("points")
    require(isinstance(points, list) and len(points) >= 2 and points[0] == [0, 0, 0], "父曲线必须从零开始")
    previous = -1
    for point in points:
        require(isinstance(point, list) and len(point) == 3 and all(number(v) for v in point) and point[0] > previous,
                "父曲线节点非法")
        previous = point[0]
    end = min(run["manifest"]["window_end_us"] for run in runs)
    require(points[-1][0] * 1000 >= end, "父曲线未覆盖短时域")
    # 累计阶梯输出；网格仅汇总，不平移反馈以冒充已辨识的提前量。
    grid = sorted(set([0, end, *range(grid_us, end, grid_us)]))
    require(len(grid) <= 100000, "分析网格过大")
    trajectory = []
    for t in grid:
        baseline = sample_parent(parent, t)
        residuals = []
        for run in runs:
            commands = run["commands.jsonl"]
            feedforward = cumulative(commands, t, "ff_d")
            # 调度误差使用冻结的软件时间带；只额外容许一个 count 的量化余数。
            envelope = parent_envelope(parent, t, run["manifest"].get("ff_schedule_tolerance_us", 0))
            require(all(envelope[axis][0] - 1.000001 < feedforward[axis] < envelope[axis][1] + 1.000001
                        for axis in (0, 1)),
                    "已完成前馈时序不符合父曲线，不能把遗漏的基线学习为残差")
            residuals.append(cumulative(commands, t, "fb_d"))
        center = [statistics.median(row[axis] for row in residuals) for axis in (0, 1)]
        mad = [statistics.median(abs(row[axis] - center[axis]) for row in residuals) for axis in (0, 1)]
        trajectory.append({"t_us": t, "parent_counts": baseline, "residual_median_counts": center,
                           "residual_mad_counts": mad, "n": len(runs),
                           "algebraic_parent_plus_residual_counts": [baseline[a] + alpha * center[a] for a in (0, 1)]})
    return {"schema_version": 1, "kind": "target_iteration_analysis", "algorithm": VERSION,
            "analysis_eligible": True, "message": "同父基线多轮残差分析完成；没有可执行候选或物理验收结论",
            "parent_profile_sha256": first["parent_profile_sha256"], "generation": first["generation"],
            "run_inputs": [{"run_id": run["manifest"]["run_id"], "manifest_sha256": run["hash"],
                            "payload_id": run["payload_id"], "split": "fit"} for run in runs],
            "unit": "backend_completed_or_zero_net_accounted_command_counts", "time_reference": "software_fire_start",
            "zero_net_ledger_rows": sum(command["status"] == "no_op" for run in runs for command in run["commands.jsonl"]),
            "ff_schedule_tolerance_us_by_run": {run["manifest"]["run_id"]:
                                               run["manifest"].get("ff_schedule_tolerance_us", 0) for run in runs},
            "grid_us": grid_us, "alpha": alpha, "window_end_us": end, "short_segment_only": True,
            "trajectory": trajectory, "executable_candidate_eligible": False, "physical_verified": False,
            "candidate": None, "reasons": ["独立响应/时序标定尚未建立可核验合同，禁止将延迟反馈直接写成下一轮前馈",
                                             "代数父基线加残差仅用于核对分量，没有独立物理验收，不是可执行弹道"]}


def main(argv=None):
    # GUI 后台进程与 PowerShell 使用同一 UTF-8 JSON 合同。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    inspect = sub.add_parser("inspect", help="完整性审核并冻结数据用途")
    inspect.add_argument("run", type=Path)
    inspect.add_argument("--registry", type=Path, required=True)
    inspect.add_argument("--output", type=Path)
    review = sub.add_parser("review", help="核查完整性并记录人工身份确认和观察原话")
    review.add_argument("run", type=Path)
    review.add_argument("--registry", type=Path, required=True)
    review.add_argument("--identity-confirmed", action="store_true")
    review.add_argument("--observation", required=True)
    review.add_argument("--output", type=Path)
    fit = sub.add_parser("fit", help="同父五轮中位数/MAD 分析；不导出可执行曲线")
    fit.add_argument("runs", nargs="+", type=Path)
    fit.add_argument("--parent", type=Path, required=True)
    fit.add_argument("--registry", type=Path, required=True)
    fit.add_argument("--grid-us", type=int, default=20000)
    fit.add_argument("--alpha", type=float, default=0.25)
    fit.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.output:
            require(not args.output.exists(), "报告已存在；请选择新路径以保留既有证据")
        if args.action == "inspect":
            run = _load_run(args.run)
            register_usage(args.registry, [run])
            result = inspect_run(args.run)
        elif args.action == "review":
            result = review_run(args.run, args.registry, args.observation, identity_confirmed=args.identity_confirmed)
        else:
            result = fit_runs(args.runs, args.parent, args.registry, grid_us=args.grid_us, alpha=args.alpha)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("x", encoding="utf-8") as output:
                output.write(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result.get("analysis_eligible", True) else 2
    except (ValueError, KeyError, OSError, TypeError) as exc:
        failure = {"schema_version": 1, "algorithm": VERSION, "error": str(exc), "message": str(exc),
                   "reasons": [str(exc)], "analysis_eligible": False, "executable_candidate_eligible": False,
                   "physical_verified": False, "candidate": None}
        if args.output and not args.output.exists():
            try:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                with args.output.open("x", encoding="utf-8") as output:
                    output.write(json.dumps(failure, ensure_ascii=False, indent=2) + "\n")
            except OSError:
                pass  # 保留原始拒绝原因；不能写文件时仍有 stderr 与失败退出码。
        print(json.dumps(failure, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
