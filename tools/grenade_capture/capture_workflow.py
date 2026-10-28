"""F9站位与F10瞄点独立采图、资源打包与发送；不产生游戏输入。"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import time
import urllib.request
import uuid
import zipfile

try:
    from .reference_assets import geometry_for, load_reference, manifest_revision, png_size, resource_directory, resource_files
    from .screen_capture import capture_primary
except ImportError:
    from reference_assets import geometry_for, load_reference, manifest_revision, png_size, resource_directory, resource_files
    from screen_capture import capture_primary


DEFAULT_ROOT = Path(__file__).resolve().parents[2] / "cache/grenade"
SHARED_STANDING = Path(__file__).with_name("standing") / "mirage-t-spawns.png"
DEFAULT_DESTINATION = r"\\192.168.3.20\XenLab$\releases\Xen-current\cache\grenade"
MARKERS = {"T1": (.223, .498), "T2": (.664, .493), "T3": (.598, .344),
           "T4": (.496, .349), "T5": (.395, .351)}
MAP_NAMES = {"de_mirage": "Mirage", "de_dust2": "Dust II", "de_inferno": "Inferno",
             "de_ancient": "Ancient", "de_anubis": "Anubis", "de_nuke": "Nuke",
             "de_vertigo": "Vertigo", "de_train": "Train", "de_cache": "Cache", "de_overpass": "Overpass"}


def write_json(path, value):
    # 稳定的.pending文件可能在子进程被终止后残留；唯一暂存名不阻断下次恢复。
    path = Path(path)
    temporary = path.with_name(path.name + ".pending-" + uuid.uuid4().hex)
    with temporary.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, ensure_ascii=False, indent=2, allow_nan=False)
        output.write("\n")
    temporary.replace(path)


def short_name(name, map_name=""):
    """只精简已有名称，保留备用瞄角等区别，不推断站位或动作。"""
    if not isinstance(name, str) or not name.strip():
        raise ValueError("方案名称为空")
    result = name.strip()
    prefixes = {map_name, map_name.removeprefix("de_"), MAP_NAMES.get(map_name, "")}
    prefixes.update(MAP_NAMES.values())
    for prefix in sorted(filter(None, prefixes), key=len, reverse=True):
        if result.lower().startswith(prefix.lower() + " "):
            result = result[len(prefix):].lstrip()
            break
    result = re.sub(r"(?:\s+R\d+|\s*\[S\d+\])+$", "", result, flags=re.IGNORECASE).strip()
    result = re.sub(r"^(CT|T)来源(\d+)号(?=\s|$)", r"\1\2", result)
    result = re.sub(r"\bVIP(?!烟)", "VIP烟", result, flags=re.IGNORECASE)
    return result


def _json(path, limit=1024 * 1024):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError("资源记录缺失或超过上限")
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _sha(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def validate_context(value, deadline=None, clock=time.monotonic):
    if not isinstance(value, dict) or value.get("ready") is not True:
        raise ValueError(value.get("reason", "辅机尚无可采集方案") if isinstance(value, dict) else "采集状态无效")
    plan, ident = value.get("plan"), value.get("plan_id")
    if (not isinstance(ident, str) or not ident.endswith(".json") or Path(ident).name != ident
            or not isinstance(plan, dict) or not plan.get("actions")
            or not isinstance(plan.get("name"), str) or not plan["name"].strip()
            or not isinstance(value.get("map"), str) or not re.fullmatch(r"de_[a-z0-9_]{1,80}", value["map"])
            or plan.get("map") != value["map"]
            or value.get("team") not in ("T", "CT")
            or not isinstance(value.get("generation"), str) or not value["generation"]
            or type(value.get("remaining_ms")) is not int or not 0 < value["remaining_ms"] <= 10000
            or not re.fullmatch(r"[0-9a-f]{64}", value.get("plan_sha256", ""))):
        raise ValueError("采集上下文字段无效")
    resource_directory(".", ident[:-5])
    if deadline is not None and clock() >= deadline:
        raise ValueError("游戏状态已过期，请待同步后重新采集")
    return value


class ContextReader:
    def __init__(self, host, port=5015, clock=time.monotonic, role="aim"):
        if role not in ("aim", "standing"):
            raise ValueError("采集类型必须为站位或瞄点")
        address = ipaddress.ip_address(host)
        private = ("127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
        if address.version != 4 or not any(address in ipaddress.ip_network(network) for network in private):
            raise ValueError("采集仅连接辅机局域网IPv4地址")
        if not 1 <= port <= 65535:
            raise ValueError("网页端口无效")
        self.url, self.clock = f"http://{address}:{port}/capture-context.json", clock
        if role == "standing":
            self.url += "?role=standing"
        # 不经系统代理发送局域网制作上下文，不跟随重定向到另一台主机。
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args, **kwargs):
                return None
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def __call__(self):
        started = self.clock()
        request = urllib.request.Request(self.url, headers={"Cache-Control": "no-store"})
        data = bytearray()
        with self.opener.open(request, timeout=2) as response:
            while len(data) <= 1024 * 1024:
                if self.clock() - started >= 2:
                    raise ValueError("采集状态读取超时")
                block = response.read1(min(65536, 1024 * 1024 + 1 - len(data)))
                if not block:
                    break
                data.extend(block)
        if len(data) > 1024 * 1024 or self.clock() - started >= 2:
            raise ValueError("采集状态读取超时或过大")
        value = validate_context(json.loads(data))
        value["_deadline"] = started + value["remaining_ms"] / 1000
        return validate_context(value, value["_deadline"], self.clock)


def _identity(value):
    return {key: value[key] for key in ("map", "team", "generation", "plan_id", "plan", "plan_sha256")}


def standing_for(context, root, shared_standing=SHARED_STANDING):
    existing = resource_directory(Path(root) / "references", context["plan_id"][:-5])
    if (existing / "reference.json").is_file():
        value, directory = load_reference(Path(root) / "references", context["plan_id"][:-5], context["plan_id"], allow_partial=True)
        if "standing_image" in value:
            return directory / value["standing_image"], value.get("standing_marker")
    custom = Path(root) / "standing" / (context["plan_id"][:-5] + ".png")
    if custom.is_file():
        png_size(custom)
        return custom, None
    name = short_name(context["plan"]["name"], context["map"])
    point = re.match(r"^(T[1-5])(?:\s|$)", name)
    if context["map"] == "de_mirage" and point:
        label = point[1]
        x, y = MARKERS[label]
        path, marker = Path(shared_standing), {"x": x, "y": y, "label": label}
    else:
        path, marker = Path(root) / "standing" / (context["plan_id"][:-5] + ".png"), None
    png_size(path)
    return path, marker


def _archive_name(title):
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", title).rstrip(" .")[:100]
    if not value:
        raise ValueError("方案名称不能用作文件名")
    if value.split(".")[0].upper() in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)], *[f"LPT{i}" for i in range(1, 10)]}:
        value = "道具 " + value
    return value + ".zip"


def _publish_directory(staged, target, backup):
    """两次同卷改名；旧版可恢复，不能视为多文件/跨机整体原子事务。"""
    target.parent.mkdir(parents=True, exist_ok=True)
    moved = False
    if target.exists():
        if target.is_symlink() or not target.is_dir():
            raise ValueError("现有参考目录不是普通目录")
        backup.parent.mkdir(parents=True, exist_ok=True)
        target.rename(backup)
        moved = True
    try:
        staged.rename(target)
    except Exception:
        if moved and not target.exists():
            backup.rename(target)
        raise


class LocalSavePending(RuntimeError):
    """图片与包已在本地暂存，下一次采集先恢复文件发布，不重新解释确认事实。"""


def _finish_local(root, identity, journal):
    record = _json(journal)
    metadata = record["metadata"]
    if (record.get("identity") != identity or metadata.get("plan_id") != identity + ".json"
            or not re.fullmatch(r"[0-9a-f]{32}", metadata.get("capture_id", ""))
            or not re.fullmatch(r"de_[a-z0-9_]{1,80}", metadata.get("map", ""))
            or not metadata.get("package", "").startswith("packages/" + metadata["map"] + "/")):
        raise ValueError("本地恢复记录身份无效")
    stage = root / "capture-staging" / metadata["capture_id"]
    staged_reference = stage / "references" / identity
    target = resource_directory(root / "references", identity)
    if staged_reference.exists():
        value, _ = load_reference(stage / "references", identity, metadata["plan_id"], allow_partial=True)
        if value["reference_revision"] != metadata["reference_revision"]:
            raise ValueError("本地暂存参考已变化")
        _publish_directory(staged_reference, target, root / "reference-backups" / identity / metadata["capture_id"])
    else:
        value, _ = load_reference(root / "references", identity, metadata["plan_id"], allow_partial=True)
        if value["reference_revision"] != metadata["reference_revision"]:
            raise ValueError("本地发布记录与参考资源不符")
    package = _safe_relative(root, metadata["package"])
    archive = stage / "capture.zip"
    if archive.exists():
        if _sha(archive) != metadata["package_sha256"]:
            raise ValueError("本地暂存资源包已变化")
        backup = root / "package-backups" / metadata["map"] / (metadata["capture_id"] + "-" + package.name)
        moved = False
        if package.exists():
            backup.parent.mkdir(parents=True, exist_ok=True)
            package.rename(backup)
            moved = True
        try:
            archive.rename(package)
        except Exception:
            if moved and not package.exists():
                backup.rename(package)
            raise
    if _sha(package) != metadata["package_sha256"]:
        raise ValueError("本地资源包尚未完整发布")
    captures = root / "captures"
    captures.mkdir(parents=True, exist_ok=True)
    write_json(captures / (identity + ".json"), metadata)
    journal.rename(journal.with_name("completed-" + metadata["capture_id"] + ".json"))
    return metadata


def capture_once(fetch_context, root=DEFAULT_ROOT, capture=capture_primary, shared_standing=SHARED_STANDING,
                 foreground=None, expected_foreground=None, report=lambda *args: None, role="aim"):
    """F9/F10各自只更新一张图片；可注入合成采图做离线回归。"""
    if role not in ("standing", "aim"):
        raise ValueError("采集类型必须为站位或瞄点")
    root = Path(root).resolve()
    before = validate_context(fetch_context())
    validate_context(before, before.get("_deadline"))
    if foreground and foreground() != expected_foreground:
        raise ValueError("游戏已失焦，未采集")
    identity, capture_id = before["plan_id"][:-5], uuid.uuid4().hex
    transactions = root / "capture-transactions" / identity
    transactions.mkdir(parents=True, exist_ok=True)
    journal = transactions / "pending.json"
    if journal.exists():
        try:
            _finish_local(root, identity, journal)
        except Exception as error:
            raise LocalSavePending("上次本地采图已保留，但保存尚未恢复：" + str(error)) from error
    stage = root / "capture-staging" / capture_id
    stage.mkdir(parents=True)
    plan_path = stage / before["plan_id"]
    write_json(plan_path, before["plan"])
    refs = stage / "references"
    directory = resource_directory(refs, identity)
    directory.mkdir(parents=True)
    current = resource_directory(root / "references", identity)
    if (current / "reference.json").is_file():
        manifest, previous = load_reference(root / "references", identity, before["plan_id"], allow_partial=True)
        # F9更新整体方案身份时，旧瞄点仍属于它采集时的方案，不能被站位采集重新确认。
        if "aim_image" in manifest:
            manifest.setdefault("aim_plan", manifest["plan"])
        for field in ("standing_image", "aim_image"):
            if field in manifest and field != role + "_image":
                (directory / manifest[field]).write_bytes((previous / manifest[field]).read_bytes())
    else:
        manifest = {"schema_version": 1, "plan_id": before["plan_id"], "reference_id": identity,
                    "manual_throw_verified": False, "standing_note": "按站位参考图定位",
                    "target_x": 160, "target_y": 160, "image_sha256": {}}
    manifest["plan"] = before["plan"]
    report("capturing", "正在采集当前" + ("站位" if role == "standing" else "瞄点") + "画面")
    # 不因获取上下文/读取已有图片耗时复活旧状态。
    validate_context(before, before.get("_deadline"))
    if foreground and foreground() != expected_foreground:
        raise ValueError("游戏已失焦，未采集")
    target = directory / (role + ".png")
    capture(target)
    width, height = png_size(target)
    manifest[role + "_image"] = target.name
    manifest["image_sha256"][role + "_image"] = _sha(target)
    captured_at = datetime.now(timezone.utc).isoformat()
    if role == "aim":
        manifest["aim_plan"] = before["plan"]
        manifest["geometry"] = geometry_for(width, height)
        manifest["manual_throw_verified"] = True
        manifest["confirmed_at_utc"] = captured_at
        manifest["confirmation_source"] = "host_f10"
    else:
        manifest.pop("standing_marker", None)
        manifest["standing_note"] = "按站位参考图定位"
        manifest["standing_source"] = "host_f9"
        manifest["standing_captured_at_utc"] = captured_at
    if foreground and foreground() != expected_foreground:
        raise ValueError("采集时切换了窗口，图片未发布")
    after = validate_context(fetch_context())
    validate_context(after, after.get("_deadline"))
    if _identity(before) != _identity(after):
        raise ValueError("采集期间方案或游戏状态改变，图片未发布")
    manifest["display_name"] = short_name(before["plan"]["name"], before["map"])
    manifest["capture_complete"] = "standing_image" in manifest and "aim_image" in manifest
    manifest["reference_revision"] = manifest_revision(manifest)
    write_json(directory / "reference.json", manifest)
    load_reference(refs, identity, before["plan_id"], allow_partial=True)
    metadata = {"capture_id": capture_id, "plan_id": before["plan_id"], "plan_sha256": before["plan_sha256"],
                "display_name": manifest["display_name"], "map": before["map"], "plan": before["plan"],
                "reference_revision": manifest["reference_revision"], "status": "pending",
                "capture_role": role, "captured_at_utc": captured_at}
    packages = root / "packages" / before["map"]
    packages.mkdir(parents=True, exist_ok=True)
    package = packages / _archive_name(manifest["display_name"])
    if package.exists():
        try:
            with zipfile.ZipFile(package) as existing:
                if existing.namelist().count("capture.json") != 1 or existing.getinfo("capture.json").file_size > 1024 * 1024:
                    raise ValueError("旧资源包记录无效")
                old = json.loads(existing.read("capture.json"))
        except (OSError, ValueError, KeyError, zipfile.BadZipFile):
            old = None
        if not isinstance(old, dict):
            old = None
        # 损坏旧包原样备份；可识别的其他方案同名包另用稳定ID区分。
        if old is not None and old.get("plan_id") != before["plan_id"]:
            package = packages / (_archive_name(manifest["display_name"])[:-4][:60] + " -- " + identity + ".zip")
    archive = stage / "capture.zip"
    metadata["package"] = package.relative_to(root).as_posix()
    members = {"plans/" + before["plan_id"]: plan_path}
    members.update({f"references/{identity}/{name}": directory / name for name in resource_files(manifest)})
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_STORED) as output:
        for name, path in members.items():
            output.write(path, name)
        output.writestr("capture.json", json.dumps(metadata, ensure_ascii=False, indent=2))
    with zipfile.ZipFile(archive) as output:
        if set(output.namelist()) != set(members) | {"capture.json"} or output.testzip() is not None:
            raise ValueError("打包校验失败")
    report("saving", "图片已采集，正在保存资源和短名资源包")
    metadata["package_sha256"] = _sha(archive)
    write_json(journal, {"identity": identity, "metadata": metadata})
    try:
        _finish_local(root, identity, journal)
    except Exception as error:
        raise LocalSavePending("本地采图与资源包已暂存，保存尚未完成：" + str(error)) from error
    report("pending", "本地资源已保存，正在发送辅机")
    return metadata


def _safe_relative(root, relative):
    root = Path(root).resolve()
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise ValueError("发送记录路径无效")
    path = (root / relative).resolve()
    if path == root or root not in path.parents:
        raise ValueError("发送记录路径越界")
    return path


def _recover_transfer(destination, identity, journal):
    if not journal.exists():
        return
    previous = _json(journal, 65536)
    if previous.get("identity") != identity:
        raise ValueError("待恢复发送记录与方案不符")
    if not re.fullmatch(r"reference-backups/" + re.escape(identity) + r"/[A-Za-z0-9_-]+", previous.get("backup", "")):
        raise ValueError("待恢复备份路径无效")
    target = resource_directory(destination / "references", identity)
    backup = _safe_relative(destination, previous["backup"])
    if not target.exists() and backup.is_dir():
        backup.rename(target)
    if target.exists():
        # 中断后出现无法校验的目录时保留现场，不把新发送盖上去。
        load_reference(destination / "references", identity, allow_partial=True)
    journal.rename(journal.with_name("recovered-" + uuid.uuid4().hex + ".json"))


def transfer_capture(root, identity, destination, deadline=None, report=lambda *args: None):
    root, destination = Path(root).resolve(), Path(destination).resolve()
    if root == destination:
        raise ValueError("辅机目标不能是本地制作目录")
    resource_directory(root / "references", identity)
    metadata = _json(root / "captures" / (identity + ".json"))
    if (metadata.get("plan_id") != identity + ".json"
            or not re.fullmatch(r"de_[a-z0-9_]{1,80}", metadata.get("map", ""))
            or not re.fullmatch(r"[0-9a-f]{64}", metadata.get("plan_sha256", ""))
            or not metadata.get("package", "").startswith("packages/" + metadata["map"] + "/")):
        raise ValueError("本地采集记录身份无效")
    manifest, local = load_reference(root / "references", identity, metadata["plan_id"], allow_partial=True)
    if manifest["reference_revision"] != metadata["reference_revision"] or manifest["plan"] != metadata["plan"]:
        raise ValueError("本地参考资源已改变，未发送")
    package = _safe_relative(root, metadata["package"])
    if _sha(package) != metadata["package_sha256"]:
        raise ValueError("本地资源包已改变，未发送")
    plan_path = destination / "plans" / metadata["plan_id"]
    def check():
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError("发送超过时间预算，本地资源保留待发送")
    def verify_plan():
        check()
        if _json(plan_path) != metadata["plan"] or _sha(plan_path) != metadata["plan_sha256"]:
            raise ValueError("辅机方案已改变或不是本次方案，资源未发布")
    verify_plan()
    transactions = destination / "capture-transfers" / identity
    transactions.mkdir(parents=True, exist_ok=True)
    journal = transactions / "pending.json"
    _recover_transfer(destination, identity, journal)
    transaction = uuid.uuid4().hex
    stage = transactions / transaction
    stage.mkdir()
    backup = destination / "reference-backups" / identity / transaction
    record = {"identity": identity, "backup": backup.relative_to(destination).as_posix(),
              "stage": stage.relative_to(destination).as_posix(), "capture_id": metadata["capture_id"]}
    write_json(journal, record)
    staged_reference = stage / identity
    staged_reference.mkdir()
    def copy_verified(source, target):
        check()
        with Path(source).open("rb") as reader, Path(target).open("xb") as writer:
            for block in iter(lambda: reader.read(1024 * 1024), b""):
                check()
                writer.write(block)
        check()
        if _sha(source) != _sha(target):
            raise ValueError("辅机变化文件回读摘要不一致")
    report("sending", "正在发送并回读本次图片与资源包")
    for name in resource_files(manifest):
        copy_verified(local / name, staged_reference / name)
    copy_verified(package, stage / "capture.zip")
    staged_manifest, _ = load_reference(stage, identity, metadata["plan_id"], allow_partial=True)
    if (staged_manifest["reference_revision"] != metadata["reference_revision"]
            or staged_manifest["plan"] != metadata["plan"] or _sha(stage / "capture.zip") != metadata["package_sha256"]):
        raise ValueError("发送期间本地资源改变，辅机参考未发布")
    verify_plan()
    target_package = _safe_relative(destination, metadata["package"])
    target_package.parent.mkdir(parents=True, exist_ok=True)
    if target_package.exists():
        old_package = destination / "package-backups" / metadata["map"] / (transaction + "-" + target_package.name)
        old_package.parent.mkdir(parents=True, exist_ok=True)
        target_package.rename(old_package)
    (stage / "capture.zip").rename(target_package)
    # 日志在旧目录改名之前写入；被终止后下一次发送先恢复旧目录，再另起事务。
    report("publishing", "正在发布辅机参考目录，旧资源保留备份")
    _publish_directory(staged_reference, resource_directory(destination / "references", identity), backup)
    verify_plan()
    remote, _ = load_reference(destination / "references", identity, metadata["plan_id"], allow_partial=True)
    if remote["reference_revision"] != metadata["reference_revision"] or _sha(target_package) != metadata["package_sha256"]:
        raise ValueError("辅机发布回读未通过；本地资源与远端恢复记录保留")
    journal.rename(transactions / ("completed-" + transaction + ".json"))
    metadata["status"] = "sent"
    metadata["sent_at_utc"] = datetime.now(timezone.utc).isoformat()
    write_json(root / "captures" / (identity + ".json"), metadata)
    report("sent", ("站位图" if metadata.get("capture_role") == "standing" else "瞄点图") + "已发送并回读通过：" + metadata["display_name"])
    return metadata
