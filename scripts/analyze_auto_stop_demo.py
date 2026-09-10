#!/usr/bin/env python3
"""离线导出 CS2 demo 位置证据；不产生输入，不判定服务器停稳。"""

import argparse
from collections import Counter
import importlib.metadata
import json
import math
from pathlib import Path
import sys


OPTIONAL_FIELDS = (
    "usercmd_forward_move", "usercmd_left_move", "usercmd_buttons_held",
    "usercmd_buttons_pressed", "usercmd_buttons_released",
)


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)
                    + "\n", encoding="utf-8")


def finite_number(value):
    try:
        number = float(value)
        return number if math.isfinite(number) else None
    except (ValueError, TypeError):
        return None


def timebase(header, requested):
    # 不从 parser.game_time 或相邻行号反推频率：前者实现可能固定除以 64。
    if requested is not None:
        if not math.isfinite(requested) or requested <= 0:
            raise ValueError("--tick-rate 必须为有限正数")
        return {"tick_rate": requested, "source": "explicit_cli_tick_rate"}
    for name in ("tick_rate", "tickrate"):
        rate = finite_number(header.get(name))
        if rate is not None and rate > 0:
            return {"tick_rate": rate, "source": "demo_header." + name}
    return {"tick_rate": None, "source": "UNKNOWN",
            "reason": "未找到明确 tick 频率；不假定 64 Hz"}


def summarize_positions(frame, clock):
    result = {"row_count": len(frame), "player_identity": "UNKNOWN",
              "unique_player_count": None, "players": [], "issues": []}
    required = ["tick", "X", "Y", "Z"]
    missing = [name for name in required if name not in frame.columns]
    if missing:
        result["issues"].append("缺少必要列：" + ",".join(missing))
        return result
    if frame.empty:
        result["issues"].append("没有位置样本")
        return result
    # 名字可重名；Steam ID 空或 0 时不把名字猜成唯一玩家身份。
    identity = "steamid" if "steamid" in frame.columns else None
    if identity is None:
        result["issues"].append("缺少 steamid，不能确认唯一玩家")
        return result
    valid_id = frame[identity].notna() & ~frame[identity].astype(str).isin(["0", "0.0", ""])
    if not valid_id.all():
        result["issues"].append("部分行缺少有效玩家身份")
    known = frame.loc[valid_id]
    result["unique_player_count"] = int(known[identity].nunique())
    result["player_identity"] = "KNOWN" if valid_id.all() else "PARTIAL"
    rate = clock["tick_rate"]
    for player_id, group in known.groupby(identity, sort=False):
        player = {"steamid": str(player_id), "row_count": len(group),
                  "invalid_position_rows": 0, "duplicate_ticks": 0,
                  "missing_ticks": 0, "gap_intervals": [],
                  "coordinate_change_intervals": [], "unchanged_intervals": []}
        rows = []
        for record in group[required].to_dict("records"):
            values = [finite_number(record[key]) for key in required]
            if any(value is None for value in values) or not values[0].is_integer():
                player["invalid_position_rows"] += 1
                continue
            rows.append((int(values[0]), values[1:]))
        rows.sort(key=lambda row: row[0])
        duplicates = {tick for tick, count in
                      Counter(row[0] for row in rows).items()
                      if count > 1}
        player["duplicate_ticks"] = len(duplicates)
        # 重复 tick 不取最后一行伪造唯一值；两侧也不能跨过它认定连续。
        rows = [row for row in rows if row[0] not in duplicates]
        player["first_tick"] = rows[0][0] if rows else None
        player["last_tick"] = rows[-1][0] if rows else None
        previous = None
        current = None
        for tick, position in rows:
            if previous is None:
                previous = (tick, position)
                continue
            before_tick, before_position = previous
            delta_tick = tick - before_tick
            if delta_tick != 1:
                player["missing_ticks"] += max(0, delta_tick - 1)
                player["gap_intervals"].append([before_tick, tick])
                current = None
                previous = (tick, position)
                continue
            delta = [position[i] - before_position[i] for i in range(3)]
            changed = any(value != 0 for value in delta)
            # 严格坐标相等仅表示采样值未变化，不表示物理速度为零。
            kind = "coordinate_change_intervals" if changed else "unchanged_intervals"
            if current is None or current[0] != kind or current[1]["end_tick"] != before_tick:
                interval = {"start_tick": before_tick, "end_tick": tick,
                            "sample_steps": 1, "displacement": delta,
                            "observed_path_distance": math.dist(position, before_position)}
                player[kind].append(interval)
                current = (kind, interval)
            else:
                interval = current[1]
                interval["end_tick"] = tick
                interval["sample_steps"] += 1
                interval["displacement"] = [interval["displacement"][i] + delta[i]
                                             for i in range(3)]
                interval["observed_path_distance"] += math.dist(position, before_position)
            previous = (tick, position)
        for kind in ("coordinate_change_intervals", "unchanged_intervals"):
            for interval in player[kind]:
                interval["duration_seconds"] = ((interval["end_tick"] - interval["start_tick"])
                                                / rate if rate else None)
        if player["invalid_position_rows"] or player["duplicate_ticks"] or player["missing_ticks"]:
            result["issues"].append("玩家 " + str(player_id) + " 存在缺测、重复或 tick 间断")
        result["players"].append(player)
    return result


def run(args):
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {"schema_version": 1, "status": "FAILED", "demo": str(args.demo.resolve()),
               "server_stopped": None, "fire_permitted": None,
               "observation_scope": "demo_sampled_positions",
               "limitations": ["坐标不变不证明服务器停稳或可以开枪",
                               "不使用解析器 velocity 列，不提供子 tick 瞬时速度",
                               "位置突变可能包含传送；本脚本不自动区分传送与移动"],
               "optional_fields": {}, "errors": []}
    write_json(output / "header.json", {})
    # 即使失败也留下明确空 CSV，防止上次结果被误读为本次成功。
    (output / "positions.csv").write_text("tick,X,Y,Z\n", encoding="utf-8")
    try:
        if not args.demo.is_file():
            raise FileNotFoundError("demo 文件不存在")
        if args.dependency_path:
            sys.path.insert(0, str(args.dependency_path.resolve()))
        from demoparser2 import DemoParser
        import pandas as pd

        summary["parser_version"] = importlib.metadata.version("demoparser2")
        parser = DemoParser(str(args.demo.resolve()))
        header = parser.parse_header()
        write_json(output / "header.json", header)
        clock = timebase(header, args.tick_rate)
        summary["timebase"] = clock
        positions = parser.parse_ticks(["X", "Y", "Z"])
        summary["parser_position_columns"] = list(positions.columns)
        for field in ("tick", "X", "Y", "Z"):
            if field not in positions.columns:
                positions[field] = pd.NA
        keys = [key for key in ("tick", "steamid", "name") if key in positions.columns]
        # 可选字段逐项隔离；一个版本不支持的字段不能抹掉有效位置证据。
        for field in OPTIONAL_FIELDS:
            positions[field] = pd.NA
            try:
                extra = parser.parse_ticks([field])
                if field not in extra.columns:
                    raise ValueError("解析器未返回所请求列")
                if not keys or any(key not in extra.columns for key in keys):
                    raise ValueError("缺少匹配位置样本所需标识列")
                if positions.duplicated(keys).any() or extra.duplicated(keys).any():
                    raise ValueError("样本标识重复，拒绝含糊合并")
                positions = positions.drop(columns=[field]).merge(
                    extra[keys + [field]], on=keys, how="left", validate="one_to_one")
                count = int(positions[field].notna().sum())
                summary["optional_fields"][field] = {
                    "status": "AVAILABLE" if count == len(positions) and count else
                              ("PARTIAL" if count else "UNKNOWN"), "non_null_rows": count}
            except BaseException as error:
                if isinstance(error, (KeyboardInterrupt, SystemExit)):
                    raise
                if field not in positions.columns:
                    positions[field] = pd.NA
                summary["optional_fields"][field] = {"status": "UNKNOWN", "reason": str(error)}
        positions.to_csv(output / "positions.csv", index=False, encoding="utf-8", na_rep="")
        result = summarize_positions(positions, clock)
        summary["positions"] = result
        summary["status"] = ("POSITION_DATA_AVAILABLE" if result["players"] and not result["issues"]
                             else "PARTIAL" if result["players"] else "NO_USABLE_POSITION_DATA")
    except BaseException as error:
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        summary["errors"].append(type(error).__name__ + ": " + str(error))
    write_json(output / "summary.json", summary)
    print(json.dumps({"status": summary["status"], "output_dir": str(output)}, ensure_ascii=False))
    return 1 if summary["status"] in ("FAILED", "NO_USABLE_POSITION_DATA") else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--dependency-path", type=Path)
    parser.add_argument("--tick-rate", type=float, help="已由现场证据核实的频率；输出标记为显式输入")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
