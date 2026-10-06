#!/usr/bin/env python3
"""只打包已有本地量测证据；不采集、不操作设备、不替人工设置 origin/reviewed。"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys

MAX_IMAGE_BYTES = 32 * 1024 * 1024

def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()

def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)

def prepare(manifest):
    manifest = Path(manifest).resolve(strict=True)
    if manifest.stat().st_size > 1024 * 1024:
        raise ValueError("manifest 超过 1 MiB")
    with manifest.open(encoding="utf-8-sig") as source:
        data = json.load(source)
    if not isinstance(data, dict) or data.get("schema", 1) != 1:
        raise ValueError("manifest schema 必须为 1")
    data = copy.deepcopy(data)
    data.setdefault("schema", 1)
    data.setdefault("origin", "simulation")
    data.setdefault("reviewed", False)
    if data["origin"] not in ("measured", "simulation") or not isinstance(data["reviewed"], bool):
        raise ValueError("origin/reviewed 必须明确合法；工具不会替换人工决定")
    for field in ("id", "source_id", "backend", "context_identity"):
        if not isinstance(data.get(field), str) or not data[field] or len(data[field]) > 256:
            raise ValueError("缺少或非法身份字段: " + field)
    geometry = data.get("geometry")
    fields = ("width", "height", "source_width", "source_height", "encoded_width", "encoded_height", "roi_x", "roi_y", "scale_x", "scale_y")
    if not isinstance(geometry, dict) or any(not finite_number(geometry.get(key)) for key in fields) or not isinstance(geometry.get("mapping_verified"), bool):
        raise ValueError("geometry 必须由原量测 manifest 完整提供")
    if not finite_number(data.get("observation_delay_ms")):
        raise ValueError("缺少显式 observation_delay_ms；不猜延迟")
    samples = data.get("samples")
    if not isinstance(samples, list) or not 8 <= len(samples) <= 128:
        raise ValueError("samples 必须有 8..128 条已有量测")
    files = []
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict) or sample.get("axis") not in ("x", "y") or any(not finite_number(sample.get(key)) for key in ("counts", "delta_error_pixels", "cross_error_pixels", "observed_delay_ms")):
            raise ValueError("非法量测记录: " + str(index))
        for prefix in ("before", "after"):
            name = sample.get(prefix + "_image")
            if not isinstance(name, str) or not name or name.startswith(("\\\\", "//")) or "://" in name:
                raise ValueError("证据必须是本地普通文件")
            original = (manifest.parent / name).resolve(strict=True)
            if not original.is_file() or original.stat().st_size > MAX_IMAGE_BYTES:
                raise ValueError("证据不是普通文件或超过 32 MiB")
            actual_hash = digest(original)
            supplied = sample.get(prefix + "_sha256")
            if supplied is not None and supplied != actual_hash:
                raise ValueError("原始证据 SHA-256 不匹配: " + name)
            # 固定输出编号避免原路径穿越及同名覆盖；内容逐字节复制，不解码图片。
            extension = original.suffix.lower() if original.suffix.lower() in (".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tiff", ".tif") else ".bin"
            relative = f"evidence/{index:03d}-{prefix}{extension}"
            sample[prefix + "_image"] = relative
            sample[prefix + "_sha256"] = actual_hash
            files.append((original, relative, actual_hash))
    return data, files

def build_bundle(manifest, output=None, check_only=False):
    data, files = prepare(manifest)
    summary = {"origin": data["origin"], "reviewed": data["reviewed"], "samples": len(data["samples"]),
               "evidence_sha256_checked": len(files), "production_validated": False,
               "note": "仅打包/核对原始证据；生产有效性由 Runtime reader 与实际上下文判断"}
    if check_only:
        return summary
    if output is None:
        raise ValueError("非 check-only 模式必须提供 --output")
    output = Path(output).resolve()
    # 独占创建；已有目录即使为空也拒绝。不改写 manifest 或任何输入证据。
    output.mkdir(parents=False, exist_ok=False)
    (output / "evidence").mkdir()
    for original, relative, expected in files:
        destination = output / relative
        with original.open("rb") as source, destination.open("xb") as target:
            shutil.copyfileobj(source, target, length=1024 * 1024)
        if digest(destination) != expected or digest(original) != expected:
            raise ValueError("复制期间证据发生变化；目录保留为未完成，不发布 calibration.json")
    # 清单最后发布；失败时留下可查的未完成新目录，不递归删除任何目录。
    with (output / "calibration.json").open("x", encoding="utf-8", newline="\n") as target:
        json.dump(data, target, ensure_ascii=False, indent=2, allow_nan=False)
        target.write("\n")
    summary["bundle"] = str(output / "calibration.json")
    return summary

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(build_bundle(args.manifest, args.output, args.check_only), ensure_ascii=False, indent=2))
    except (OSError, ValueError, TypeError) as error:
        print("校准证据未打包: " + str(error), file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
