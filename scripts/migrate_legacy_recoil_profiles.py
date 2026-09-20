"""已确认旧17曲线的离散迁移：prepare只写独立计划目录，apply校核后原子切换索引。"""
import argparse
import contextlib
import copy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import uuid

from import_recoil_profiles import convert, convert_discrete, load_weapon_names


SOURCE_PROFILES_SHA256 = "3b16b56c3a51fc0d09cafffc8ffc49e280a5abffad0c6340e2f2ad8ec645d200"


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def redirected(path):
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


def child(directory, name):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]+\.json", name):
        raise ValueError("曲线文件名无效")
    result = directory / name
    if redirected(result) or (result.exists() and result.resolve().parent != directory.resolve()):
        raise ValueError("拒绝目录外引用")
    return result


def read(path, limit=16 * 1024 * 1024):
    if redirected(path) or not path.is_file() or path.stat().st_size > limit:
        raise ValueError("文件类型或大小无效")
    raw = path.read_bytes()
    return raw, json.loads(raw.decode("utf-8-sig"))


def prepare(source, source_profiles, target, output, data_root, expected_source_hash=SOURCE_PROFILES_SHA256):
    if any(redirected(path) for path in (source, target, output)):
        raise ValueError("迁移目录不得重定向")
    source, target = source.resolve(strict=True), target.resolve(strict=True)
    if output.exists():
        raise FileExistsError("计划目录已存在，请另存")
    if output.resolve().is_relative_to(target):
        raise ValueError("计划目录必须与活动曲线目录分离")
    manifest_raw, manifest = read(data_root / "assets/recoil/legacy_manifest.json")
    profiles_raw, profiles = read(source_profiles)
    if digest(profiles_raw) != expected_source_hash:
        raise ValueError("旧recoil_profiles.json SHA256不符")
    names = load_weapon_names(data_root)
    old_by_id = {p["name"]: p for p in profiles["weapons"]}
    entries = manifest["profiles"]
    if len(entries) != 17 or len({e['id'] for e in entries}) != 17 or set(old_by_id) != {e['id'] for e in entries}:
        raise ValueError("只允许完整已批准17武器清单")
    active_raw, active = read(target / "active.json", 65536)
    if active.get("schema_version") != 1 or not isinstance(active.get("active"), dict):
        raise ValueError("活动索引格式无效")
    next_active = copy.deepcopy(active)
    changes, files = [], {}
    for entry in entries:
        old = old_by_id[entry["id"]]
        pairs = [("length", "legacy_length"), ("multiple", "multiple"),
                 ("sleep_divider", "sleep_divider"), ("sleep_suber", "sleep_suber_ms")]
        if any(old[a] != entry[b] for a, b in pairs) or Path(old["pattern_file"]).name != entry["file"]:
            raise ValueError("旧profile参数与批准清单不符")
        canonical = names[entry["canonical_weapon_id"].casefold()]
        keys = [key for key in active["active"] if names.get(key.casefold()) == canonical]
        if len(keys) != 1:
            raise ValueError("活动曲线缺失或存在歧义: " + canonical)
        key = keys[0]
        old_file = active["active"][key]["file"]
        current_raw, current = read(child(target, old_file))
        if (current.get("schema_version") != 2 or current.get("verified") is not True
                or names.get(current.get("weapon_id", "").casefold()) != canonical):
            raise ValueError("只迁移已确认且匹配的schema2旧曲线: " + key)
        sensitivity = current.get("sensitivity")
        if type(sensitivity) not in (int, float) or not math.isfinite(sensitivity) or sensitivity <= 0:
            raise ValueError("原灵敏度无效")
        if type(current.get("revision")) is not int or not 0 < current["revision"] < 2147483647:
            raise ValueError("原修订号无效")
        if redirected(source / entry["file"]) or (source / entry["file"]).resolve().parent != source:
            raise ValueError("CSV路径无效")
        raw_csv = (source / entry["file"]).read_bytes()
        prior = convert(raw_csv, entry, manifest, sensitivity)
        points = current.get("points", [])
        if len(points) != len(prior["points"]) or any(
                not isinstance(a, list) or len(a) != 3 or any(
                    type(x) not in (int, float) or not math.isfinite(x) or not math.isclose(x, y, rel_tol=1e-12, abs_tol=1e-10)
                    for x, y in zip(a, b)) for a, b in zip(points, prior["points"])):
            raise ValueError("当前累计曲线不是批准来源的原转换结果: " + key)
        migrated = convert_discrete(raw_csv, entry, manifest, sensitivity)
        migrated.update(id=current["id"], weapon_id=current["weapon_id"],
                        revision=current["revision"] + 1, verified=True)
        new_file = f"{current['id']}-r{migrated['revision']}-discrete.json"
        if child(target, new_file).exists() or new_file in files:
            raise FileExistsError("目标版本已存在，禁止覆盖")
        next_active["active"][key] = {"file": new_file, "previous": old_file}
        new_raw = encoded(migrated)
        files[new_file] = new_raw
        changes.append({"weapon": key, "old_file": old_file, "old_sha256": digest(current_raw),
                        "new_file": new_file, "new_sha256": digest(new_raw),
                        "source_sha256": digest(raw_csv), "sensitivity": sensitivity,
                        "events": len(migrated["events"])})
    # 其他武器索引与额外字段不改变，候选和历史不参与迁移。
    next_raw = encoded(next_active)
    plan = {"schema_version": 1, "task": "RECOIL-LEGACY-PARITY-001", "migration_id": uuid.uuid4().hex,
            "active_before_sha256": digest(active_raw), "active_after_sha256": digest(next_raw),
            "manifest_sha256": digest(manifest_raw), "source_profiles_sha256": digest(profiles_raw),
            "changes": changes, "physical_output_started": False}
    output.mkdir(parents=True)
    (output / "profiles").mkdir()
    for name, raw in files.items():
        (output / "profiles" / name).write_bytes(raw)
    (output / "active-before.json").write_bytes(active_raw)
    (output / "active-after.json").write_bytes(next_raw)
    (output / "plan.json").write_bytes(encoded(plan))
    # 回读已写内容，避免把写入成功当作完整产物。
    validate_plan(output)
    return plan


def validate_plan(output):
    if redirected(output) or redirected(output / "profiles"):
        raise ValueError("计划目录不得重定向")
    _, plan = read(output / "plan.json")
    if (plan.get("schema_version") != 1 or plan.get("task") != "RECOIL-LEGACY-PARITY-001"
            or not re.fullmatch(r"[0-9a-f]{32}", plan.get("migration_id", ""))
            or len(plan.get("changes", [])) != 17):
        raise ValueError("迁移计划格式无效")
    before_raw, before = read(output / "active-before.json", 65536)
    after_raw, after = read(output / "active-after.json", 65536)
    if digest(before_raw) != plan["active_before_sha256"] or digest(after_raw) != plan["active_after_sha256"]:
        raise ValueError("计划索引被修改")
    expected = copy.deepcopy(before)
    seen = set()
    for change in plan["changes"]:
        name, weapon = change["new_file"], change["weapon"]
        if name in seen or before["active"][weapon]["file"] != change["old_file"]:
            raise ValueError("计划曲线重复或与原索引不符")
        seen.add(name)
        raw, profile = read(child(output / "profiles", name))
        if digest(raw) != change["new_sha256"] or profile.get("schema_version") != 3 or profile.get("verified") is not True:
            raise ValueError("计划曲线被修改或不是确认离散版本")
        expected["active"][weapon] = {"file": name, "previous": change["old_file"]}
    if expected != after:
        raise ValueError("计划修改了范围外索引")
    return plan, before_raw, after_raw


@contextlib.contextmanager
def index_lock(directory):
    # 与RecoilStore::IndexLock使用同一Windows独占句柄，不凭锁文件存在推断占用。
    if redirected(directory / "active.lock"):
        raise ValueError("索引锁路径不得重定向")
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.CreateFileW(str(directory / "active.lock"), 0xC0000000, 0, None, 4, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError("活动索引正被占用，请关闭相关操作后重试")
        try:
            yield
        finally:
            kernel.CloseHandle(handle)
    else:
        import fcntl
        with (directory / "active.lock").open("a+b") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield


def apply(output, target):
    if redirected(output) or redirected(target):
        raise ValueError("迁移目录不得重定向")
    output, target = output.resolve(strict=True), target.resolve(strict=True)
    plan, before_raw, after_raw = validate_plan(output)
    with index_lock(target):
        if read(target / "active.json", 65536)[0] != before_raw:
            raise ValueError("活动索引已变化，拒绝过期迁移计划")
        for change in plan["changes"]:
            if digest(read(child(target, change["old_file"]))[0]) != change["old_sha256"]:
                raise ValueError("原曲线已变化，拒绝过期迁移计划")
            new = child(target, change["new_file"])
            if new.exists() and digest(read(new)[0]) != change["new_sha256"]:
                raise FileExistsError("目标版本冲突")
        backup_root = target / "migration-backups"
        if redirected(backup_root) or redirected(backup_root / plan["migration_id"]):
            raise ValueError("备份路径不得重定向")
        backup = backup_root / plan["migration_id"]
        backup.mkdir(parents=True, exist_ok=True)
        for name, raw in [("active-before.json", before_raw), ("plan.json", encoded(plan))]:
            destination = backup / name
            if destination.exists():
                if destination.read_bytes() != raw:
                    raise FileExistsError("备份内容冲突")
            else:
                with destination.open("xb") as handle:
                    handle.write(raw); handle.flush(); os.fsync(handle.fileno())
        for change in plan["changes"]:
            destination = child(target, change["new_file"])
            if not destination.exists():
                with destination.open("xb") as handle:
                    handle.write(read(child(output / "profiles", change["new_file"]))[0])
                    handle.flush(); os.fsync(handle.fileno())
            if digest(destination.read_bytes()) != change["new_sha256"]:
                raise OSError("新曲线回读失败")
        temporary = target / ("active.tmp-" + uuid.uuid4().hex)
        try:
            with temporary.open("xb") as handle:
                handle.write(after_raw); handle.flush(); os.fsync(handle.fileno())
            os.replace(temporary, target / "active.json")
        finally:
            if temporary.exists():
                temporary.unlink()
        if read(target / "active.json", 65536)[0] != after_raw:
            raise OSError("索引切换后回读失败")
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["prepare", "apply"])
    parser.add_argument("--profiles-directory", type=Path, required=True)
    parser.add_argument("--plan-directory", type=Path, required=True)
    parser.add_argument("--source-directory", type=Path)
    parser.add_argument("--source-profiles", type=Path)
    args = parser.parse_args()
    if args.mode == "prepare":
        if not args.source_directory or not args.source_profiles:
            parser.error("prepare需要source-directory与source-profiles")
        script = Path(__file__).resolve().parent
        packaged = script.name.casefold() == "recoil" and script.parent.name.casefold() == "tools"
        data_root = script.parent.parent if packaged else script.parent
        plan = prepare(args.source_directory, args.source_profiles, args.profiles_directory, args.plan_directory, data_root)
    else:
        plan = apply(args.plan_directory, args.profiles_directory)
    print(json.dumps({"mode": args.mode, "weapons": len(plan["changes"]), "migration_id": plan["migration_id"],
                      "physical_output_started": False}, ensure_ascii=False))


if __name__ == "__main__":
    main()
