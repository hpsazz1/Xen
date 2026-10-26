"""按人工确认、站位、复位瞄点三步建立外部双图资源；不会执行游戏动作。"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time

try:
    from .reference_assets import geometry_for, load_reference, manifest_revision, png_size, resource_directory, validate_standing_marker
    from .screen_capture import capture_primary
except ImportError:
    from reference_assets import geometry_for, load_reference, manifest_revision, png_size, resource_directory, validate_standing_marker
    from screen_capture import capture_primary


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".pending")
    if temporary.exists():
        raise ValueError("存在未结束的暂存文件，请检查后重试")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def begin(root, plan_path, verified, standing_note="", standing_marker=None):
    if not verified:
        raise ValueError("必须先人工完成一次成功投掷，再按F10采集")
    source = Path(plan_path).resolve()
    if source.suffix.lower() != ".json" or source.stat().st_size > 1024 * 1024:
        raise ValueError("方案文件无效")
    plan = json.loads(source.read_text(encoding="utf-8-sig"))
    if not isinstance(plan, dict) or not plan.get("actions") or not plan.get("map"):
        raise ValueError("方案必须包含地图和既有动作")
    if standing_marker is not None:
        validate_standing_marker(standing_marker)
    directory = resource_directory(root, source.stem)
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        raise ValueError("该资源目录已存在内容；继续 standing/aim，或为新版方案使用新文件名")
    draft = {"schema_version": 1, "plan_id": source.name, "reference_id": source.stem,
             "plan": plan, "manual_throw_verified": True,
             "confirmed_at_utc": datetime.now(timezone.utc).isoformat(),
             "standing_note": standing_note, "standing_image": "standing.png", "aim_image": "aim.png",
             "target_x": 160, "target_y": 160}
    if standing_marker is not None:
        draft["standing_marker"] = dict(standing_marker)
    write_json(directory / "draft.json", draft)
    return source.stem


def add_image(root, identity, role, image=None, capture=False, delay=5.0,
              roi_x=None, roi_y=None, scale_x=1.0, scale_y=1.0, capture_function=capture_primary):
    if role not in ("standing", "aim") or bool(image) == bool(capture):
        raise ValueError("每步只能选择 --image 或 --capture")
    if not 0 <= delay <= 60:
        raise ValueError("采图等待需在 0 至 60 秒之间")
    directory = resource_directory(root, identity)
    if (directory / "reference.json").exists():
        raise ValueError("已完成资源不覆盖；新版请绑定新的方案文件名")
    draft = json.loads((directory / "draft.json").read_text(encoding="utf-8-sig"))
    if draft.get("manual_throw_verified") is not True or draft.get("reference_id") != identity:
        raise ValueError("先 begin 并明确确认人工投掷成功")
    target = directory / (role + ".png")
    temporary = directory / (role + ".pending.png")
    if target.exists() or temporary.exists():
        raise ValueError("该步骤已有图片，拒绝覆盖人工资源")
    if role == "aim" and not (directory / "standing.png").is_file():
        raise ValueError("先采集 standing，再复位到原始站位和瞄角采集 aim")
    try:
        if capture:
            print(f"{delay:g} 秒后采集主屏；请切回游戏，关闭控制台及提示窗，保持正确站位/瞄角。", flush=True)
            time.sleep(delay)
            capture_function(temporary)
        else:
            png_size(image)
            temporary.write_bytes(Path(image).read_bytes())
        width, height = png_size(temporary)
        if role == "aim" and min(width, height) < 320:
            raise ValueError("需要完整原始全屏图，不接收缩略图")
        # 共用站位图只作定位说明；不裁剪或缩放来凑瞄点图的分辨率。
        geometry = geometry_for(width, height, roi_x, roi_y, scale_x, scale_y) if role == "aim" else None
        temporary.replace(target)
        if role == "standing":
            return False
        draft["geometry"] = geometry
        draft["image_sha256"] = {field: hashlib.sha256((directory / draft[field]).read_bytes()).hexdigest()
                                 for field in ("standing_image", "aim_image")}
        draft["reference_revision"] = manifest_revision(draft)
        write_json(directory / "reference.json", draft)
        load_reference(root, identity, draft["plan_id"])
        return True
    finally:
        if temporary.exists():
            temporary.unlink()  # 只回收本次明确创建的单个暂存图片。
