"""显式本地导入旧增量CSV为未校准累计候选；不会发送设备输入。"""
import argparse
import csv
import hashlib
import io
import json
import math
import re
from pathlib import Path


def load_weapon_names(data_root):
    # C++ 与导入器读取同一个目录，不另维护一份 M4A4/M4A1-S 映射。
    rows = {}
    for line in (data_root / "assets/weapon_catalog.inc").read_text(encoding="utf-8-sig").splitlines():
        if not line.strip() or line.lstrip().startswith("//"):
            continue
        match = re.fullmatch(r'XEN_WEAPON\("([^"\n]+)", "([^"\n]+)", "([^"\n]+)"\)', line)
        if not match:
            raise ValueError("武器名称目录格式无效")
        canonical, gsi, display = match.groups()
        for alias in (canonical, gsi, display):
            key = alias.casefold()
            if key in rows and rows[key] != canonical:
                raise ValueError("武器名称目录别名冲突")
            rows[key] = canonical
    if not rows:
        raise ValueError("武器名称目录为空")
    return rows


def convert(raw, entry, manifest, sensitivity):
    if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
        raise ValueError("CSV SHA256与已审计版本不符: " + entry["file"])
    if len(raw) > 1024 * 1024:
        raise ValueError("CSV超过1MiB")
    points = [[0.0, 0.0, 0.0]]
    t = x = y = 0.0
    rows = list(csv.reader(io.StringIO(raw.decode("utf-8-sig"))))
    if not 1 <= len(rows) <= 100000:
        raise ValueError("行数无效")
    for row in rows:
        if len(row) != 3:
            raise ValueError("每行必须为delta_x,delta_y,delay_ms")
        dx, dy, delay = map(float, row)
        duration = entry["multiple"] * (delay / entry["sleep_divider"] - entry["sleep_suber_ms"])
        if not all(map(math.isfinite, (dx, dy, duration))) or duration <= 0:
            raise ValueError("位移须有限且转换周期须为正")
        t += duration
        x += dx * 2.45 / sensitivity
        y -= dy * 2.45 / sensitivity
        if t > 60000 or abs(x) > 1e7 or abs(y) > 1e7:
            raise ValueError("累计曲线越界")
        points.append([t, x, y])
    return {
        "schema_version": 1, "id": entry["id"], "revision": 1,
        "weapon_id": entry["canonical_weapon_id"], "unit": "device_counts", "sample_semantics": "cumulative",
        "fire_mode": "automatic", "state": "IMPORTED",
        "source": {"repository": manifest["repository"], "commit": manifest["commit"],
                   "sha256": entry["sha256"], "license": "MIT_REPOSITORY_DATA_PROVENANCE_UNRESOLVED",
                   "source_unit": "legacy_relative_delta_empirical_2.45_over_sensitivity_y_inverted",
                   "conversion_revision": "cumulative-v1-all-rows-no-skipped-substep",
                   "redistribution_verified": False},
        "calibration": {"game_build": "", "input_path": "", "conditions": "", "evidence": "", "sensitivity": sensitivity},
        "phase_tolerance_ms": None, "recovery_ms": None, "points": points,
        "import_report": {"row_count": len(rows), "legacy_length": entry["legacy_length"],
                          "duration_ms": t, "total_x_counts": x, "total_y_counts": y,
                          "legacy_multiple": entry["multiple"], "legacy_sleep_divider": entry["sleep_divider"],
                          "legacy_sleep_suber_ms": entry["sleep_suber_ms"],
                          "difference": "保留全部CSV行；不随机化、不跳过首子步；周期不是武器射速；未校准"}}


def convert_discrete(raw, entry, manifest, sensitivity):
    """按旧生产离散编译语义重建；默认仍是待确认版本。"""
    if not math.isfinite(sensitivity) or sensitivity <= 0:
        raise ValueError("灵敏度须有限且为正")
    if len(raw) > 1024 * 1024 or hashlib.sha256(raw).hexdigest() != entry["sha256"]:
        raise ValueError("CSV大小或SHA256与已审计版本不符")
    multiple, length = entry["multiple"], entry["legacy_length"]
    divider, suber = entry["sleep_divider"], entry["sleep_suber_ms"]
    if (type(multiple) is not int or type(length) is not int or multiple <= 0 or length <= 0
            or not math.isfinite(divider) or divider <= 0 or not math.isfinite(suber)):
        raise ValueError("离散编译参数无效")
    rows = [row for row in csv.reader(io.StringIO(raw.decode("utf-8-sig"))) if row]
    if not 1 <= len(rows) <= 100000 or min(len(rows), length) * multiple > 4096:
        raise ValueError("离散事件数量越界")
    subdivided = []
    scale = 2.45 / sensitivity
    for index, row in enumerate(rows):
        if len(row) != 3:
            raise ValueError("每行必须为delta_x,delta_y,delay_ms")
        dx, dy, delay = map(float, row)
        if (not all(map(math.isfinite, (dx, dy, delay))) or delay <= 0 or delay > 1000
                or abs(dx) > 10000 or abs(dy) > 10000):
            raise ValueError("CSV位移与延迟无效")
        if index >= length:
            continue
        # 正延迟使用C++ std::round的半数向上语义，不能用Python的偶数舍入。
        normalized_delay = math.floor(delay * 10.0 + 0.5) / 10.0
        if normalized_delay <= 0:
            raise ValueError("归一后的旧子步延迟须为正")
        interval = normalized_delay / divider - suber
        x, y = dx * scale, dy * scale
        if not all(map(math.isfinite, (interval, x, y))) or interval <= 0:
            raise ValueError("离散位移或周期无效")
        base_x, base_y = x / multiple, y / multiple
        for substep in range(multiple):
            last = substep == multiple - 1
            subdivided.append((x if last else base_x, y if last else base_y, interval))
            if not last:
                x -= base_x
                y -= base_y
    if len(subdivided) < 2:
        raise ValueError("首子步只等待后必须仍有事件")
    events = []
    elapsed = subdivided[0][2]
    for dx, dy, interval in subdivided[1:]:
        if elapsed > 60000 or abs(dx) > 1e7 or abs(dy) > 1e7:
            raise ValueError("离散事件越界")
        events.append([elapsed, dx, -dy])
        elapsed += interval
    return {"schema_version": 3, "id": entry["id"], "revision": 1,
            "weapon_id": entry["canonical_weapon_id"], "sensitivity": sensitivity,
            "sample_semantics": "discrete_delta", "unit": "device_counts", "verified": False,
            "events": events,
            "import_report": {"row_count": len(rows), "legacy_length": length,
                              "conversion_revision": "legacy-discrete-v1", "source_sha256": entry["sha256"],
                              "repository": manifest["repository"], "commit": manifest["commit"],
                              "redistribution_verified": False}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--reference-sensitivity", type=float, required=True)
    parser.add_argument("--discrete-legacy", action="store_true", help="另存旧版离散schema3候选；不自动确认或切换active")
    args = parser.parse_args()
    if not math.isfinite(args.reference_sensitivity) or args.reference_sensitivity <= 0:
        parser.error("reference-sensitivity必须是明确正值；它不代表完成校准")
    script_directory = Path(__file__).resolve().parent
    # 只支持仓库scripts与发布tools/recoil两种明确布局，不依赖调用方CWD或搜索祖先目录。
    packaged = script_directory.name.casefold() == "recoil" and script_directory.parent.name.casefold() == "tools"
    data_root = script_directory.parent.parent if packaged else script_directory.parent
    manifest = json.loads((data_root / "assets/recoil/legacy_manifest.json").read_text(encoding="utf-8-sig"))
    names = load_weapon_names(data_root)
    source = args.source_directory.resolve(strict=True)
    outputs = []
    for original in manifest["profiles"]:
        entry = dict(original)
        canonical = names.get(entry["canonical_weapon_id"].casefold())
        if not canonical:
            raise ValueError("清单武器名称不在共享目录中")
        entry["canonical_weapon_id"] = canonical
        file = source / entry["file"]
        if file.is_symlink() or file.resolve().parent != source:
            raise ValueError("不允许导入目录外文件")
        converter = convert_discrete if args.discrete_legacy else convert
        suffix = "-r1-discrete.json" if args.discrete_legacy else "-r1-imported.json"
        outputs.append((entry["id"] + suffix, converter(file.read_bytes(), entry, manifest, args.reference_sensitivity)))
    args.output_directory.mkdir(parents=True, exist_ok=True)
    if any((args.output_directory / name).exists() for name, _ in outputs):
        raise FileExistsError("输出版本已存在；请选择新的本地目录，不覆盖基线")
    for name, profile in outputs:
        with (args.output_directory / name).open("x", encoding="utf-8") as handle:
            json.dump(profile, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"已导入{len(outputs)}份IMPORTED候选；未校准、未发布、未输出设备输入。")


if __name__ == "__main__":
    main()
