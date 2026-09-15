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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--reference-sensitivity", type=float, required=True)
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
        outputs.append((entry["id"] + "-r1-imported.json", convert(file.read_bytes(), entry, manifest, args.reference_sensitivity)))
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
