"""将完整Debug JSON与明确人工测量整理为离线Tuner数据集。

用法：python scripts/build_recoil_dataset.py manifest.json new-dataset.json
manifest的schema_version为1，字段如下：
  base: {debug_report: "run.json", profile_key: "profile-id:revision"}
  trials: [{id, use: "fit"|"holdout", debug_report, source_run, firing_id,
    completed: true, independent_recoil: true, timing_valid: true,
    previously_used_generation: 0,
    measurement: {source: "human-measurement.txt", clock_domain: "local_steady",
      at_steady_ns, residual: [x_pixels,y_pixels], noise: [x_pixels,y_pixels],
      timing_uncertainty_ms, reference_confirmed: true}}]
  response_experiments: [{id, baseline: <不含use的trial>, changed: <不含use的trial>}]
source_run必须等于Debug根session_id，路径相对manifest所在目录解析。
响应变化必须逐节点符合terminal_smoothstep_v1。counts、曲线身份、相位限制、
时钟与回执只从Debug取得；测量必须明确提供人工证据，不从GSI或counts推造。
completed是人工确认项：尾段为零时，仅凭回执闭合不能证明自然结束。
schema2批次文件另需完整BEGIN/END与连续事件序号；自然耗尽仍不代替人工测量。
同一次采集Run的不同批次或模型分段不能用作独立训练与留出。
不使用游戏、网络、设备或活动配置。跨代留出用途由Tuner登记表负责。
"""
import argparse
import bisect
import hashlib
import json
import math
import os
from pathlib import Path

MAX_BYTES = 16 * 1024 * 1024
MAX_BATCH_RECORDS = 32768


def require(ok, message):
    if not ok:
        raise ValueError(message)


def integer(value, name, minimum=0, maximum=(1 << 63) - 1):
    require(type(value) is int and minimum <= value <= maximum, name + " 必须为范围内整数")
    return value


def number(value, name, minimum=-1e7, maximum=1e7):
    require(type(value) in (int, float) and math.isfinite(value) and minimum <= value <= maximum,
            name + " 必须为范围内有限数")
    return value


def text(value, name):
    require(type(value) is str and 0 < len(value) <= 4096, name + " 必须明确填写")
    return value


def pair(value, name, minimum=-1e7, maximum=1e7):
    require(type(value) is list and len(value) == 2, name + " 必须为两轴数组")
    return [number(x, name, minimum, maximum) for x in value]


def fields(value, allowed, required=None):
    require(type(value) is dict, "需要JSON对象")
    require(not (set(value) - set(allowed)), "不允许额外字段: " + ",".join(sorted(set(value) - set(allowed))))
    require(set(required or allowed) <= set(value), "缺少必填字段")


def read_bytes(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    require(0 < len(data) <= MAX_BYTES, "输入为空或超过16MiB")
    return data


def unique_object(items):
    result = {}
    for key, value in items:
        require(key not in result, "JSON重复字段: " + key)
        result[key] = value
    return result


def load_json(path):
    data = read_bytes(path)
    value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("非有限JSON数值")))
    stack = [(value, 0)]
    while stack:
        item, depth = stack.pop()
        require(depth <= 20, "JSON深度超过20")
        if isinstance(item, dict):
            stack.extend((x, depth + 1) for x in item.values())
        elif isinstance(item, list):
            stack.extend((x, depth + 1) for x in item)
    return value, hashlib.sha256(data).hexdigest()


def profile_points(profile):
    require(type(profile["schema_version"]) is int and profile["schema_version"] == 1 and profile["unit"] == "device_counts" and
            profile["sample_semantics"] == "cumulative" and profile["fire_mode"] == "automatic" and
            profile["state"] in ("CALIBRATED", "ACCEPTED"), "Debug曲线必须是已校准累计counts自动模式")
    text(profile["environment_fingerprint"], "Debug环境指纹")
    integer(profile["revision"], "profile revision", 1)
    phase = number(profile["phase_tolerance_ms"], "相位容差", 0, 1000)
    require(phase > 0, "相位容差缺失")
    points = profile["points"]
    require(type(points) is list and 2 <= len(points) <= 100000 and points[0] == [0, 0, 0], "曲线起点或数量无效")
    previous = -1
    for p in points:
        require(type(p) is list and len(p) == 3, "曲线节点格式无效")
        time = number(p[0], "节点时间", 0, 60000)
        require(time > previous, "曲线时间必须严格递增")
        number(p[1], "节点X"); number(p[2], "节点Y")
        previous = time
    return points


def sample(points, time):
    if time >= points[-1][0]:
        return points[-1][1:]
    right = bisect.bisect_right(points, [time, math.inf, math.inf])
    if right == 0:
        return [0, 0]
    a, b = points[right - 1], points[right]
    f = (time - a[0]) / (b[0] - a[0])
    return [a[axis] + (b[axis] - a[axis]) * f for axis in (1, 2)]


class PhaseEnvelope:
    """Range extrema tree: bounded O(points + receipts * log(points)) lookups."""
    def __init__(self, points, phase):
        self.points, self.phase = points, phase
        self.times = [p[0] for p in points]
        self.size = 1 << (len(points) - 1).bit_length()
        self.low = [[math.inf, math.inf] for _ in range(2 * self.size)]
        self.high = [[-math.inf, -math.inf] for _ in range(2 * self.size)]
        for i, p in enumerate(points):
            self.low[self.size + i] = self.high[self.size + i] = p[1:]
        for i in range(self.size - 1, 0, -1):
            self.low[i] = [min(self.low[2 * i][a], self.low[2 * i + 1][a]) for a in range(2)]
            self.high[i] = [max(self.high[2 * i][a], self.high[2 * i + 1][a]) for a in range(2)]

    def matches(self, counts, time):
        a, b = sample(self.points, time - self.phase), sample(self.points, time + self.phase)
        low, high = [min(a[i], b[i]) for i in range(2)], [max(a[i], b[i]) for i in range(2)]
        left = bisect.bisect_left(self.times, time - self.phase) + self.size
        right = bisect.bisect_right(self.times, time + self.phase) + self.size
        while left < right:
            for node in ([left] if left & 1 else []) + ([right - 1] if right & 1 else []):
                low = [min(low[i], self.low[node][i]) for i in range(2)]
                high = [max(high[i], self.high[node][i]) for i in range(2)]
            left = (left + 1) // 2
            right //= 2
        return all(low[i] - 1 <= counts[i] <= high[i] + 1 for i in range(2))


class Builder:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.runs = set()
        self.hashes = set()
        self.ids = set()

    def debug(self, filename):
        root, digest = load_json(self.directory / text(filename, "Debug路径"))
        recoil = root["recoil"]
        require(type(recoil["schema"]) is int and recoil["schema"] in (1, 2) and recoil["config"]["enabled"] is True and
                recoil["config"]["mixed_aim"] is False, "必须是已启用独立压枪Debug schema1或完整批次schema2")
        execution = recoil["execution"]
        require(execution["clock_domain"] == "local_steady", "执行时钟域未知")
        if recoil["schema"] == 1:
            # 旧报告不具有批次边界，不能用新语义放宽其全局覆盖拒绝。
            require(type(execution["dropped_count"]) is int and execution["dropped_count"] == 0,
                    "执行记录缺失或丢弃")
            require(type(execution["records"]) is list and 1 <= len(execution["records"]) <= 2048,
                    "完整有界执行记录缺失")
        else:
            require(text(root["acquisition_run_id"], "原始采集Run") == text(root["session_id"], "批次Run"),
                    "批次文件不能把模型分段或文件名作为新的采集Run")
            self.complete_batch(execution)
        profiles = {}
        for p in execution["profiles"]:
            key = text(p["id"], "profile id") + ":" + str(integer(p["revision"], "revision", 1))
            require(key not in profiles, "Debug重复profile key")
            profiles[key] = p
        return root, recoil, profiles, digest

    @staticmethod
    def complete_batch(execution):
        records, batch = execution["records"], execution["batch"]
        require(type(records) is list and 1 <= len(records) <= MAX_BATCH_RECORDS,
                "批次需要有界非空命令；零命令批次不能提供优化回执")
        require(batch["coverage_complete"] is True and batch["end_reason"] == "EXHAUSTED",
                "批次覆盖缺口、取消或未知结束不可用于优化")
        require(batch.get("missing_sequence_ranges", []) == [] and batch.get("incomplete_reason", "") == "",
                "批次完整标记与缺口或不完整原因矛盾")
        begin = integer(batch["begin_sequence"], "BEGIN序号", 1)
        end = integer(batch["end_sequence"], "END序号", 1)
        count = integer(batch["records_count"], "批次命令数", 1, MAX_BATCH_RECORDS)
        require(count == len(records) and end - begin - 1 == count, "批次BEGIN/END或命令序号存在缺口")
        begin_at = integer(batch["begin_at_steady_ns"], "BEGIN时间", 1)
        end_at = integer(batch["end_at_steady_ns"], "END时间", 1)
        started = integer(batch["firing_started_at_steady_ns"], "批次起点", 1)
        require(started <= begin_at <= end_at, "批次开始结束时序无效")
        identity = (integer(batch["firing_id"], "批次firing id", 1), text(batch["profile"], "批次profile"),
                    integer(batch["weapon_generation"], "批次武器代际", 1),
                    integer(batch["device_epoch"], "批次设备代际", 1), started)
        source = text(batch["firing_source"], "批次射击来源")
        require(source in ("INPUT_ESTIMATED", "COMMAND_ESTIMATED"), "批次射击来源未知")
        for index, record in enumerate(records):
            require(integer(record["event_sequence"], "命令事件序号", 1) == begin + index + 1,
                    "批次事件缺失、重复或乱序")
            current = (integer(record["firing_id"], "命令firing id", 1), record["profile"],
                       integer(record["weapon_generation"], "命令武器代际", 1),
                       integer(record["device_epoch"], "命令设备代际", 1),
                       integer(record["firing_started_at_steady_ns"], "命令射击起点", 1))
            require(current == identity and record["firing_source"] == source,
                    "批次边界与命令的身份、代际、来源或起点不一致")
            require(integer(record["completed_at_steady_ns"], "批次命令完成时间", 1) <= end_at,
                    "END早于命令完成")

    def trial(self, entry, use=None):
        keys = {"id", "debug_report", "source_run", "firing_id", "completed", "independent_recoil",
                "timing_valid", "previously_used_generation", "measurement"}
        if use is not None:
            keys.add("use")
        fields(entry, keys)
        for flag in ("completed", "independent_recoil", "timing_valid"):
            require(entry[flag] is True, flag + " 必须由用户明确确认")
        trial_id = text(entry["id"], "Trial id")
        run = text(entry["source_run"], "source_run")
        firing = integer(entry["firing_id"], "firing_id", 1)
        root, recoil, profiles, digest = self.debug(entry["debug_report"])
        require(text(root["session_id"], "Debug session_id") == run, "manifest Run与Debug session_id不匹配")
        require(trial_id not in self.ids and run not in self.runs and digest not in self.hashes,
                "Run/内容重复；不能把同一份Debug拆为独立训练或留出")
        self.ids.add(trial_id); self.runs.add(run); self.hashes.add(digest)
        measurement = entry["measurement"]
        fields(measurement, {"source", "clock_domain", "at_steady_ns", "residual", "noise",
                             "timing_uncertainty_ms", "reference_confirmed"})
        require(measurement["reference_confirmed"] is True and measurement["clock_domain"] == "local_steady",
                "需人工确认参考及Debug本地steady时钟域")
        measured = integer(measurement["at_steady_ns"], "测量完成时间", 1)
        evidence_path = (self.directory / text(measurement["source"], "测量来源")).resolve()
        evidence_hash = hashlib.sha256(read_bytes(evidence_path)).hexdigest()
        residual = pair(measurement["residual"], "人工残差")
        noise = pair(measurement["noise"], "人工噪声", 0, 1e7)
        require(all(x > 0 for x in noise), "测量噪声必须正值，不能缺测填零")
        uncertainty = number(measurement["timing_uncertainty_ms"], "测量时序不确定度", 0, 100)
        selected, command_ids = [], set()
        for record in recoil["execution"]["records"]:
            cid = integer(record["command_id"], "command_id", 1)
            require(cid not in command_ids, "Debug重复command id")
            command_ids.add(cid)
            if integer(record["firing_id"], "record firing_id", 1) == firing:
                selected.append(record)
        require(selected, "没有选定射击段的执行记录")
        profile_key = selected[0]["profile"]
        require(profile_key in profiles, "记录的profile快照缺失")
        profile = profiles[profile_key]
        points = profile_points(profile)
        start = integer(selected[0]["firing_started_at_steady_ns"], "射击起始时间", 1)
        source = selected[0]["firing_source"]
        require(source in ("INPUT_ESTIMATED", "COMMAND_ESTIMATED"), "射击事件来源未知")
        phase = profile["phase_tolerance_ms"]
        if recoil["schema"] == 2:
            batch = recoil["execution"]["batch"]
            require(batch["firing_id"] == firing and batch["profile"] == profile_key,
                    "所选firing与归档批次不一致")
            require(start + points[-1][0] * 1e6 <= batch["end_at_steady_ns"] <= measured,
                    "自然结束时间未覆盖曲线或晚于人工测量")
        envelope = PhaseEnvelope(points, phase)
        counts, receipts = [0, 0], []
        vertex = 0
        last_planned, last_completed, last_id = start, start, 0
        identity = (selected[0]["weapon_generation"], selected[0]["device_epoch"])
        require(all(type(x) is int and x > 0 for x in identity), "设备/武器代际缺失")
        for r in selected:
            require(r["profile"] == profile_key and r["firing_started_at_steady_ns"] == start and
                    r["firing_source"] == source and (r["weapon_generation"], r["device_epoch"]) == identity,
                    "同一射击段存在profile/时间/来源/代际切换")
            require(r["receipt"] == "ACKNOWLEDGED" and r["backend_called"] is True,
                    "UNKNOWN/NOT_SENT/未调用后端不允许进入Dataset")
            planned = integer(r["planned_at_steady_ns"], "planned steady", 1)
            completed = integer(r["completed_at_steady_ns"], "completed steady", 1)
            expires = integer(r["expires_at_steady_ns"], "expires steady", 1)
            require(r["command_id"] > last_id and (last_id == 0 or r["command_id"] == last_id + 1) and
                    planned >= last_planned and completed >= last_completed and
                    start <= planned <= completed <= expires and completed <= measured,
                    "命令顺序或源时相无效")
            # Debug only stores nonzero intents; zero plateaus do not provide
            # scheduler samples, so gaps between intents are not lateness proof.
            require((completed - planned) / 1e6 <= phase, "完成延迟超出profile相位容差")
            require(abs((expires - planned) / 1e6 - phase) < 1e-5, "意图期限与所选profile不一致")
            requested = r["requested_counts"]
            require(type(requested) is list and len(requested) == 2, "requested counts缺失")
            delta = [integer(x, "设备counts", -32767, 32767) for x in requested]
            planned_ms, completed_ms = (planned - start) / 1e6, (completed - start) / 1e6
            require(planned_ms <= points[-1][0] + phase, "命令晚于曲线终点窗口")
            while vertex < len(points) and points[vertex][0] < completed_ms:
                require(envelope.matches(counts, points[vertex][0]), "回执间曲线节点超出相位包络")
                vertex += 1
            require(envelope.matches(counts, completed_ms), "回执前累计位移超出相位包络")
            counts = [counts[i] + delta[i] for i in range(2)]
            expected = sample(points, planned_ms)
            require(all(abs(counts[i] - expected[i]) < 1 for i in range(2)), "实际累计counts与逐时刻曲线不匹配")
            require(envelope.matches(counts, completed_ms), "回执后累计位移超出相位包络")
            receipts.append({"command_id": r["command_id"], "planned_ms": planned_ms, "completed_ms": completed_ms,
                             "x_counts": delta[0], "y_counts": delta[1], "state": "acknowledged"})
            last_planned, last_completed, last_id = planned, completed, r["command_id"]
        measurement_ms = (measured - start) / 1e6
        require(points[-1][0] <= measurement_ms <= 3600000 and
                all(abs(counts[i] - points[-1][i + 1]) < 1 for i in range(2)), "人工确认完整射击与回执终点不闭合")
        previous = integer(entry["previously_used_generation"], "已使用代际")
        require(use != "holdout" or previous == 0, "已使用留出不能重新标为独立")
        trial = {"id": trial_id, "content_hash": digest, "firing_id": run + ":" + str(firing), "source_run": run,
                 "environment_fingerprint": profile["environment_fingerprint"], "executed_profile_revision": str(profile["revision"]),
                 "measurement_source": str(evidence_path), "measurement_evidence_sha256": evidence_hash,
                 "use": use or "fit", "previously_used_generation": previous,
                 "completed": True, "completion_evidence": "controller_exhausted_and_human_confirmation_and_receipt_closure"
                 if recoil["schema"] == 2 else "human_confirmation_and_receipt_closure",
                 "independent_recoil": True, "timing_valid": True, "reference_confirmed": True,
                 "measurement_ms": measurement_ms, "timing_uncertainty_ms": uncertainty,
                 "residual": residual, "noise": noise, "executed_counts": counts, "receipts": receipts}
        return trial, profile


def build_dataset(manifest_path):
    manifest_path = Path(manifest_path)
    manifest, manifest_hash = load_json(manifest_path)
    fields(manifest, {"schema_version", "base", "trials", "response_experiments"})
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == 1, "manifest schema不支持")
    fields(manifest["base"], {"debug_report", "profile_key"})
    builder = Builder(manifest_path.parent)
    _, _, profiles, _ = builder.debug(manifest["base"]["debug_report"])
    base = profiles[manifest["base"]["profile_key"]]
    points = profile_points(base)
    require(type(manifest["trials"]) is list and 5 <= len(manifest["trials"]) <= 10000, "需要独立训练及留出Run")
    require(type(manifest["response_experiments"]) is list and 3 <= len(manifest["response_experiments"]) <= 10000,
            "需要至少3组明确响应差异实验，不自动合成H")
    trials = []
    for entry in manifest["trials"]:
        use = entry["use"]
        require(use in ("fit", "holdout"), "每Run必须显式fit/holdout")
        trial, p = builder.trial(entry, use)
        require(p == base, "训练/留出执行profile快照不等于选定基线")
        trials.append(trial)
    require(sum(t["use"] == "fit" for t in trials) >= 3 and sum(t["use"] == "holdout" for t in trials) >= 2,
            "至少3个fit与2个独立holdout")
    experiments, ids = [], set()
    for item in manifest["response_experiments"]:
        fields(item, {"id", "baseline", "changed"})
        experiment_id = text(item["id"], "响应实验id")
        require(experiment_id not in ids, "响应实验重复")
        ids.add(experiment_id)
        before, p = builder.trial(item["baseline"])
        after, q = builder.trial(item["changed"])
        require(p == base and q["environment_fingerprint"] == base["environment_fingerprint"] and
                len(q["points"]) == len(points), "响应实验基线/环境/时间基函数不匹配")
        delta = [after["executed_counts"][i] - before["executed_counts"][i] for i in range(2)]
        require(math.hypot(*delta) > 0, "响应实验没有实际输入差异")
        for b, c in zip(points, q["points"]):
            require(b[0] == c[0], "响应实验不能同时修改时序")
            t = b[0] / points[-1][0]
            weight = t * t * (3 - 2 * t)
            require(all(abs(c[i + 1] - b[i + 1] - delta[i] * weight) <= 1e-6 for i in range(2)),
                    "实际输入差异不是terminal_smoothstep_v1，不可代入此H")
        experiments.append({"id": experiment_id, "environment_fingerprint": base["environment_fingerprint"],
            "baseline_run": before["source_run"], "changed_run": after["source_run"],
            "baseline_hash": before["content_hash"], "changed_hash": after["content_hash"], "basis": "terminal_smoothstep_v1",
            "delta_counts": delta, "delta_residual": [after["residual"][i] - before["residual"][i] for i in range(2)],
            # Conservative sum; does not assume independent measurement errors.
            "noise": math.hypot(*before["noise"]) + math.hypot(*after["noise"]),
            "baseline_timing_uncertainty_ms": before["timing_uncertainty_ms"],
            "changed_timing_uncertainty_ms": after["timing_uncertainty_ms"],
            "acknowledged": True, "timing_valid": True,
            "baseline_measurement_source": before["measurement_source"], "changed_measurement_source": after["measurement_source"]})
    return {"schema_version": 1, "environment_fingerprint": base["environment_fingerprint"],
            "phase_tolerance_ms": base["phase_tolerance_ms"], "base_profile_revision": str(base["revision"]),
            "base_curve": [{"time_ms": p[0], "x_counts": p[1], "y_counts": p[2]} for p in points],
            "trials": trials, "response_experiments": experiments,
            "provenance": {"manifest_sha256": manifest_hash, "physical_acceptance": None,
                           "completion": "explicit human confirmation plus software receipt closure"}}


def write_dataset(manifest, output):
    dataset = build_dataset(manifest)
    content = json.dumps(dataset, ensure_ascii=False, allow_nan=False, indent=2).encode("utf-8")
    require(len(content) <= MAX_BYTES, "Dataset超过Tuner的16MiB上限")
    with Path(output).open("xb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        write_dataset(args.manifest, args.output)
    except (ValueError, KeyError, TypeError, OSError, RecursionError, OverflowError) as error:
        parser.exit(1, "拒绝生成Dataset：" + str(error) + "\n")
    print("已写入独立Dataset；未执行设备，未生成或激活压枪候选。")


if __name__ == "__main__":
    main()
