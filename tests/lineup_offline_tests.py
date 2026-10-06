"""合成图的真实 CLI 回归；不用于真实场景视觉精度验收。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import cv2
import numpy as np


def roi_regression(executable, workspace, template):
    """独立生成 320 ROI；新帧几何是夹具明示证据，不从参考推定。"""
    image = np.random.default_rng(20261005).integers(0, 256, (320, 320, 3), dtype=np.uint8)
    geometry = dict(width=320, height=320, source_width=1920, source_height=1080,
                    encoded_width=320, encoded_height=320, roi_x=800, roi_y=380,
                    scale_x=1., scale_y=1.)
    failures, results = [], {}
    for known in (True, False):
        root = workspace / ('roi-known' if known else 'roi-unknown')
        reference, frames, output = root / 'reference', root / 'frames', root / 'report'
        reference.mkdir(parents=True, exist_ok=False)
        frames.mkdir()
        declared = copy.deepcopy(geometry)
        if not known:
            declared.update(source_width=0, source_height=0, roi_x=0, roi_y=0)
        metadata = copy.deepcopy(template)
        metadata.update(schema_version=2, frame_mode='roi', source_mapping_verified=known,
                        id='synthetic-roi-known' if known else 'synthetic-roi-unknown',
                        session_id='synthetic-roi-independent-fixture', sequence=1,
                        geometry=declared, aim=[160., 160.], normalized_aim=[.5, .5])
        (reference / 'reference.json').write_text(json.dumps(metadata), encoding='utf-8')
        for name, value in [('raw.png', image), ('mask.png', np.full((320, 320), 255, np.uint8))]:
            ok, encoded = cv2.imencode('.png', value)
            assert ok
            encoded.tofile(reference / name)
        base = dict(geometry=copy.deepcopy(declared), source_mapping_verified=known,
                    aim=[160., 160.], negative=False)
        entries = {'00-identity.png': copy.deepcopy(base), '01-missing-geometry.png': {'source_mapping_verified': known},
                   '02-wrong-origin.png': copy.deepcopy(base), '03-missing-mapping.png': {'geometry': copy.deepcopy(declared)},
                   '04-wrong-dimensions.png': copy.deepcopy(base)}
        entries['02-wrong-origin.png']['geometry']['roi_x'] += 5
        entries['04-wrong-dimensions.png']['geometry']['width'] = 319
        for name in entries:
            ok, encoded = cv2.imencode('.png', image)
            assert ok
            encoded.tofile(frames / name)
        (frames / 'manifest.json').write_text(json.dumps({'dataset_kind': 'synthetic', 'frames': entries}), encoding='utf-8')
        completed = subprocess.run([str(executable), '--reference', str(reference), '--frames', str(frames), '--output', str(output)], capture_output=True)
        if completed.returncode:
            failures.append(f'ROI {known} CLI: {completed.stderr.decode("utf-8", errors="replace")}')
            continue
        rows = json.loads((output / 'observations.json').read_text(encoding='utf-8'))
        results[str(known)] = rows
        by_name = {row['file']: row for row in rows}
        identity = by_name['00-identity.png']
        if identity['status'] != 'VALID' or identity['independent_error_px'] is None or identity['independent_error_px'] > .1:
            failures.append(f'ROI {known} identity: {identity}')
        if identity['coordinate_space'] != 'local_image_pixels':
            failures.append('ROI 坐标空间未明确为局部 image pixels')
        if known and (identity['source_aim'] is None or np.linalg.norm(np.array(identity['source_aim']) - [960, 540]) > .1):
            failures.append('ROI 已验证映射的源坐标错误')
        if not known and (identity['source_aim'] is not None or identity['source_mapping_verified']):
            failures.append('ROI 未知源映射被错误升级为源坐标')
        for name, reason in [('01-missing-geometry.png', 'offline_roi_geometry_unknown'),
                             ('02-wrong-origin.png', 'geometry_changed_recapture_required'),
                             ('03-missing-mapping.png', 'offline_roi_mapping_declaration_unknown'),
                             ('04-wrong-dimensions.png', 'invalid_roi_geometry')]:
            row = by_name[name]
            if row['status'] != 'INVALID_FRAME' or row['reason'] != reason or row['aim'] is not None:
                failures.append(f'ROI {known} {name}: {row}')
    return failures, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    metadata = json.loads((args.reference / "reference.json").read_text(encoding="utf-8-sig"))
    if "synthetic" not in metadata["session_id"].lower():
        raise ValueError("此夹具生成器只接受明确标为 synthetic 的参考")
    image = cv2.imdecode(np.fromfile(args.reference / "raw.png", dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError("参考图读取失败")
    fixture = args.workspace / "offline-fixture"
    report = args.workspace / "offline-report"
    fixture.mkdir(parents=True, exist_ok=False)
    if report.exists():
        raise FileExistsError(report)
    height, width = image.shape[:2]
    aim = np.array([*metadata["aim"], 1.0], dtype=np.float64)
    manifest = {"dataset_kind": "synthetic", "provenance": "浏览器 synthetic-test-fixture 原图；坐标真值由独立已知变换计算，不来自定位输出", "frames": {}}

    def save(name, sample, **truth):
        ok, encoded = cv2.imencode(".png", sample)
        if not ok:
            raise ValueError("夹具编码失败")
        encoded.tofile(fixture / name)
        if metadata.get('schema_version') == 2:
            truth.update(geometry=copy.deepcopy(metadata['geometry']),
                         source_mapping_verified=metadata['source_mapping_verified'])
        manifest["frames"][name] = truth

    save("00-identity.png", image, aim=aim[:2].tolist(), negative=False)
    for index, (dx, dy) in enumerate([(12, -7), (-18, 9), (25, 15)], 1):
        transform = np.array([[1., 0., dx], [0., 1., dy]], dtype=np.float64)
        sample = cv2.warpAffine(image, transform, (width, height))
        save(f"{index:02}-translation.png", sample, aim=(transform @ aim).tolist(), negative=False, transform=transform.tolist())
    for index, angle in enumerate([-3.0, 2.0, 4.0], 4):
        transform = cv2.getRotationMatrix2D((width / 2, height / 2), angle, 1.)
        sample = cv2.warpAffine(image, transform, (width, height))
        save(f"{index:02}-rotation.png", sample, aim=(transform @ aim).tolist(), negative=False, transform=transform.tolist())
    save("10-negative-blank.png", np.full_like(image, 80), negative=True)
    occluded = image.copy()
    x, y = [int(v) for v in aim[:2]]
    occluded[max(0, y-60):min(height, y+61), max(0, x-60):min(width, x+61)] = 0
    save("11-negative-aim-occluded.png", occluded, negative=True)
    yy, xx = np.indices((height, width))
    checker = (((xx // 24 + yy // 24) % 2) * 255).astype(np.uint8)
    save("12-negative-repeated.png", cv2.cvtColor(checker, cv2.COLOR_GRAY2BGR), negative=True)
    (fixture / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    completed = subprocess.run([str(args.exe), "--reference", str(args.reference), "--frames", str(fixture), "--output", str(report)], capture_output=True)
    (args.workspace / "offline-cli.stdout.txt").write_bytes(completed.stdout)
    (args.workspace / "offline-cli.stderr.txt").write_bytes(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(f"CLI 返回 {completed.returncode}: {completed.stderr.decode('utf-8', errors='replace')}")
    summary = json.loads((report / "summary.json").read_text(encoding="utf-8"))
    rows = json.loads((report / "observations.json").read_text(encoding="utf-8"))
    failures = []
    for row in rows:
        truth = manifest["frames"][row["file"]]
        if truth["negative"] and row["status"] == "VALID":
            failures.append(f"负例误报: {row['file']}")
        if not truth["negative"] and row["status"] != "VALID":
            failures.append(f"合成正例拒识: {row['file']} / {row['reason']}")
        if not truth["negative"] and row["independent_error_px"] is not None and row["independent_error_px"] > 2.0:
            failures.append(f"合成坐标偏差>2px: {row['file']} / {row['independent_error_px']}")
    roi_failures, roi_results = roi_regression(args.exe, args.workspace, metadata)
    failures.extend(roi_failures)
    result = {"scope": "synthetic_program_behavior_only", "passed": not failures, "failures": failures, "summary": summary, "roi_cases": roi_results}
    (args.workspace / "offline-regression-result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
