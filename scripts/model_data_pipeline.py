"""Xen 本地采集数据审核、冻结、训练 CLI；不激活模型、不访问鼠标。

python scripts/model_data_pipeline.py --job job.json --status status.json --cancel cancel.flag
审核结果必须显式声明正例/负例；漏标签永远不是负例。
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import time
import uuid
import zipfile


SCHEMA = 1
TRAINER_VERSION = "8.3.203"
VERIFIED = {"VERIFIED_POSITIVE", "VERIFIED_NEGATIVE"}
REVIEW_TEMPLATE = Path(__file__).with_name("model_data_review.html")


class PipelineError(Exception):
    pass


class Cancelled(PipelineError):
    pass


def now():
    return datetime.now(timezone.utc).isoformat()


def read_json(path):
    with Path(path).open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contained(base, relative):
    base = Path(base).resolve()
    relative = Path(relative)
    if relative.is_absolute() or ".." in relative.parts:
        raise PipelineError(f"不允许越界路径：{relative}")
    resolved = (base / relative).resolve()
    if not resolved.is_relative_to(base):
        raise PipelineError(f"路径离开数据目录：{relative}")
    return resolved


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,159}", value) or "__" in value:
        raise PipelineError("session/sample ID 必须为安全的英文工程名称")
    return value


def class_schema(value):
    if not isinstance(value, list) or not value or any(not isinstance(n, str) or not n.strip() for n in value):
        raise PipelineError("必须提供经确认的非空 class_names 数组")
    if len(set(value)) != len(value) or any("\n" in n or "\r" in n for n in value):
        raise PipelineError("类别名称重复或包含换行")
    return value


def positive_int(value, name, maximum=100000):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 < value <= maximum:
        raise PipelineError(f"{name} 必须为 1..{maximum} 的整数")
    return value


def training_workers(value=0):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 8:
        raise PipelineError("workers 必须为 0..8 的整数")
    return value


def boxes_checked(boxes, width, height, names):
    checked = []
    if not isinstance(boxes, list):
        raise PipelineError("detections 必须为数组")
    for box in boxes:
        if not isinstance(box, dict):
            raise PipelineError("标注框必须为对象")
        cls = box.get("class_id")
        if isinstance(cls, bool) or not isinstance(cls, int) or not 0 <= cls < len(names):
            raise PipelineError("标注类别超出冻结 schema")
        try:
            x1, y1, x2, y2 = (float(box[key]) for key in ("x1", "y1", "x2", "y2"))
        except (KeyError, ValueError, TypeError) as exc:
            raise PipelineError("标注坐标缺失或不是数字") from exc
        if not all(math.isfinite(v) for v in (x1, y1, x2, y2)) or not (0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height):
            raise PipelineError("标注框必须有限、非空且位于图片内")
        checked.append(dict(class_id=cls, x1=x1, y1=y1, x2=x2, y2=y2))
    return checked


def yolo_text(boxes, width, height):
    return "".join(f"{b['class_id']} {(b['x1']+b['x2'])/(2*width):.10f} {(b['y1']+b['y2'])/(2*height):.10f} {(b['x2']-b['x1'])/width:.10f} {(b['y2']-b['y1'])/height:.10f}\n" for b in boxes)


def read_labels(path, width, height, names):
    if not Path(path).is_file():
        raise PipelineError(f"缺少显式标签文件，不能按负例处理：{path}")
    boxes = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5 or not re.fullmatch(r"\d+", fields[0]):
            raise PipelineError("YOLO 标签必须为 class_id cx cy w h")
        try:
            cx, cy, w, h = map(float, fields[1:])
        except ValueError as exc:
            raise PipelineError("YOLO 标签坐标无效") from exc
        # 导出对中心和宽高分别保留10位小数：边界误差至多(0.5+0.25)*1e-10。
        # 另留少量二进制浮点运算误差；四边采用同一预算，随后裁回图片边界。
        edge_tolerance = 7.5e-11 + 4 * math.ulp(1.0)
        if not all(math.isfinite(v) for v in (cx, cy, w, h)) or not (0 < w <= 1 and 0 < h <= 1 and -edge_tolerance <= cx-w/2 and cx+w/2 <= 1+edge_tolerance and -edge_tolerance <= cy-h/2 and cy+h/2 <= 1+edge_tolerance):
            raise PipelineError("YOLO 归一化坐标越界")
        boxes.append(dict(class_id=int(fields[0]), x1=max(0, (cx-w/2)*width), y1=max(0, (cy-h/2)*height), x2=min(width, (cx+w/2)*width), y2=min(height, (cy+h/2)*height)))
    return boxes_checked(boxes, width, height, names)


def image_info(path):
    try:
        from PIL import Image
    except ImportError as exc:
        raise PipelineError("缺少 Pillow；请在独立 Python 环境安装依赖") from exc
    with Image.open(path) as picture:
        picture.load()
        if picture.format != "PNG":
            raise PipelineError("采集原图必须为 PNG")
        size = picture.size
        thumb = picture.convert("RGB").resize((16, 16))
        raw = thumb.tobytes()
        pixels = tuple(tuple(raw[i:i+3]) for i in range(0, len(raw), 3))
    return size, pixels


class Context:
    def __init__(self, job, status=None, cancel=None):
        self.job = job
        self.status = Path(status) if status else None
        self.cancel = Path(cancel) if cancel else None

    def check(self):
        if self.cancel and self.cancel.exists():
            raise Cancelled("作业已取消；已完成的原始数据与检查点保留")

    def report(self, state, message, result=None):
        if self.status:
            write_json(self.status, dict(schema_version=SCHEMA, state=state, operation=self.job.get("operation"), message=message, result=result or {}, updated_at=now()))

    def output(self):
        if not self.job.get("output"):
            raise PipelineError("必须指定新的 output 目录")
        output = Path(self.job["output"]).resolve()
        if output.exists():
            raise PipelineError("output 已存在；必须新建版本，不能覆盖已有产物")
        output.mkdir(parents=True, exist_ok=False)
        return output


def load_samples(ctx):
    names = class_schema(ctx.job.get("class_names"))
    root = Path(ctx.job["root"]).resolve()
    if not root.is_dir():
        raise PipelineError("采集数据 root 不存在")
    selected = ctx.job.get("session")
    if selected:
        selected_path = Path(selected)
        session_dirs = [selected_path.resolve() if selected_path.is_absolute() else contained(root, selected)]
        if not session_dirs[0].is_relative_to(root):
            raise PipelineError("session 必须位于 root 内")
    else:
        session_dirs = sorted(p.parent for p in root.glob("*/session.json"))
    samples = {}
    for directory in session_dirs:
        ctx.check()
        directory = directory.resolve()
        if not directory.is_relative_to(root):
            raise PipelineError("session 链接指向 root 之外")
        header = read_json(directory / "session.json")
        sid = identifier(header["session_id"])
        if header.get("schema_version") != SCHEMA or header.get("class_names") != names or sid != directory.name:
            raise PipelineError("session 身份、版本或类别 schema 不一致")
        for record in sorted((directory / "samples").glob("*.json")):
            ctx.check()
            sample = read_json(record)
            sample_id = identifier(sample["sample_id"])
            key = (sid, sample_id)
            if key in samples or record.stem != sample_id or sample.get("session_id") != sid or sample.get("schema_version") != SCHEMA:
                raise PipelineError("重复样本或 manifest 身份不匹配")
            path = contained(directory, sample["image"])
            if not path.is_file() or sha256(path) != sample.get("image_sha256"):
                raise PipelineError(f"原图缺失或 SHA-256 不匹配：{sid}/{sample_id}")
            (width, height), thumb = image_info(path)
            if (width, height) != (sample.get("width"), sample.get("height")):
                raise PipelineError("图片实际尺寸与 manifest 不一致")
            detections = boxes_checked(sample.get("detections", []), width, height, names)
            confidences = []
            for box in sample.get("detections", []):
                confidence = box.get("confidence")
                if confidence is not None and (isinstance(confidence, bool) or not isinstance(confidence, (int, float)) or
                        not math.isfinite(confidence) or not 0 <= confidence <= 1):
                    raise PipelineError("原始预标注 confidence 必须为有限的 0..1 数值")
                confidences.append(confidence)
            samples[key] = dict(sample, detections=detections, _confidences=confidences,
                                _path=path, _thumb=thumb, _session=directory)
    if not samples:
        raise PipelineError("未找到完整采集样本")
    return names, samples


def latest_reviews(root, samples, names):
    reviews = {}
    for directory in sorted((Path(root) / "reviews").glob("*")):
        manifest = directory / "revision.json"
        if not manifest.is_file():
            continue
        revision = read_json(manifest)
        if revision.get("schema_version") != SCHEMA or revision.get("class_names") != names:
            raise PipelineError("审核 revision 的 schema 不一致")
        for item in revision["samples"]:
            key = (item["session_id"], item["sample_id"])
            if key not in samples:
                continue
            sample = samples[key]
            if item.get("image_sha256") != sample["image_sha256"]:
                raise PipelineError("审核 revision 与原图身份不一致")
            boxes = boxes_checked(item.get("detections", []), sample["width"], sample["height"], names)
            state = item["state"]
            if state not in VERIFIED | {"EXCLUDED"} or (state == "VERIFIED_POSITIVE" and not boxes) or (state == "VERIFIED_NEGATIVE" and boxes):
                raise PipelineError("审核状态与标注框矛盾")
            reviews[key] = dict(item, detections=boxes, revision=directory.name)
    return reviews


def review_export(ctx):
    names, samples = load_samples(ctx)
    reviews = latest_reviews(ctx.job["root"], samples, names)
    prelabels = {}
    if ctx.job.get("prelabels"):
        prediction = read_json(ctx.job["prelabels"])
        if prediction.get("schema_version") != SCHEMA or prediction.get("class_names") != names:
            raise PipelineError("预标注 revision schema 不一致")
        for item in prediction["samples"]:
            key = (item["session_id"], item["sample_id"])
            if key not in samples or key in prelabels or item["image_sha256"] != samples[key]["image_sha256"]:
                raise PipelineError("预标注 revision 图片身份不一致")
            sample = samples[key]
            prelabels[key] = dict(detections=boxes_checked(item["detections"], sample["width"], sample["height"], names))
    output = ctx.output()
    (output / "obj_train_data").mkdir()
    entries = []
    paths = []
    for key, sample in samples.items():
        ctx.check()
        basename = "__".join(key)
        image = f"obj_train_data/{basename}.png"
        label = f"obj_train_data/{basename}.txt"
        shutil.copyfile(sample["_path"], output / image)
        candidate = reviews.get(key, prelabels.get(key, sample))
        (output / label).write_text(yolo_text(candidate["detections"], sample["width"], sample["height"]), encoding="utf-8")
        entries.append(dict(session_id=key[0], sample_id=key[1], image_sha256=sample["image_sha256"], state="UNKNOWN", label=label))
        paths.append(image)
    (output / "obj.names").write_text("\n".join(names) + "\n", encoding="utf-8")
    (output / "train.txt").write_text("\n".join(paths) + "\n", encoding="utf-8")
    (output / "obj.data").write_text(f"classes = {len(names)}\ntrain = train.txt\nnames = obj.names\nbackup = backup/\n", encoding="utf-8")
    write_json(output / "review.json", dict(schema_version=SCHEMA, class_names=names, samples=entries))
    with zipfile.ZipFile(output / "cvat_yolo.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(output.rglob("*")):
            if path.is_file() and path.name not in {"cvat_yolo.zip", "review.json"}:
                archive.write(path, path.relative_to(output).as_posix())
    return dict(output=str(output), review_manifest=str(output / "review.json"), samples=len(entries), instruction="在 CVAT 审核后将 YOLO txt 放回对应路径；逐图设置 VERIFIED_POSITIVE/VERIFIED_NEGATIVE/EXCLUDED，再导入 review.json。UNKNOWN 不会被导入。")


def stable_json_hash(value):
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def review_root_identity(root, names):
    # 不猜测 UNC 与映射盘的别名；审核生成和导入须使用同一规范数据根。
    return stable_json_hash(dict(schema_version=SCHEMA, root=os.path.normcase(str(Path(root).resolve())), class_names=names))


def review_token(review):
    return stable_json_hash(review) if review is not None else "none"


def box_iou(left, right):
    intersection = max(0, min(left["x2"], right["x2"]) - max(left["x1"], right["x1"])) * max(0, min(left["y2"], right["y2"]) - max(left["y1"], right["y1"]))
    union = (left["x2"] - left["x1"]) * (left["y2"] - left["y1"]) + (right["x2"] - right["x1"]) * (right["y2"] - right["y1"])-intersection
    return intersection / union if union > 0 else 0


def prediction_disagreements(original, predicted):
    # 一对一几何配对后再检查类别，避免类别冲突被同类配对隐藏。
    candidates = sorted(((-box_iou(a, b), i, j) for i, a in enumerate(original)
                         for j, b in enumerate(predicted)), key=lambda value: value)
    used_original, used_predicted = set(), set()
    class_conflicts = position_conflicts = 0
    for negative_iou, i, j in candidates:
        if -negative_iou < 0.5:
            break
        if i in used_original or j in used_predicted:
            continue
        used_original.add(i); used_predicted.add(j)
        class_conflicts += original[i]["class_id"] != predicted[j]["class_id"]
        position_conflicts += -negative_iou < 0.8
    return dict(unmatched_original=len(original)-len(used_original),
                unmatched_predicted=len(predicted)-len(used_predicted),
                class_conflicts=class_conflicts, position_conflicts=position_conflicts)


def review_curate(ctx):
    names, samples = load_samples(ctx)
    reviews = latest_reviews(ctx.job["root"], samples, names)
    predicted = {}
    prediction_identity = None
    if ctx.job.get("prelabels"):
        prediction = read_json(ctx.job["prelabels"])
        if prediction.get("schema_version") != SCHEMA or prediction.get("class_names") != names:
            raise PipelineError("离线预标注 schema 与采集数据不一致")
        prediction_identity = dict(file_sha256=sha256(ctx.job["prelabels"]),
                                   model_sha256=prediction.get("model_sha256"),
                                   settings=prediction.get("prediction_settings"))
        for item in prediction.get("samples", []):
            key = (item["session_id"], item["sample_id"])
            if key not in samples or key in predicted or item.get("image_sha256") != samples[key]["image_sha256"]:
                raise PipelineError("离线预标注重复、未知样本或图片哈希不符")
            sample = samples[key]
            predicted[key] = boxes_checked(item["detections"], sample["width"], sample["height"], names)
            for box in item["detections"]:
                confidence = box.get("confidence")
                if confidence is not None and (isinstance(confidence, bool) or not isinstance(confidence, (int, float)) or not math.isfinite(confidence) or not 0 <= confidence <= 1):
                    raise PipelineError("离线预标注 confidence 必须是0..1有限数")
    # 与浏览器审核契约一致的单图边界；必须在框的两两比较和产物目录创建前拒绝。
    for key, sample in samples.items():
        ctx.check()
        if sample["width"] > 16384 or sample["height"] > 16384:
            raise PipelineError("浏览器审核单图宽高不能超过16384；请单独处理超限原图")
        for boxes in (sample["detections"], predicted.get(key, []), reviews.get(key, {}).get("detections", [])):
            if len(boxes) > 1024:
                raise PipelineError("浏览器审核单图框数不能超过1024；原始、离线预测和已有审核均须满足")
    if not REVIEW_TEMPLATE.is_file():
        raise PipelineError("缺少本地 model_data_review.html 审核模板")
    template = REVIEW_TEMPLATE.read_text(encoding="utf-8")
    if template.count("__XEN_REVIEW_DATA__") != 1:
        raise PipelineError("审核模板数据占位符必须唯一")
    output = ctx.output()
    (output / "images").mkdir()
    exact = Counter(sample["image_sha256"] for sample in samples.values())
    entries, representatives = [], []
    seen_hashes, near_groups = set(), set()
    display_groups = {}
    from PIL import Image
    for index, (key, sample) in enumerate(sorted(samples.items())):
        ctx.check()
        reasons, priority = [], 0
        original = sample["detections"]
        reviewed = reviews.get(key)
        boxes = reviewed["detections"] if reviewed else predicted.get(key, original)
        state = reviewed["state"] if reviewed else "UNKNOWN"
        if not original:
            reasons.append("原始预标注为空，需确认是否漏检"); priority = max(priority, 80)
        if sample.get("detection_status") != "SUCCESS":
            reasons.append("原始检测未成功或状态未知，空框不能作为正常负例"); priority = max(priority, 100)
        if any(min(box["x2"]-box["x1"], box["y2"]-box["y1"]) < 8 for box in original):
            reasons.append("存在最小边不足8px的小框，仅提示放大检查，不判无效"); priority = max(priority, 65)
        if any(min(box["x1"], box["y1"], sample["width"]-box["x2"], sample["height"]-box["y2"]) <= 1 for box in original):
            reasons.append("存在距图边不超过1px的框，需核对截断与遮挡"); priority = max(priority, 65)
        if any(value is not None and value < 0.5 for value in sample["_confidences"]):
            reasons.append("原始预标注存在低置信框"); priority = max(priority, 60)
        if any(box_iou(a, b) >= 0.7 for i, a in enumerate(original) for b in original[i+1:]):
            reasons.append("原始框高度重叠，需核对重复或类别冲突"); priority = max(priority, 70)
        if key in predicted:
            disagreement = prediction_disagreements(original, predicted[key])
            if disagreement["unmatched_original"] or disagreement["unmatched_predicted"]:
                reasons.append(f"离线模型框数或位置分歧：原始未配对{disagreement['unmatched_original']}，离线未配对{disagreement['unmatched_predicted']}")
                priority = max(priority, 90)
            if disagreement["class_conflicts"]:
                reasons.append("离线模型与原始预标注类别分歧"); priority = max(priority, 100)
            if disagreement["position_conflicts"]:
                reasons.append("离线模型与原始预标注定位分歧"); priority = max(priority, 85)
            if not original and not predicted[key]:
                reasons.append("两份预测均为空，仍不能自动视为负样本")
            if prediction_identity["settings"] is None:
                reasons.append("离线推理参数未记录，分歧仅供提示")
        if exact[sample["image_sha256"]] > 1:
            reasons.append(f"原图完全重复组，共{exact[sample['image_sha256']]}张；未删除任何样本")
        near = None
        # 有界最近代表比较；不会宣称穷尽全库近重复，更不会传播标签。
        if sample["image_sha256"] not in seen_hashes:
            for other_key, other in reversed(representatives):
                distance = sum(abs(x-y) for a, b in zip(sample["_thumb"], other["_thumb"]) for x, y in zip(a, b)) / 768
                if distance <= 2:
                    near = other_key
                    near_groups.add(other_key)
                    break
            if near is None:
                representatives.append((key, sample))
                representatives = representatives[-64:]
            display_groups[sample["image_sha256"]] = samples[near]["image_sha256"] if near else sample["image_sha256"]
            seen_hashes.add(sample["image_sha256"])
        if near is not None:
            reasons.append(f"画面近似{near[0]}/{near[1]}，仅提示，须保留小目标差异")
            if prediction_disagreements(original, samples[near]["detections"]) != dict(unmatched_original=0, unmatched_predicted=0, class_conflicts=0, position_conflicts=0):
                reasons.append("近似画面的框或类别有变化，不可传播标签")
        if reviewed:
            reasons.insert(0, "已有人工审核；自动建议不会覆盖审核结果")
        if not reasons:
            reasons.append("待人工核对")
        image = f"images/{key[0]}__{key[1]}.png"
        with Image.open(sample["_path"]) as picture:
            picture = picture.convert("RGB")
            picture.thumbnail((640, 640), Image.Resampling.LANCZOS)
            picture.save(output / image, format="PNG")
        entries.append(dict(session_id=key[0], sample_id=key[1], image_sha256=sample["image_sha256"],
                            width=sample["width"], height=sample["height"], image=image, detections=boxes,
                            state=state, reasons=reasons, group=display_groups[sample["image_sha256"]], priority=priority,
                            base_review_token=review_token(reviewed)))
        ctx.report("RUNNING", f"自动整理 {index+1}/{len(samples)}；未自动确认审核")
    near_hashes = {samples[key]["image_sha256"] for key in near_groups}
    for item in entries:
        item["group_kind"] = "near" if item["group"] in near_hashes else "exact" if exact[item["image_sha256"]] > 1 else "single"
    entries.sort(key=lambda item: (-item["priority"], item["session_id"], item["sample_id"]))
    summary = dict(original_count=len(entries), exact_unique_count=len(exact),
                   near_duplicate_group_count=len(near_groups),
                   near_duplicate_scope="最近64个不同图像代表的缩略图提示；不是独立场景数量")
    payload = dict(schema_version=SCHEMA, class_names=names, summary=summary,
                   root_identity=review_root_identity(ctx.job["root"], names), samples=entries)
    report = dict(payload, policy=dict(version=1, low_confidence=0.5, overlap_iou=0.7,
                                      match_iou=0.5, position_iou=0.8, near_thumbnail_mad=2,
                                      automatic_verification=False, near_duplicates="recent_64_representative_hints_only"),
                  prediction=prediction_identity, counts=dict(samples=len(entries),
                  exact_duplicate_groups=sum(count > 1 for count in exact.values()),
                  states=dict(Counter(item["state"] for item in entries))))
    write_json(output / "triage.json", report)
    encoded = json.dumps(payload, ensure_ascii=False, allow_nan=False).replace("<", "\\u003c").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    (output / "review.html").write_text(template.replace("__XEN_REVIEW_DATA__", encoded), encoding="utf-8")
    return dict(review_preview=str(output / "review.html"), triage_report=str(output / "triage.json"),
                **summary, samples=len(entries), message="已自动整理全部样本；仅人工明确处理并导入的条目进入审核修订")


@contextmanager
def review_write_lock(root):
    directory = Path(root).resolve() / "reviews"
    directory.mkdir(parents=True, exist_ok=True)
    lock = directory / ".import.lock"
    try:
        stream = lock.open("x", encoding="utf-8")
    except FileExistsError as exc:
        raise PipelineError("其他审核导入正在写入或留下未恢复锁，拒绝并发覆盖") from exc
    try:
        with stream:
            stream.write(str(os.getpid()))
        yield directory
    finally:
        lock.unlink()


def append_review_revision(directory, names, reviewer, manifest_path, accepted):
    timestamp = datetime.now(timezone.utc)
    previous = sorted(p.name for p in directory.iterdir() if (p / "revision.json").is_file())
    if previous:
        try:
            last_time = datetime.strptime(previous[-1].split("-", 1)[0], "%Y%m%dT%H%M%S%fZ").replace(tzinfo=timezone.utc)
        except ValueError as exc:
            raise PipelineError("既有审核修订名称不能安全排序，拒绝产生可能失效的新修订") from exc
        if timestamp <= last_time:
            timestamp = last_time + timedelta(microseconds=1)
    revision_id = timestamp.strftime("%Y%m%dT%H%M%S%fZ") + "-" + uuid.uuid4().hex[:8]
    destination = directory / revision_id
    destination.mkdir(exist_ok=False)
    # revision.json 最后原子出现；原样本、旧修订和未完成目录均不覆盖。
    write_json(destination / "revision.json", dict(schema_version=SCHEMA, revision_id=revision_id,
               created_at=now(), class_names=names, reviewer=reviewer,
               source_manifest_sha256=sha256(manifest_path), samples=accepted))
    return dict(revision=str(destination), samples=len(accepted), states=dict(Counter(i["state"] for i in accepted)))


def import_labels(ctx):
    job = dict(ctx.job)
    manifest_path = Path(job.get("review_manifest") or job.get("session") or "").resolve()
    if not manifest_path.is_file():
        raise PipelineError("必须指定已人工填写的 review_manifest 文件")
    job.pop("session", None)
    names, samples = load_samples(Context(job, cancel=ctx.cancel))
    manifest = read_json(manifest_path)
    if manifest.get("schema_version") != SCHEMA or manifest.get("class_names") != names:
        raise PipelineError("审核 manifest schema 或类别不匹配")
    native = manifest.get("format") == "xen-native-review-v1"
    if manifest.get("format") not in (None, "xen-native-review-v1"):
        raise PipelineError("未知审核格式")
    if native and manifest.get("root_identity") != review_root_identity(job["root"], names):
        raise PipelineError("审核数据根身份不匹配")
    with review_write_lock(job["root"]) as directory:
        current = latest_reviews(job["root"], samples, names)
        accepted, seen = [], set()
        for item in manifest.get("samples", []):
            ctx.check()
            key = (item["session_id"], item["sample_id"])
            if key in seen or key not in samples:
                raise PipelineError("审核 manifest 包含重复或未知样本")
            seen.add(key)
            sample = samples[key]
            if item.get("image_sha256") != sample["image_sha256"]:
                raise PipelineError("审核图片哈希不匹配")
            state = item.get("state")
            if state == "UNKNOWN" and not native:
                continue
            if state not in VERIFIED | {"EXCLUDED"}:
                raise PipelineError("必须显式设置审核正例、负例或排除状态；UNKNOWN 不能训练")
            if native:
                if item.get("base_review_token") != review_token(current.get(key)):
                    raise PipelineError("所选样本审核已更新，请重新整理后复核，不能覆盖过期审核")
                boxes = boxes_checked(item["detections"], sample["width"], sample["height"], names)
            else:
                boxes = [] if state == "EXCLUDED" else read_labels(contained(manifest_path.parent, item["label"]), sample["width"], sample["height"], names)
            if (state in {"VERIFIED_NEGATIVE", "EXCLUDED"} and boxes) or (state == "VERIFIED_POSITIVE" and not boxes):
                raise PipelineError("审核正负或排除状态与标签内容矛盾")
            accepted.append(dict(session_id=key[0], sample_id=key[1], image_sha256=sample["image_sha256"], state=state, detections=boxes))
        if not accepted:
            raise PipelineError("没有明确人工审核的样本；UNKNOWN 不能训练")
        ctx.check()
        return append_review_revision(directory, names, manifest.get("reviewer", ""), manifest_path, accepted)


def check_leakage(entries):
    # RGB 缩略图平均绝对差 <= 2/255 视为近重复；保守拒绝，人工重新分组。
    for index, left in enumerate(entries):
        for right in entries[index + 1:]:
            if left["split"] == right["split"]:
                continue
            if left["sample"]["image_sha256"] == right["sample"]["image_sha256"]:
                raise PipelineError("相同图片跨 train/val/test 泄漏，请调整 session 分组")
            a, b = left["sample"]["_thumb"], right["sample"]["_thumb"]
            distance = sum(abs(x-y) for p, q in zip(a, b) for x, y in zip(p, q)) / (len(a)*3)
            if distance <= 2:
                raise PipelineError("近重复图片跨 train/val/test；请将关联 session 放同组或排除重复")


def check_annotation_consistency(entries):
    identities = {}
    for entry in entries:
        sample, review = entry["sample"], entry["review"]
        signature = (review["state"], yolo_text(review["detections"], sample["width"], sample["height"]))
        previous = identities.setdefault(sample["image_sha256"], signature)
        if previous != signature:
            raise PipelineError("同一图片存在矛盾的审核标签或正负状态，请先纠正审核")


def export_dataset(ctx):
    names, samples = load_samples(ctx)
    reviews = latest_reviews(ctx.job["root"], samples, names)
    valid = {key: sample for key, sample in samples.items() if reviews.get(key, {}).get("state") in VERIFIED}
    groups = sorted({key[0] for key in valid}, key=lambda sid: hashlib.sha256(sid.encode()).hexdigest())
    if len(groups) < 3:
        raise PipelineError("至少需要 3 个有已审核样本的独立 session；不能随机拆分相邻帧")
    split_groups = ctx.job.get("split_groups")
    registry_path = Path(ctx.job["root"]).resolve() / "split_registry.json"
    previous = read_json(registry_path) if registry_path.exists() else None
    if previous and (previous.get("schema_version") != SCHEMA or previous.get("class_names") != names):
        raise PipelineError("既有 split registry 类别 schema 不一致")
    old_groups = previous["split_groups"] if previous else {}
    if split_groups is None:
        if old_groups:
            split_groups = {sid: old_groups.get(sid, "train") for sid in groups}
        else:
            test_count = max(1, round(len(groups)*0.15))
            val_count = max(1, round(len(groups)*0.15))
            split_groups = {sid: "test" if i < test_count else "val" if i < test_count+val_count else "train" for i, sid in enumerate(groups)}
    if set(split_groups) != set(groups) or set(split_groups.values()) != {"train", "val", "test"}:
        raise PipelineError("split_groups 必须完整映射有效 session，且 train/val/test 都非空")
    if any(sid in old_groups and old_groups[sid] != split for sid, split in split_groups.items()):
        raise PipelineError("既有 session split 不可改变，避免旧留出集回流训练")
    entries = [dict(key=key, sample=sample, review=reviews[key], split=split_groups[key[0]]) for key, sample in valid.items()]
    check_annotation_consistency(entries)
    check_leakage(entries)
    for split in ("train", "val", "test"):
        covered = {b["class_id"] for e in entries if e["split"] == split for b in e["review"]["detections"]}
        if covered != set(range(len(names))):
            raise PipelineError(f"{split} 缺少某些类别的正例；请补充独立已审核数据")
    output = ctx.output()
    records = []
    for entry in entries:
        ctx.check()
        sample, review, split = entry["sample"], entry["review"], entry["split"]
        basename = "__".join(entry["key"])
        image = f"images/{split}/{basename}.png"
        label = f"labels/{split}/{basename}.txt"
        (output / image).parent.mkdir(parents=True, exist_ok=True)
        (output / label).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sample["_path"], output / image)
        (output / label).write_text(yolo_text(review["detections"], sample["width"], sample["height"]), encoding="utf-8")
        records.append(dict(session_id=entry["key"][0], sample_id=entry["key"][1], split=split, image=image, label=label, image_sha256=sha256(output / image), label_sha256=sha256(output / label), width=sample["width"], height=sample["height"], state=review["state"], revision=review["revision"]))
    # JSON 是 YAML 1.2 子集，无需依赖 YAML 库即可写标准 data.yaml。
    write_json(output / "data.yaml", dict(path=str(output), train="images/train", val="images/val", test="images/test", names=names))
    manifest = dict(schema_version=SCHEMA, created_at=now(), class_names=names, split_groups=split_groups, data_yaml_sha256=sha256(output / "data.yaml"), samples=records, counts=dict(Counter(r["split"] for r in records)), states=dict(Counter(r["state"] for r in records)), leakage_check="sha256 + RGB 16x16 MAD <= 2; session 分组", source_root=str(Path(ctx.job["root"]).resolve()))
    write_json(output / "dataset.json", manifest)
    write_json(output / "dataset_identity.json", dict(dataset_sha256=sha256(output / "dataset.json")))
    write_json(registry_path, dict(schema_version=SCHEMA, class_names=names, split_groups=dict(old_groups, **split_groups), updated_at=now()))
    return dict(output=str(output), dataset=str(output), dataset_sha256=sha256(output / "dataset.json"), counts=manifest["counts"], states=manifest["states"])


def validate_dataset(path):
    path = Path(path).resolve()
    manifest = read_json(path / "dataset.json")
    identity = read_json(path / "dataset_identity.json")
    names = class_schema(manifest.get("class_names"))
    if manifest.get("schema_version") != SCHEMA or identity.get("dataset_sha256") != sha256(path / "dataset.json") or manifest.get("data_yaml_sha256") != sha256(path / "data.yaml"):
        raise PipelineError("冻结数据版本身份不匹配")
    config = read_json(path / "data.yaml")
    if config != dict(path=str(path), train="images/train", val="images/val", test="images/test", names=names):
        raise PipelineError("冻结 data.yaml 不得重定向或添加下载配置；移动目录后请重新导出")
    expected_images, expected_labels, entries, groups = set(), set(), [], {}
    keys = set()
    for item in manifest["samples"]:
        split, state = item["split"], item["state"]
        key = (identifier(item["session_id"]), identifier(item["sample_id"]))
        if key in keys or split not in {"train", "val", "test"} or state not in VERIFIED:
            raise PipelineError("冻结数据含重复、未知状态或无效 split")
        keys.add(key)
        if key[0] in groups and groups[key[0]] != split:
            raise PipelineError("同 session 跨 split")
        groups[key[0]] = split
        image, label = contained(path, item["image"]), contained(path, item["label"])
        if image.parent != path / "images" / split or label.parent != path / "labels" / split or image.stem != label.stem:
            raise PipelineError("导出图片标签目录或名称不匹配")
        if image in expected_images or label in expected_labels or sha256(image) != item["image_sha256"] or sha256(label) != item["label_sha256"]:
            raise PipelineError("导出文件重复或 SHA-256 不匹配")
        (width, height), thumb = image_info(image)
        if (width, height) != (item["width"], item["height"]):
            raise PipelineError("导出图片尺寸改变")
        boxes = read_labels(label, width, height, names)
        if (state == "VERIFIED_NEGATIVE" and boxes) or (state == "VERIFIED_POSITIVE" and not boxes):
            raise PipelineError("冻结数据正负状态与标签矛盾")
        expected_images.add(image)
        expected_labels.add(label)
        entries.append(dict(split=split, sample=dict(image_sha256=item["image_sha256"], width=width, height=height, _thumb=thumb), review=dict(state=state, detections=boxes)))
    actual_images = {p.resolve() for p in (path / "images").rglob("*") if p.is_file()}
    actual_labels = {p.resolve() for p in (path / "labels").rglob("*") if p.is_file()}
    if expected_images != actual_images or expected_labels != actual_labels or set(groups.values()) != {"train", "val", "test"} or groups != manifest["split_groups"]:
        raise PipelineError("冻结数据白名单/分组不完整，拒绝混入未审核图片")
    check_leakage(entries)
    check_annotation_consistency(entries)
    return manifest


def check_pt_trust(model_path, job):
    """只确认当前作业明确指定的文件；ONNX 不经过 PT 反序列化门禁。"""
    path = Path(model_path).resolve()
    if path.suffix.lower() != ".pt":
        return
    if not isinstance(job, dict) or job.get("trusted_weights") is not True:
        raise PipelineError("PT 来源尚未明确确认可信，拒绝加载")
    confirmed = job.get("trusted_weights_path")
    if not isinstance(confirmed, str) or not confirmed or not Path(confirmed).is_absolute() or Path(confirmed).resolve() != path:
        raise PipelineError("当前 PT 与明确确认的可信文件不同，拒绝加载")
    expected = job.get("expected_weights_sha256")
    if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected):
        raise PipelineError("可信 PT 缺少有效 expected_weights_sha256")
    if not path.is_file() or sha256(path) != expected.lower():
        raise PipelineError("可信 PT 的 SHA-256 已变化或文件缺失，拒绝加载")


def load_yolo(model_path, training=False, *, job=None, internal_training_output=False):
    path = Path(model_path).resolve()
    allowed = {".pt"} if training else {".pt", ".onnx"}
    if not path.is_file() or path.suffix.lower() not in allowed:
        raise PipelineError("必须提供已存在的本地 .pt 权重" if training else "必须提供已存在的本地 .pt/.onnx 模型")
    if not internal_training_output:
        check_pt_trust(path, job)
    # 禁止训练器自动补依赖或下载示例权重；权重只从显式本地路径读取。
    os.environ["YOLO_AUTOINSTALL"] = "false"
    os.environ["YOLO_OFFLINE"] = "true"
    try:
        import torch  # noqa: F401
        import ultralytics
        from ultralytics import YOLO
        if training:
            import onnx  # noqa: F401
    except ImportError as exc:
        raise PipelineError("DEPENDENCY_MISSING：独立 Python 缺少 torch/ultralytics/onnx；请按 model_training_requirements.txt 配置环境") from exc
    if ultralytics.__version__ != TRAINER_VERSION:
        raise PipelineError(f"DEPENDENCY_VERSION：要求 ultralytics=={TRAINER_VERSION}，拒绝未知导出/NMS 语义")
    # 独立本地作业不接入用户环境中可能已经安装的云端日志集成。
    from ultralytics.utils import callbacks
    callbacks.add_integration_callbacks = lambda _instance: None
    model = YOLO(str(path), task="detect")
    if model.task != "detect":
        raise PipelineError("首版仅支持二维目标检测模型")
    return model


def model_names(model):
    names = model.names
    return [names[i] for i in range(len(names))] if isinstance(names, dict) else list(names)


def prelabel(ctx):
    check_pt_trust(ctx.job.get("model", ""), ctx.job)
    names, samples = load_samples(ctx)
    model = load_yolo(ctx.job.get("model", ""), job=ctx.job)
    if model_names(model) != names:
        raise PipelineError("预标注模型类别名称与采集 schema 不一致")
    output = ctx.output()
    records = []
    for index, (key, sample) in enumerate(samples.items()):
        ctx.check()
        results = model.predict(source=str(sample["_path"]), conf=0.1, device=ctx.job.get("device", "cpu"), verbose=False, save=False)
        boxes = []
        for row in results[0].boxes.data.cpu().tolist():
            x1, y1, x2, y2, confidence, cls = row
            boxes.append(dict(class_id=int(cls), x1=x1, y1=y1, x2=x2, y2=y2, confidence=confidence))
        boxes_checked(boxes, sample["width"], sample["height"], names)
        records.append(dict(session_id=key[0], sample_id=key[1], image_sha256=sample["image_sha256"], state="PRELABELED", detections=boxes))
        ctx.report("RUNNING", f"预标注 {index+1}/{len(samples)}；等待人工审核")
    write_json(output / "prelabels.json", dict(schema_version=SCHEMA, class_names=names, model_sha256=sha256(ctx.job["model"]), samples=records))
    return dict(output=str(output), prelabels=str(output / "prelabels.json"), samples=len(records), state="PRELABELED")


def versions():
    result = {"python": platform.python_version(), "platform": platform.platform()}
    for package in ("ultralytics", "torch", "torchvision", "onnx", "onnxruntime"):
        try:
            result[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            result[package] = "未安装"
    return result


def onnx_contract(path, class_count, imgsz):
    try:
        import onnx
    except ImportError as exc:
        raise PipelineError("DEPENDENCY_MISSING：缺少 onnx，无法核验导出合同") from exc
    graph = onnx.load(str(path))
    onnx.checker.check_model(graph)
    dimensions = lambda value: [dim.dim_value for dim in value.type.tensor_type.shape.dim]
    if len(graph.graph.input) != 1 or dimensions(graph.graph.input[0]) != [1, 3, imgsz, imgsz] or graph.graph.input[0].type.tensor_type.elem_type != onnx.TensorProto.FLOAT:
        raise PipelineError("ONNX 输入不符合 batch=1 RGB FP32 静态尺寸合同")
    if len(graph.graph.output) != 1:
        raise PipelineError("ONNX 必须为单个原始检测输出")
    shape = dimensions(graph.graph.output[0])
    if len(shape) != 3 or shape[0] != 1 or shape[1] != 4+class_count or shape[2] <= shape[1] or any(node.op_type == "NonMaxSuppression" for node in graph.graph.node):
        raise PipelineError("ONNX 输出不是 [1,4+nc,N] 外部 NMS 合同；候选不可部署")
    return dict(input_shape=[1, 3, imgsz, imgsz], output_shape=shape, input_type="float32", nms="external", checker="passed", detector_runtime="NOT_EXECUTED", provider_performance="NOT_EXECUTED")


def resume_source(job, dataset, manifest):
    weights = Path(job.get("weights", "")).resolve()
    if weights.name != "last.pt" or weights.parent.name != "weights" or weights.parent.parent.name != "run":
        raise PipelineError("续训必须选择本工具未完成作业的 run/weights/last.pt")
    original = weights.parent.parent.parent
    if (original / "candidate.json").exists():
        raise PipelineError("原作业已有完整候选；不能把新的微调伪装为续训")
    config = read_json(original / "config.json")
    checkpoint = read_json(original / "checkpoint.json")
    identity = sha256(dataset / "dataset.json")
    if config.get("dataset_sha256") != identity or checkpoint.get("dataset_sha256") != identity or config.get("class_names") != manifest["class_names"]:
        raise PipelineError("续训数据版本或类别不同；新数据必须启动新实验")
    if checkpoint.get("config_sha256") != sha256(original / "config.json") or checkpoint.get("last_sha256") != sha256(weights):
        raise PipelineError("续训检查点与原作业身份不匹配")
    best = weights.with_name("best.pt")
    if not best.is_file() or checkpoint.get("best_sha256") != sha256(best):
        raise PipelineError("续训需要原作业完整的 best.pt 历史最佳检查点")
    effective = dict(config["effective_training"])
    effective["workers"] = training_workers(effective.get("workers", 0))
    if "workers" in job:
        training_workers(job["workers"])
    for name in ("epochs", "imgsz", "workers"):
        if name in job and job[name] != effective[name]:
            raise PipelineError(f"续训必须保持原 {name}={effective[name]}；改变训练预算/尺寸请开新实验")
    return dict(output=str(original), config=config, checkpoint=checkpoint, best=best, effective=effective)


def redirected_resume_trainer(base_class, output, dataset, source_best, workers=0):
    """v8.3.203 在 check_resume 后才创建 save_dir；保持原生状态恢复。"""
    class ResumeTrainer(base_class):
        def check_resume(self, overrides):
            super().check_resume(overrides)
            self.args.save_dir = str(output / "run")
            self.args.project = str(output)
            self.args.name = "run"
            self.args.exist_ok = False
            self.args.data = str(dataset / "data.yaml")
            self.args.workers = workers

        def resume_training(self, checkpoint):
            super().resume_training(checkpoint)
            # 保留原历史最佳，避免后续没有更高 fitness 时新目录缺 best.pt。
            if not Path(self.best).exists():
                shutil.copyfile(source_best, self.best)
    return ResumeTrainer


def detection_trainer_class():
    from ultralytics.models.yolo.detect import DetectionTrainer
    return DetectionTrainer


def working_dataset(ctx, dataset, manifest, output):
    """训练器会在标签旁写缓存；仅向作业独占副本传递数据，冻结白名单保持严格。"""
    working = output / "working-dataset"
    working.mkdir(exist_ok=False)
    for item in manifest["samples"]:
        ctx.check()
        for key in ("image", "label"):
            source = contained(dataset, item[key])
            destination = contained(working, item[key])
            destination.parent.mkdir(parents=True, exist_ok=True)
            # 不能硬链接：第三方若写回图片/标签，不得改变冻结版本。
            shutil.copyfile(source, destination)
            if sha256(destination) != item[f"{key}_sha256"]:
                raise PipelineError("工作数据副本与冻结文件 SHA-256 不一致")
    write_json(working / "data.yaml", dict(path=str(working), train="images/train", val="images/val", test="images/test", names=manifest["class_names"]))
    return working


def train(ctx):
    check_pt_trust(ctx.job.get("weights", ""), ctx.job)
    dataset = Path(ctx.job["dataset"]).resolve()
    manifest = validate_dataset(dataset)
    if ctx.job.get("class_names") and ctx.job["class_names"] != manifest["class_names"]:
        raise PipelineError("作业 class_names 与冻结数据不一致")
    resume = ctx.job.get("resume", False)
    if not isinstance(resume, bool):
        raise PipelineError("resume 必须为 JSON 布尔值")
    source = resume_source(ctx.job, dataset, manifest) if resume else None
    workers = training_workers(source["effective"]["workers"] if source else ctx.job.get("workers", 0))
    epochs = positive_int(source["effective"]["epochs"] if source else ctx.job.get("epochs", 20), "epochs", 10000)
    imgsz = positive_int(source["effective"]["imgsz"] if source else ctx.job.get("imgsz", 640), "imgsz", 4096)
    batch = positive_int(ctx.job.get("batch", 8), "batch", 1024)
    if imgsz % 32:
        raise PipelineError("imgsz 必须为 32 的倍数")
    device = str(ctx.job.get("device", "cpu"))
    if device != "cpu" and not re.fullmatch(r"\d+", device):
        raise PipelineError("首版只支持 cpu 或一个 GPU ID，避免多进程取消失配")
    model = load_yolo(ctx.job.get("weights", ""), training=True, job=ctx.job)
    if getattr(model.model, "end2end", False) or getattr(model.model.model[-1], "end2end", False):
        raise PipelineError("首版拒绝 end-to-end 检测头；需要经核验的外部 NMS 输出")
    if source:
        checkpoint = getattr(model, "ckpt", None)
        if not isinstance(checkpoint, dict) or checkpoint.get("optimizer") is None or not isinstance(checkpoint.get("epoch"), int) or not 0 <= checkpoint["epoch"] < epochs-1:
            raise PipelineError("last.pt 没有可恢复的 optimizer/epoch，或已到总轮数；不能作为真实续训")
        if checkpoint["epoch"] != source["checkpoint"]["epoch"] or model_names(model) != manifest["class_names"]:
            raise PipelineError("续训 checkpoint 的 epoch/类别与原作业不一致")
    output = ctx.output()
    work_data = working_dataset(ctx, dataset, manifest, output)
    config = dict(schema_version=SCHEMA, job=ctx.job, dataset_sha256=sha256(dataset / "dataset.json"), class_names=manifest["class_names"], effective_training=dict(epochs=epochs, imgsz=imgsz, batch=batch, device=device, workers=workers), weights_sha256=sha256(ctx.job["weights"]), versions=versions(), started_at=now(), resume_from=source["output"] if source else None)
    write_json(output / "config.json", config)
    cancelled = False
    last_progress = 0.0

    def on_batch_end(trainer):
        nonlocal last_progress
        current = time.monotonic()
        if current - last_progress >= 1:
            last_progress = current
            requested = ctx.cancel and ctx.cancel.exists()
            ctx.report("RUNNING", "收到取消；等待本 epoch 完成以保存检查点" if requested else f"训练 epoch {trainer.epoch+1}/{epochs}", dict(epoch=trainer.epoch+1, epochs=epochs, output=str(output)))

    def on_epoch_end(trainer):
        nonlocal cancelled
        if ctx.cancel and ctx.cancel.exists():
            cancelled = True
            trainer.stop = True
        ctx.report("RUNNING", "收到取消；等待本轮验证和检查点保存" if cancelled else f"训练 epoch {trainer.epoch+1}/{epochs}", dict(epoch=trainer.epoch+1, epochs=epochs, output=str(output)))

    def on_model_save(trainer):
        directory = Path(trainer.save_dir) / "weights"
        last, best = directory / "last.pt", directory / "best.pt"
        write_json(output / "checkpoint.json", dict(schema_version=SCHEMA, dataset_sha256=config["dataset_sha256"], config_sha256=sha256(output / "config.json"), last_sha256=sha256(last), best_sha256=sha256(best) if best.is_file() else None, epoch=trainer.epoch, updated_at=now()))
        if cancelled:
            # 此回调在 save_model 之后；抛出避免 trainer.final_eval 清除优化器状态。
            raise Cancelled("训练已在 epoch 边界保存 last.pt 后取消")

    model.add_callback("on_train_epoch_end", on_epoch_end)
    model.add_callback("on_train_batch_end", on_batch_end)
    model.add_callback("on_model_save", on_model_save)
    ctx.check()
    resume_options = {}
    if source:
        resume_options = dict(resume=True, trainer=redirected_resume_trainer(detection_trainer_class(), output, work_data, source["best"], workers))
    result = model.train(data=str(work_data / "data.yaml"), epochs=epochs, imgsz=imgsz, batch=batch, device=device, project=str(output), name="run", exist_ok=False, workers=workers, seed=0, deterministic=True, pretrained=False, amp=False, save=True, save_period=1, plots=False, val=True, patience=20, **resume_options)
    ctx.check()
    run_dir = Path(model.trainer.save_dir)
    best, last = run_dir / "weights" / "best.pt", run_dir / "weights" / "last.pt"
    if not best.is_file() or not last.is_file():
        raise PipelineError("训练器没有生成 best.pt/last.pt")
    metrics = {str(k): float(v) for k, v in result.results_dict.items()}
    write_json(output / "validation_metrics.json", metrics)
    # 同一作业刚生成并核验的 best.pt 是本次已授权训练的内部产物。
    candidate = load_yolo(best, training=True, internal_training_output=True)
    if model_names(candidate) != manifest["class_names"]:
        raise PipelineError("训练后类别 schema 与数据集不一致")
    ctx.report("RUNNING", "训练完成，正在导出与核验候选 ONNX")
    onnx_path = Path(candidate.export(format="onnx", imgsz=imgsz, batch=1, dynamic=False, half=False, simplify=False, nms=False, opset=17, device="cpu"))
    ctx.check()
    contract = onnx_contract(onnx_path, len(manifest["class_names"]), imgsz)
    card = dict(schema_version=SCHEMA, state="CANDIDATE_NOT_ACTIVATED", created_at=now(), dataset_sha256=config["dataset_sha256"], class_names=manifest["class_names"], best=str(best), last=str(last), best_sha256=sha256(best), onnx=str(onnx_path), onnx_sha256=sha256(onnx_path), validation_metrics=metrics, versions=config["versions"], contract=contract, test_evaluation="NOT_EXECUTED", physical_acceptance="NOT_EXECUTED")
    write_json(output / "candidate.json", card)
    return dict(output=str(output), candidate=str(onnx_path.resolve()), candidate_card=str(output / "candidate.json"), state=card["state"], onnx=str(onnx_path.resolve()))


def evaluation_json_value(value):
    """仅接受指标的标准JSON结构和NumPy数值标量，不把未知对象字符串化。"""
    import numpy as np
    if isinstance(value, np.generic):
        if value.dtype.kind not in "biufU":
            raise PipelineError("评估指标含不支持的 NumPy 标量")
        value = value.item()
    if value is None or type(value) in (bool, int, str):
        return value
    if type(value) is float:
        if not math.isfinite(value):
            raise PipelineError("评估指标必须为有限数值")
        return value
    if type(value) is list:
        return [evaluation_json_value(item) for item in value]
    if type(value) is dict:
        if any(type(key) is not str for key in value):
            raise PipelineError("评估指标对象键必须为字符串")
        return {key: evaluation_json_value(item) for key, item in value.items()}
    raise PipelineError("评估指标含不支持的对象类型")


def evaluate(ctx):
    workers = training_workers(ctx.job.get("workers", 0))
    for key in ("model", "baseline_model"):
        if ctx.job.get(key):
            check_pt_trust(ctx.job[key], ctx.job)
    dataset = Path(ctx.job["dataset"]).resolve()
    manifest = validate_dataset(dataset)
    split = ctx.job.get("split", "test")
    if split not in {"val", "test"}:
        raise PipelineError("评价只能使用 val 或 test 留出集")
    imgsz = positive_int(ctx.job.get("imgsz", 640), "imgsz", 4096)
    confidence = float(ctx.job.get("conf", 0.25))
    if not 0 <= confidence <= 1:
        raise PipelineError("评价 conf 必须为 0..1")
    output = ctx.output()

    work_data = working_dataset(ctx, dataset, manifest, output)

    def overlap(left, right):
        intersection = max(0, min(left[2], right[2])-max(left[0], right[0])) * max(0, min(left[3], right[3])-max(left[1], right[1]))
        union = (left[2]-left[0])*(left[3]-left[1]) + (right[2]-right[0])*(right[3]-right[1]) - intersection
        return intersection / union if union > 0 else 0

    def measure(model_path, name):
        ctx.check()
        model = load_yolo(model_path, job=ctx.job)
        if model_names(model) != manifest["class_names"]:
            raise PipelineError("评价模型 class_names 与真值不一致")
        def check_batch(_validator):
            ctx.check()
        model.add_callback("on_val_batch_end", check_batch)
        # mAP 使用同一低置信门槛构建曲线；背景误报另用固定部署候选门槛。
        result = model.val(data=str(work_data / "data.yaml"), split=split, imgsz=imgsz, conf=0.001, iou=0.7, batch=ctx.job.get("batch", 8), device=ctx.job.get("device", "cpu"), project=str(output), name=name, plots=False, save_json=False, workers=workers)
        ctx.check()
        negative_frames = false_positive_frames = false_positives = 0
        fixed = [dict(class_id=i, name=name, tp=0, fp=0, fn=0) for i, name in enumerate(manifest["class_names"])]
        for item in manifest["samples"]:
            if item["split"] != split:
                continue
            ctx.check()
            predictions = model.predict(source=str(work_data / item["image"]), conf=confidence, iou=0.7, imgsz=imgsz, device=ctx.job.get("device", "cpu"), verbose=False, save=False)
            rows = predictions[0].boxes.data.cpu().tolist()
            truth = read_labels(dataset / item["label"], item["width"], item["height"], manifest["class_names"])
            unmatched = set(range(len(truth)))
            for row in sorted(rows, key=lambda row: -row[4]):
                cls = int(row[5])
                if not 0 <= cls < len(fixed):
                    raise PipelineError("模型输出了 schema 之外的类别")
                matches = [(overlap(row, [truth[i][k] for k in ("x1", "y1", "x2", "y2")]), i) for i in unmatched if truth[i]["class_id"] == cls]
                best_iou, best_index = max(matches, default=(0, -1))
                if best_iou >= 0.5:
                    fixed[cls]["tp"] += 1
                    unmatched.remove(best_index)
                else:
                    fixed[cls]["fp"] += 1
            for i in unmatched:
                fixed[truth[i]["class_id"]]["fn"] += 1
            if item["state"] == "VERIFIED_NEGATIVE":
                negative_frames += 1
                false_positives += len(rows)
                false_positive_frames += bool(rows)
        for counts in fixed:
            counts["precision"] = counts["tp"] / (counts["tp"] + counts["fp"]) if counts["tp"] + counts["fp"] else None
            counts["recall"] = counts["tp"] / (counts["tp"] + counts["fn"]) if counts["tp"] + counts["fn"] else None
        return dict(model=str(Path(model_path).resolve()), model_sha256=sha256(model_path), metrics={str(k): float(v) for k, v in result.results_dict.items()}, per_class=result.summary(), fixed_threshold_per_class=fixed, fixed_threshold_match_iou=0.5, negative_frames=negative_frames, background_fp_per_frame=false_positives/negative_frames if negative_frames else None, background_false_positive_frame_rate=false_positive_frames/negative_frames if negative_frames else None)

    report = dict(schema_version=SCHEMA, created_at=now(), dataset_sha256=sha256(dataset / "dataset.json"), split=split, confidence=confidence, map_confidence_floor=0.001, nms_iou=0.7, versions=versions(), physical_acceptance="NOT_EXECUTED", **measure(ctx.job["model"], "validation"))
    baseline = None
    comparison = dict(state="BASELINE_NOT_PROVIDED", improvement_claim=False)
    if ctx.job.get("baseline_model"):
        ctx.report("RUNNING", "正在同一留出集评价基线模型")
        baseline = measure(ctx.job["baseline_model"], "baseline")
        deltas = {key: value-baseline["metrics"][key] for key, value in report["metrics"].items() if key in baseline["metrics"]}
        fp_delta = report["background_fp_per_frame"]-baseline["background_fp_per_frame"] if report["negative_frames"] else None
        comparison = dict(state="MEASURED_REVIEW_REQUIRED", metric_deltas=deltas, background_fp_per_frame_delta=fp_delta, improvement_claim=False, note="需按预先冻结的类别召回与误报准入标准人工复核；整体指标增量不自动代表提升")
    model_path = Path(ctx.job["model"]).resolve()
    contract = onnx_contract(model_path, len(manifest["class_names"]), imgsz) if model_path.suffix.lower() == ".onnx" else None
    report.update(model=str(model_path), dataset=str(dataset), class_names=manifest["class_names"], task="detect", input_size=imgsz, passed_compatibility=contract is not None, contract=contract, baseline=baseline, comparison=comparison)
    report = evaluation_json_value(report)
    write_json(output / "evaluation.json", report)
    return dict(output=str(output), evaluation=str(output / "evaluation.json"), model=str(model_path), model_sha256=report["model_sha256"], dataset=str(dataset), dataset_sha256=report["dataset_sha256"], class_names=manifest["class_names"], task="detect", input_size=imgsz, passed_compatibility=contract is not None, metrics=report["metrics"], comparison=comparison)


def inspect(ctx):
    result = dict(versions=versions())
    if ctx.job.get("model"):
        path = Path(ctx.job["model"]).resolve()
        if not path.is_file() or path.suffix.lower() != ".onnx":
            raise PipelineError("模型元数据检查要求本地 ONNX")
        try:
            import onnxruntime as ort
        except ImportError as exc:
            raise PipelineError("DEPENDENCY_MISSING：元数据检查需要 onnxruntime") from exc
        session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
        result.update(model=str(path), model_sha256=sha256(path), metadata=session.get_modelmeta().custom_metadata_map, inputs=[dict(name=i.name, shape=i.shape, type=i.type) for i in session.get_inputs()], outputs=[dict(name=i.name, shape=i.shape, type=i.type) for i in session.get_outputs()])
        encoded_names = result["metadata"].get("names")
        if encoded_names:
            try:
                try:
                    parsed = json.loads(encoded_names)
                except json.JSONDecodeError:
                    parsed = ast.literal_eval(encoded_names)
                if isinstance(parsed, dict):
                    if set(parsed) == set(range(len(parsed))):
                        parsed = [parsed[i] for i in range(len(parsed))]
                    elif set(parsed) == {str(i) for i in range(len(parsed))}:
                        parsed = [parsed[str(i)] for i in range(len(parsed))]
                    else:
                        raise PipelineError("模型 names 的类别 ID 不连续")
                result["model_class_names"] = class_schema(parsed)
            except (ValueError, SyntaxError, PipelineError, TypeError):
                result["model_class_names"] = []
                result["metadata_warning"] = "模型 names 无法完整安全解析；请核对原训练 data.yaml，不猜类别名称"
    if ctx.job.get("root"):
        root = Path(ctx.job["root"])
        if ctx.job.get("model") and not any(root.glob("*/session.json")):
            result.update(samples=0, sessions=0, states={})
        else:
            names, samples = load_samples(ctx)
            reviews = latest_reviews(ctx.job["root"], samples, names)
            result.update(samples=len(samples), sessions=len({key[0] for key in samples}), states=dict(Counter(reviews.get(key, {}).get("state", "UNKNOWN") for key in samples)), class_names=names)
    elif not ctx.job.get("model"):
        raise PipelineError("inspect 需要 root 或 model")
    return result


OPERATIONS = dict(review_curate=review_curate, inspect=inspect, review_export=review_export, import_labels=import_labels, export=export_dataset, prelabel=prelabel, train=train, evaluate=evaluate)


def execute(job, status=None, cancel=None):
    ctx = Context(job, status, cancel)
    try:
        operation = job.get("operation")
        if operation not in OPERATIONS:
            raise PipelineError("未知 operation：" + str(operation))
        ctx.report("RUNNING", "正在检查作业输入")
        ctx.check()
        result = OPERATIONS[operation](ctx)
        ctx.check()
        ctx.report("SUCCEEDED", "作业完成；训练候选不会自动激活", result)
        return 0
    except Cancelled as exc:
        ctx.report("CANCELLED", str(exc))
        return 2
    except Exception as exc:
        ctx.report("FAILED", f"{type(exc).__name__}: {exc}")
        print(f"作业失败：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job", required=True)
    parser.add_argument("--status", required=True)
    parser.add_argument("--cancel")
    args = parser.parse_args()
    try:
        job = read_json(args.job)
    except Exception as exc:
        write_json(args.status, dict(schema_version=SCHEMA, state="FAILED", message=f"作业 JSON 无法读取：{exc}", updated_at=now()))
        return 1
    return execute(job, args.status, args.cancel)


if __name__ == "__main__":
    raise SystemExit(main())
