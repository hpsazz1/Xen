#!/usr/bin/env python3
"""离线核验 HUD 当前速度与位置；只报告客户端可见运动区间。"""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics


NUMBER = r"[-+]?\d+(?:\.\d+)?"


def number(value):
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (ValueError, TypeError):
        return None


def load_frames(ocr, capture):
    document = json.loads(ocr.read_text(encoding="utf-8-sig"))
    with capture.open(encoding="utf-8-sig", newline="") as stream:
        raw = list(csv.DictReader(stream))
    by_file = {row["png"]: row for row in raw if row.get("png")}
    frames = []
    seen = set()
    for item in document.get("frames", []):
        name = item.get("file", "")
        row = by_file.get(name, {})
        issues = []
        time = number(row.get("captured_at_ns"))
        pos = [number(item.get(key)) for key in ("posX", "posY", "posZ")]
        velocity = number(item.get("vel"))
        lines = item.get("visual_lines", [])
        velocity_line = next((line for line in lines if re.match(r"^\s*vel\s*:", line, re.I)), "")
        position_line = next((line for line in lines if re.match(r"^\s*pos\s*:", line, re.I)), "")
        raw_velocity = re.match(r"^\s*vel\s*:\s*(" + NUMBER + r")\s*(?:\(|$)", velocity_line, re.I)
        raw_position = re.match(r"^\s*pos\s*:\s*(" + NUMBER + r")\s+(" + NUMBER +
                                r")\s+(" + NUMBER + r")\s*$", position_line, re.I)
        if item.get("status") != "OCR_OK": issues.append("OCR_NOT_OK")
        if row.get("status") != "FRAME" or row.get("error"): issues.append("CAPTURE_INVALID")
        if time is None or time <= 0: issues.append("CAPTURE_TIME_MISSING")
        if name in seen: issues.append("DUPLICATE_PNG")
        seen.add(name)
        if velocity is None or velocity < 0 or any(value is None for value in pos):
            issues.append("VALUE_MISSING_OR_INVALID")
        if not raw_velocity or velocity != number(raw_velocity.group(1)):
            issues.append("RAW_CURRENT_VELOCITY_UNVERIFIED")
        if not raw_position or pos != [number(value) for value in raw_position.groups()]:
            issues.append("RAW_POSITION_UNVERIFIED")
        frames.append({"file": name, "png": str(capture.parent / name), "time_ns": time,
                       "position": pos, "velocity": velocity, "issues": issues})
    frames.sort(key=lambda frame: frame["time_ns"] if frame["time_ns"] is not None else -1)
    for left, right in zip(frames, frames[1:]):
        if left["time_ns"] == right["time_ns"]:
            left["issues"].append("DUPLICATE_TIMESTAMP")
            right["issues"].append("DUPLICATE_TIMESTAMP")
    return document, frames


def analyze(frames, group_gap, stable_seconds, sample_gap, precision):
    # 相邻零显示却发生超过量化界的位置变化，不能拿该零显示当停稳。
    for left, right in zip(frames, frames[1:]):
        if not left["issues"] and not right["issues"] and left["velocity"] == right["velocity"] == 0:
            if any(abs(a - b) > 2 * precision + 1e-9
                   for a, b in zip(left["position"], right["position"])):
                right["issues"].append("ZERO_VELOCITY_POSITION_CONTRADICTION")
    movement = [index for index, frame in enumerate(frames)
                if not frame["issues"] and frame["velocity"] > 0]
    groups = []
    for index in movement:
        if not groups or (frames[index]["time_ns"] - frames[groups[-1][-1]]["time_ns"]) / 1e9 > group_gap:
            groups.append([])
        groups[-1].append(index)
    output = []
    for group_index, group in enumerate(groups):
        start, end = group[0], group[-1]
        first, last = frames[start], frames[end]
        limit = groups[group_index + 1][0] if group_index + 1 < len(groups) else len(frames)
        previous = frames[start - 1] if start else None
        trial = {"group": group_index + 1, "status": "UNRESOLVED", "first_motion_png": first["png"],
                 "last_motion_png": last["png"], "first_motion_ns": first["time_ns"],
                 "last_motion_ns": last["time_ns"], "first_persistent_zero_ns": None,
                 "visible_motion_span_seconds": (last["time_ns"] - first["time_ns"]) / 1e9,
                 "start_bracket_ns": ([previous["time_ns"], first["time_ns"]]
                                      if previous and not previous["issues"] and previous["velocity"] == 0
                                      and first["time_ns"] - previous["time_ns"] <= sample_gap * 1e9 else None),
                 "stop_bracket_ns": None, "overshoot": None, "issues": []}
        segment = frames[start:limit]
        trial["invalid_samples_in_group_and_tail"] = sum(bool(frame["issues"]) for frame in segment)
        run = []
        stable = None
        for index in range(end + 1, limit):
            frame = frames[index]
            if frame["issues"] or frame["velocity"] != 0:
                run = []
                continue
            if run and frame["time_ns"] - run[-1]["time_ns"] > sample_gap * 1e9:
                run = []
            run.append(frame)
            if any(max(item["position"][axis] for item in run) -
                   min(item["position"][axis] for item in run) > 2 * precision + 1e-9 for axis in range(3)):
                run = [frame]
            if (frame["time_ns"] - run[0]["time_ns"]) / 1e9 >= stable_seconds:
                stable = list(run)
                break
        if stable:
            stable_first = stable[0]
            trial["first_persistent_zero_ns"] = stable_first["time_ns"]
            trial["persistent_zero_png"] = stable_first["png"]
            trial["stable_window_end_png"] = stable[-1]["png"]
            trial["stable_window_seconds"] = (stable[-1]["time_ns"] - stable_first["time_ns"]) / 1e9
            trial["stop_bracket_ns"] = [last["time_ns"], stable_first["time_ns"]]
            trial["stop_bracket_width_seconds"] = (stable_first["time_ns"] - last["time_ns"]) / 1e9
            trial["first_motion_to_zero_seconds"] = (stable_first["time_ns"] - first["time_ns"]) / 1e9
            trial["visible_duration_interval_seconds"] = (
                [(last["time_ns"] - first["time_ns"]) / 1e9,
                 (stable_first["time_ns"] - trial["start_bracket_ns"][0]) / 1e9]
                if trial["start_bracket_ns"] else None)
            trial["status"] = "CLIENT_ZERO_AND_POSITION_STABLE"
            tail = frames[frames.index(stable[0]):limit]
            valid_tail = [frame for frame in tail if not frame["issues"]]
            tail_drift = any(max(frame["position"][axis] for frame in valid_tail) -
                             min(frame["position"][axis] for frame in valid_tail) > 2 * precision + 1e-9
                             for axis in range(3))
            tail_missing = any(frame["issues"] for frame in tail) or any(
                right["time_ns"] - left["time_ns"] > sample_gap * 1e9
                for left, right in zip(tail, tail[1:]))
            trial["tail_verification"] = {
                "last_observed_png": tail[-1]["png"], "last_observed_ns": tail[-1]["time_ns"],
                "position_left_initial_stable_box": tail_drift,
                "has_missing_or_invalid_observations": tail_missing,
                "final_stability_supported": not tail_drift and not tail_missing}
            trial["first_zero_window_position_xyz"] = [
                statistics.median(frame["position"][axis] for frame in stable) for axis in range(3)]
            trial["final_stable_position_xyz"] = (trial["first_zero_window_position_xyz"]
                                                   if not tail_drift and not tail_missing else None)
            invalid_inside = any(frame["issues"] for frame in frames[start:frames.index(stable_first) + 1])
            if invalid_inside:
                trial["issues"].append("运动或停止边界存在不可用样本，区间可能包含缺测")
                trial["status"] = "PARTIAL"
            if trial["stop_bracket_width_seconds"] > sample_gap:
                trial["issues"].append("最后运动到持续零之间有较大采样间隙")
                trial["status"] = "PARTIAL"
            origin = previous if previous and not previous["issues"] else first
            observed = [frame for frame in frames[start:frames.index(stable[-1]) + 1] if not frame["issues"]]
            # 每轴保留正向峰值和负向谷值到终点的回退，避免初始单轴投影遮住组合另一轴。
            trial["axis_extrema_to_final"] = {}
            for axis, label in enumerate(("x", "y")):
                final_axis = statistics.median(frame["position"][axis] for frame in stable)
                maximum = max(observed, key=lambda frame: frame["position"][axis])
                minimum = min(observed, key=lambda frame: frame["position"][axis])
                high_retreat = maximum["position"][axis] - final_axis
                low_retreat = final_axis - minimum["position"][axis]
                error_axis = 2 * precision
                trial["axis_extrema_to_final"][label] = {
                    "final_position": final_axis,
                    "maximum_png": maximum["png"], "minimum_png": minimum["png"],
                    "maximum_to_final": high_retreat, "final_minus_minimum": low_retreat,
                    "maximum_to_final_quantization_interval": [max(0, high_retreat - error_axis), max(0, high_retreat + error_axis)],
                    "final_minus_minimum_quantization_interval": [max(0, low_retreat - error_axis), max(0, low_retreat + error_axis)],
                    "note": "两种方向的极值到终点差；须结合该轴原运动方向解释，不能把全程位移误叫过冲"}
            direction = None
            for index in group:
                vector = [frames[index]["position"][axis] - origin["position"][axis] for axis in (0, 1)]
                length = math.hypot(*vector)
                if length > 2 * precision * math.sqrt(2):
                    direction = [value / length for value in vector]
                    break
            if direction:
                values = [(sum((frame["position"][axis] - origin["position"][axis]) * direction[axis]
                               for axis in (0, 1)), frame)
                          for frame in frames[start:frames.index(stable[-1]) + 1] if not frame["issues"]]
                peak, peak_frame = max(values, key=lambda value: value[0])
                final_position = [statistics.median(frame["position"][axis] for frame in stable) for axis in (0, 1)]
                final = sum((final_position[axis] - origin["position"][axis]) * direction[axis] for axis in (0, 1))
                error = 2 * precision * sum(abs(value) for value in direction)
                trial["overshoot"] = {"direction_xy": direction, "peak_png": peak_frame["png"],
                    "final_position_xy": final_position, "observed_distance": peak - final,
                    "quantization_error_bound": error, "distance_interval": [max(0, peak - final - error), max(0, peak - final + error)],
                    "resolved_reverse_displacement": peak - final > error,
                    "interval_scope": "固定估计方向下的位置量化区间，不是总体置信界",
                    "direction_note": "方向来自初始小位移，其量化角度误差未传播；组合必须另查独立XY极值",
                    "limitation": "离散可见过冲，不保证捕获两帧之间的峰值"}
            else:
                trial["issues"].append("初始位移不足以确定方向，过冲未知")
            if tail_drift or tail_missing:
                trial["status"] = "PARTIAL"
                trial["issues"].append("首个零显示窗口之后发生位置漂移或观测缺失，最终稳定位置未知")
                # 首次零窗口仍保留；最终位置未知时不能给出以该位置为终点的过冲。
                trial["overshoot"] = None
                trial["axis_extrema_to_final"] = None
        else:
            trial["issues"].append("尾部缺少足够长的精确零速度与位置稳定段")
        good_times = [frame["time_ns"] for frame in segment if not frame["issues"]]
        trial["max_valid_sample_gap_seconds"] = max(((b - a) / 1e9 for a, b in zip(good_times, good_times[1:])), default=None)
        output.append(trial)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ocr", type=Path, required=True)
    parser.add_argument("--frames-csv", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--group-gap-seconds", type=float, default=0.5)
    parser.add_argument("--stable-seconds", type=float, default=0.1)
    parser.add_argument("--max-sample-gap-seconds", type=float, default=0.05)
    parser.add_argument("--position-precision", type=float, default=0.01)
    args = parser.parse_args()
    for value in (args.group_gap_seconds, args.stable_seconds, args.max_sample_gap_seconds, args.position_precision):
        if not math.isfinite(value) or value <= 0: parser.error("测量参数必须为有限正数")
    if args.output_dir.exists(): parser.error("输出必须为新目录")
    document = json.loads(args.ocr.read_text(encoding="utf-8-sig"))
    capture = args.frames_csv or Path(document["input_directory"]) / "frames.csv"
    document, frames = load_frames(args.ocr, capture)
    groups = analyze(frames, args.group_gap_seconds, args.stable_seconds,
                     args.max_sample_gap_seconds, args.position_precision)
    invalid = [{"png": frame["png"], "issues": frame["issues"]} for frame in frames if frame["issues"]]
    result = {"schema_version": 1, "status": "OBSERVATIONS_AVAILABLE" if groups else "NO_MOTION_GROUPS",
              "ocr": str(args.ocr.resolve()), "frames_csv": str(capture.resolve()), "frame_count": len(frames),
              "invalid_frames": invalid, "groups": groups, "server_stopped": None, "fire_permitted": None,
              "parameters": {"group_gap_seconds": args.group_gap_seconds, "stable_seconds": args.stable_seconds,
                             "max_sample_gap_seconds": args.max_sample_gap_seconds, "position_precision": args.position_precision},
              "limitations": ["分组间隔只用于试验切段，不是生产速度阈值", "精确显示零不证明实际瞬时速度为零",
                              "时间仅为本机 captured_at_ns，不是 release 到 settled", "未使用括号中的3秒峰值",
                              "姿态变化、人工混入及方向量化须配合PNG人工复核",
                              "投影过冲区间仅条件于固定估计方向，不包含方向误差，不能当总体置信界",
                              "组合初始方向可能只有单轴；独立XY极值仍需按轴运动方向解释"]}
    args.output_dir.mkdir(parents=True)
    (args.output_dir / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    lines = ["# HUD 客户端运动观察", "", f"样本 {len(frames)}；不可用 {len(invalid)}；运动分组 {len(groups)}。", "",
             "时间从可见运动首帧起算，不是松键到停稳；括号峰值未参与。", "",
             "投影区间仅包含固定估计方向下的位置量化，不含方向误差，不能当总体置信界。组合须另查 summary 的独立 XY 极值。", "",
             "| 组 | 状态 | 首运动至末运动 s | 首运动至持续零 s | 固定方向下回退量化区间 |", "|---|---|---:|---:|---|"]
    for group in groups:
        over = group["overshoot"]
        lines.append(f"| {group['group']} | {group['status']} | {group['visible_motion_span_seconds']:.6f} | "
                     f"{group.get('first_motion_to_zero_seconds', '未知')} | {over['distance_interval'] if over else '未知'} |")
    lines += ["", "人工抽查："]
    for group in groups:
        for key in ("first_motion_png", "last_motion_png", "persistent_zero_png"):
            if key in group: lines.append(f"- 第 {group['group']} 组 {key}：[PNG](<{Path(group[key]).as_posix()}>)")
    (args.output_dir / "RESULT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "groups": len(groups), "invalid_frames": len(invalid)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
