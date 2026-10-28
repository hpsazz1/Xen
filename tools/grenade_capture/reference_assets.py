"""道具双图资源契约；图像保持原始像素，标记只在预览层绘制。"""

from __future__ import annotations

import json
import hashlib
import math
from pathlib import Path
import re
import struct
import zlib


DEFAULT_ROOT = Path(__file__).resolve().parents[2] / "cache/grenade/references"
MAX_IMAGE_BYTES = 64 * 1024 * 1024


def resource_directory(root, identity):
    if (not isinstance(identity, str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", identity)
            or identity.endswith(".") or ".." in identity
            or identity.split(".")[0].upper() in {
                "CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)],
                *[f"LPT{i}" for i in range(1, 10)]}):
        raise ValueError("资源 ID 必须为安全的英文文件名 stem")
    base = Path(root).resolve()
    target = (base / identity).resolve()
    if target.parent != base:
        raise ValueError("资源目录超出指定根目录")
    return target


def png_size(path):
    path = Path(path)
    if not path.is_file() or not 0 < path.stat().st_size <= MAX_IMAGE_BYTES:
        raise ValueError("PNG 缺失或文件过大")
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("参考图必须为原始 PNG")
    offset, size, image_data, ended = 8, None, False, False
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError("PNG 数据截断")
        kind, body = data[offset + 4:offset + 8], data[offset + 8:end - 4]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != struct.unpack_from(">I", data, end - 4)[0]:
            raise ValueError("PNG 校验失败")
        if offset == 8:
            if kind != b"IHDR" or length != 13:
                raise ValueError("PNG 图片头无效")
            size = struct.unpack_from(">II", body)
            if not all(1 <= value <= 8192 for value in size):
                raise ValueError("PNG 尺寸超出范围")
        if kind == b"IDAT" and length:
            image_data = True
        if kind == b"IEND":
            ended = length == 0 and end == len(data)
            break
        offset = end
    if not size or not image_data or not ended:
        raise ValueError("PNG 图片不完整")
    return size


def geometry_for(width, height, roi_x=None, roi_y=None, scale_x=1.0, scale_y=1.0):
    if any(type(scale) not in (int, float) or not math.isfinite(scale) or scale <= 0
           for scale in (scale_x, scale_y)):
        raise ValueError("截图缩放比例无效")
    geometry = {"source_width": width, "source_height": height,
                "roi_x": math.floor((width - 320 * scale_x) / 2) if roi_x is None else roi_x,
                "roi_y": math.floor((height - 320 * scale_y) / 2) if roi_y is None else roi_y,
                "roi_width": 320, "roi_height": 320,
                "scale_x": scale_x, "scale_y": scale_y}
    validate_geometry(geometry)
    return geometry


def validate_geometry(geometry):
    if not isinstance(geometry, dict):
        raise ValueError("缺少截图几何")
    fields = ("source_width", "source_height", "roi_x", "roi_y", "roi_width", "roi_height")
    if any(type(geometry.get(key)) is not int for key in fields):
        raise ValueError("截图尺寸与 ROI 坐标必须为整数")
    w, h, x, y, rw, rh = (geometry[key] for key in fields)
    if (rw, rh) != (320, 320) or min(w, h) < 320 or max(w, h) > 8192:
        raise ValueError("首版参考 ROI 必须为 320×320")
    for key in ("scale_x", "scale_y"):
        value = geometry.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError("截图缩放比例无效")
    if x < 0 or y < 0 or x + rw * geometry["scale_x"] > w or y + rh * geometry["scale_y"] > h:
        raise ValueError("ROI 超出原图")


def validate_standing_marker(marker):
    if not isinstance(marker, dict):
        raise ValueError("站位标记必须包含归一化坐标与名称")
    for field in ("x", "y"):
        coordinate = marker.get(field)
        if type(coordinate) not in (int, float) or not math.isfinite(coordinate) or not 0 <= coordinate <= 1:
            raise ValueError("站位标记坐标必须位于原图范围内")
    label = marker.get("label")
    if (not isinstance(label, str) or not label or len(label.encode("utf-8")) > 64
            or any(ord(char) < 32 or ord(char) == 127 for char in label)):
        raise ValueError("站位标记名称无效")


def resource_files(value):
    """只打包清单中实际已有的图片，不为另一种采集补占位图。"""
    return tuple(value[field] for field in ("standing_image", "aim_image") if field in value) + ("reference.json",)


def load_reference(root, identity, plan_id=None, allow_partial=False):
    directory = resource_directory(root, identity)
    manifest_path = directory / "reference.json"
    if not manifest_path.is_file() or manifest_path.stat().st_size > 1024 * 1024:
        raise ValueError("参考资源尚未完成")
    value = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if (not isinstance(value, dict) or type(value.get("schema_version")) is not int
            or value.get("schema_version") != 1
            or not isinstance(value.get("plan"), dict) or not value["plan"].get("actions")):
        raise ValueError("参考资源缺少有效方案")
    if (not isinstance(value["plan"].get("name", ""), str)
            or not isinstance(value.get("standing_note", ""), str)
            or not isinstance(value.get("image_sha256"), dict)):
        raise ValueError("参考资源名称、站位说明或摘要无效")
    if value.get("reference_id", identity) != identity:
        raise ValueError("参考资源 ID 不一致")
    if plan_id is not None and value.get("plan_id") != plan_id:
        raise ValueError("参考资源绑定了其他方案")
    has_aim, has_standing = "aim_image" in value, "standing_image" in value
    if not has_aim and not has_standing:
        raise ValueError("参考资源尚未采集图片")
    if type(value.get("capture_complete", True)) is not bool or value.get("capture_complete", True) != (has_aim and has_standing):
        raise ValueError("参考资源采集状态与已有图片不符")
    if has_aim:
        if value.get("manual_throw_verified") is not True:
            raise ValueError("瞄点资源缺少人工投掷确认")
        aim_plan = value.get("aim_plan", value["plan"])
        if not isinstance(aim_plan, dict) or not aim_plan.get("actions"):
            raise ValueError("瞄点资源缺少采集时的方案")
        if not allow_partial and aim_plan != value["plan"]:
            raise ValueError("参考资源与当前方案不一致，请重新采集瞄点图")
        validate_geometry(value.get("geometry"))
    elif not allow_partial:
        raise ValueError("瞄点尚未采集，请按F10采集瞄点图")
    elif value.get("manual_throw_verified") is not False or "geometry" in value:
        raise ValueError("站位图不能冒充已确认的瞄点资源")
    if "standing_marker" in value:
        validate_standing_marker(value["standing_marker"])
    for key, limit in (("target_x", 320), ("target_y", 320)):
        coordinate = value.get(key, 160)
        if type(coordinate) not in (int, float) or not math.isfinite(coordinate) or not 0 <= coordinate < limit:
            raise ValueError("参考瞄点超出 ROI")
    for field, expected in (("standing_image", "standing.png"), ("aim_image", "aim.png")):
        if field not in value:
            if expected in {path.name for path in directory.iterdir()} or field in value["image_sha256"]:
                raise ValueError("未登记的参考图片不能混入资源")
            continue
        if value.get(field) != expected or (directory / expected).resolve().parent != directory:
            raise ValueError("参考图路径无效")
        size = png_size(directory / expected)
        # 站位图可跨方案共用；只有用于匹配的瞄点原图绑定 Capture 几何。
        if field == "aim_image" and size != (value["geometry"]["source_width"], value["geometry"]["source_height"]):
            raise ValueError("瞄点图尺寸必须与源几何一致")
        if value.get("image_sha256", {}).get(field) != hashlib.sha256((directory / expected).read_bytes()).hexdigest():
            raise ValueError("参考图片已改变，需要重新确认资源")
    if value.get("reference_revision") != manifest_revision(value):
        raise ValueError("参考资源清单身份已改变")
    return value, directory


def manifest_revision(value):
    content = {key: item for key, item in value.items() if key != "reference_revision"}
    encoded = json.dumps(content, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()
