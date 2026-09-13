#!/usr/bin/env python3
"""有界离线弹着点几何评估；不控制设备，不证明速度为零。

shots JSON: {"shots": [{"start_ns": 1, "end_ns": 2}, ...]}。
窗口使用 frames.csv captured_at_ns 的同一时钟，须包含弹着点首次可见帧及持续帧。
输入必须先有一帧未开枪背景；所有阈值和颜色确认由操作者明确提供。
"""
import argparse
import csv
import json
import math
from pathlib import Path

import cv2
import numpy as np


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    timing = p.add_mutually_exclusive_group(required=True)
    timing.add_argument('--shots-json', type=Path)
    timing.add_argument('--report-json', type=Path)
    p.add_argument('--frame-time-field', choices=('captured_at_ns', 'source_time_at_ns'), default='captured_at_ns')
    p.add_argument('--command-frame-clock-confirmed', action='store_true')
    p.add_argument('--accept-command-windows', action='store_true')
    p.add_argument('--max-source-clock-uncertainty-ms', type=float)
    p.add_argument('--candidate-window-ms', type=float)
    p.add_argument('--expected', type=int, choices=(7, 8), required=True)
    p.add_argument('--roi', type=int, nargs=4, required=True, metavar=('X', 'Y', 'W', 'H'))
    p.add_argument('--hsv-low', type=int, nargs=3, required=True)
    p.add_argument('--hsv-high', type=int, nargs=3, required=True)
    p.add_argument('--color-confirmed', action='store_true')
    p.add_argument('--y-tolerance', type=float, required=True)
    p.add_argument('--slope-tolerance', type=float, required=True)
    p.add_argument('--min-x-span', type=float, required=True)
    p.add_argument('--association-radius', type=float, default=3)
    p.add_argument('--min-area', type=int, default=3)
    p.add_argument('--max-area', type=int, default=200)
    p.add_argument('--persistence', type=int, default=2)
    p.add_argument('--background-channel-tolerance', type=int, default=0)
    p.add_argument('--registration-roi', type=int, nargs=4)
    p.add_argument('--min-registration-texture', type=float)
    p.add_argument('--min-registration-response', type=float)
    p.add_argument('--max-registration-residual', type=float)
    p.add_argument('--max-translation', type=float)
    p.add_argument('--max-frames', type=int, default=1000)
    p.add_argument('--max-evidence', type=int, default=64)
    return p


def register_frame(reference, frame, o):
    """只从独立背景估计相对首帧纯平移；落点绝不参与变换估计。"""
    rx, ry, rw, rh = o.registration_roi
    gray_a = cv2.cvtColor(reference, cv2.COLOR_BGR2GRAY)
    gray_b = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    a = gray_a[ry:ry+rh, rx:rx+rw].astype(np.float64)
    b = gray_b[ry:ry+rh, rx:rx+rw].astype(np.float64)
    detail = {'translation_x': None, 'translation_y': None, 'response': None,
              'residual': None, 'texture': float(min(a.std(), b.std()))}
    if detail['texture'] < o.min_registration_texture:
        return None, detail, 'REGISTRATION_TEXTURE_INSUFFICIENT'
    window = cv2.createHanningWindow((rw, rh), cv2.CV_64F)
    shift, response = cv2.phaseCorrelate(a, b, window)
    dx, dy = shift
    if not all(math.isfinite(v) for v in (dx, dy, response)):
        return None, detail, 'REGISTRATION_NONFINITE'
    detail.update(translation_x=dx, translation_y=dy, response=response)
    if response < o.min_registration_response or math.hypot(dx, dy) > o.max_translation:
        return None, detail, 'REGISTRATION_RESPONSE_OR_TRANSLATION_INVALID'
    transform = np.array([[1., 0., -dx], [0., 1., -dy]])
    gray_aligned = cv2.warpAffine(gray_b, transform, (frame.shape[1], frame.shape[0]))
    residual = float(cv2.absdiff(gray_a, gray_aligned)[ry:ry+rh, rx:rx+rw].mean())
    detail['residual'] = residual
    if residual > o.max_registration_residual:
        return None, detail, 'REGISTRATION_RESIDUAL_INVALID'
    # 最近邻保留色标面积，亚像素位置仍保存配准变换；不伪造旋转/透视校正。
    aligned = cv2.warpAffine(frame, transform, (frame.shape[1], frame.shape[0]), flags=cv2.INTER_NEAREST)
    return aligned, detail, None


def analyze(o):
    finite = (o.y_tolerance, o.slope_tolerance, o.min_x_span, o.association_radius)
    if any(not math.isfinite(v) or v < 0 for v in finite) or o.min_x_span <= 0:
        raise ValueError('几何阈值必须有限非负，横向跨度必须为正')
    if not (2 <= o.persistence <= 100 and 1 <= o.max_frames <= 5000 and
            1 <= o.max_evidence <= 128 and 1 <= o.min_area <= o.max_area <= 10000 and
            0 <= o.background_channel_tolerance <= 255):
        raise ValueError('资源或检测参数越界')
    if any(not 0 <= low <= high <= maximum for low, high, maximum in
           zip(o.hsv_low, o.hsv_high, (179, 255, 255))):
        raise ValueError('HSV 阈值非法（OpenCV H 为 0..179）')
    x, y, width, height = o.roi
    if min(x, y) < 0 or min(width, height) <= 0 or width * height > 4000000:
        raise ValueError('ROI 非法或超过四百万像素')
    if o.registration_roi is not None:
        rx, ry, rw, rh = o.registration_roi
        limits = (o.min_registration_texture, o.min_registration_response,
                  o.max_registration_residual, o.max_translation)
        if any(v is None or not math.isfinite(v) for v in limits):
            raise ValueError('启用背景配准必须显式指定纹理、response、残差和平移四项质量门')
        if not (0 < o.min_registration_texture <= 128 and 0.1 <= o.min_registration_response <= 1 and
                0 <= o.max_registration_residual <= 32 and 0 < o.max_translation <= 64):
            raise ValueError('配准质量门越界')
        if (min(rx, ry) < 0 or min(rw, rh) < 32 or rw*rh > 4000000 or
                (rx < x+width and x < rx+rw and ry < y+height and y < ry+rh)):
            raise ValueError('配准 ROI 必须与测量 ROI 分离，且至少 32x32')
    root = o.input.resolve()
    candidate_windows = o.report_json is not None
    if candidate_windows:
        if not o.command_frame_clock_confirmed or not o.accept_command_windows:
            raise ValueError('命令与所选帧时刻必须明确确认同一单调时钟域；接收时刻不能代替来源曝光时刻')
        raw_report = json.loads(o.report_json.read_text(encoding='utf-8-sig'))
        if raw_report.get('success') is not True or raw_report.get('capture_complete') is not True:
            raise ValueError('执行或采集报告不完整，不能自动建立完整逐枪候选窗')
        candidate_ms = o.candidate_window_ms if o.candidate_window_ms is not None else raw_report['plan']['shot_interval_ms']
        if not math.isfinite(candidate_ms) or not 0 < candidate_ms <= 1000:
            raise ValueError('候选观察窗口必须在 0..1000ms 内，此值不是游戏弹点延迟测量')
        commands = raw_report['commands']
        if len(commands) > 128:
            raise ValueError('命令记录超过上限')
        times = [c['submit_ns'] for c in commands if c.get('kind') == 'left_button' and c.get('value') == 1]
        if any(type(t) is not int for t in times):
            raise ValueError('submit_ns 必须为整数')
        shots = [{'start_ns': t, 'end_ns': times[i+1]-1 if i+1 < len(times) else t+int(candidate_ms*1000000)}
                 for i, t in enumerate(times)]
    else:
        shots = json.loads(o.shots_json.read_text(encoding='utf-8-sig'))['shots']
    if len(shots) != o.expected:
        raise ValueError('试次窗口数量必须等于 expected')
    for i, shot in enumerate(shots):
        if (type(shot['start_ns']) is not int or type(shot['end_ns']) is not int or
                not 0 < shot['start_ns'] < shot['end_ns'] or
                (i and shots[i-1]['end_ns'] >= shot['start_ns'])):
            raise ValueError('试次窗口必须正序、不重叠，使用整数纳秒')
    rows = []
    with (root / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if len(rows) >= o.max_frames:
                raise ValueError('捕获记录超过上限，拒绝截断后判定')
            rows.append(row)
    o.output.mkdir(parents=True, exist_ok=False)
    issues = [] if o.color_confirmed else ['COLOR_UNCONFIRMED']
    tracks, first, previous_mask = [], None, None
    reference_frame = None
    registrations = []
    last_time, evidence_count = 0, 0
    seen_files = set()
    frame_shape = None
    for index, row in enumerate(rows):
        if row['status'] == 'NO_FRAME':
            continue
        if row['status'] != 'FRAME' or row.get('error'):
            issues.append('CAPTURE_INVALID')
            continue
        path = (root / row['png']).resolve()
        if not path.is_relative_to(root) or path.suffix.lower() != '.png':
            raise ValueError('PNG 路径越界或格式非法')
        if path in seen_files:
            raise ValueError('重复 PNG 不能作为独立持续帧')
        seen_files.add(path)
        frame_time_field = o.frame_time_field
        if candidate_windows:
            basis = row.get('time_basis')
            if basis == 'NDI':
                frame_time_field = 'source_time_at_ns'
                limit = o.max_source_clock_uncertainty_ms
                uncertainty = float(row.get('source_clock_uncertainty_ms', 'nan'))
                if limit is None or not math.isfinite(limit) or limit < 0 or not math.isfinite(uncertainty) or not 0 <= uncertainty <= limit:
                    raise ValueError('NDI 来源时钟不确定性缺失或超过显式许可范围')
            elif basis == 'DESKTOP_DUPLICATION':
                frame_time_field = 'captured_at_ns'
            else:
                raise ValueError('报告候选窗只支持已声明时钟的 DESKTOP_DUPLICATION 或 NDI')
        if frame_time_field == 'source_time_at_ns' and row.get('source_time_valid', row.get('source_time_timing_valid')) not in ('1', 'true', 'True'):
            issues.append('SOURCE_TIME_INVALID')
            continue
        time_ns = int(row[frame_time_field])
        if time_ns <= last_time:
            raise ValueError('帧时刻不严格递增')
        last_time = time_ns
        frame = cv2.imread(str(path))
        if frame is None or frame.size > 48000000:
            raise ValueError('图像不可读或过大')
        if frame_shape is not None and frame.shape != frame_shape:
            raise ValueError('图像尺寸发生变化')
        frame_shape = frame.shape
        if x + width > frame.shape[1] or y + height > frame.shape[0]:
            raise ValueError('ROI 超出图像')
        registration = {'translation_x': 0., 'translation_y': 0.}
        raw_frame = frame
        if o.registration_roi is not None:
            border = math.ceil(o.max_translation)+1
            for bx, by, bw, bh in (o.roi, o.registration_roi):
                if min(bx, by) < border or bx+bw > frame.shape[1]-border or by+bh > frame.shape[0]-border:
                    raise ValueError('测量/配准 ROI 必须留足最大平移边界，拒绝填充像素')
            if reference_frame is None:
                reference_frame = frame.copy()
            frame, registration, failure = register_frame(reference_frame, frame, o)
            registrations.append({'file': row['png'], 'captured_at_ns': time_ns,
                                  'status': failure or 'OK', **registration})
            if failure:
                issues.append(failure)
                continue
        roi = frame[y:y+height, x:x+width]
        mask = cv2.inRange(cv2.cvtColor(roi, cv2.COLOR_BGR2HSV),
                           np.array(o.hsv_low), np.array(o.hsv_high))
        if first is None:
            first = roi.copy()
            if time_ns >= shots[0]['start_ns'] or np.any(mask):
                issues.append('CLEAN_PRE_SHOT_BACKGROUND_MISSING')
        else:
            # 只排除当前及上一帧标记像素，不用点列拟合旋转校正相机。
            excluded = cv2.dilate(cv2.bitwise_or(mask, previous_mask), np.ones((3, 3), np.uint8)) > 0
            changed = np.max(cv2.absdiff(roi, first), axis=2) > o.background_channel_tolerance
            if np.any(changed & ~excluded):
                issues.append('BACKGROUND_CHANGED')
        count, labels, stats, centroids = cv2.connectedComponentsWithStats(mask)
        visible = set()
        new_tracks = []
        for label in range(1, count):
            area = int(stats[label, cv2.CC_STAT_AREA])
            if area < o.min_area or area > o.max_area:
                issues.append('COLOR_COMPONENT_AREA_INVALID')
                continue
            center = centroids[label]
            matches = [j for j, t in enumerate(tracks)
                       if np.linalg.norm(center - np.array(t['center'])) <= o.association_radius]
            if len(matches) > 1:
                issues.append('OVERLAP_OR_AMBIGUOUS_ASSOCIATION')
                continue
            if matches:
                j = matches[0]
                t = tracks[j]
                if j in visible or area != t['area']:
                    issues.append('OVERLAP_OR_CHANGED_MARK')
                if t['last_index'] != index - 1:
                    issues.append('MARK_DISAPPEARED_OR_REAPPEARED')
                t['consecutive'] = t['consecutive'] + 1 if t['last_index'] == index-1 else 1
                t['max_persistence'] = max(t['max_persistence'], t['consecutive'])
                if t['consecutive'] >= o.persistence and t['confirmed_ns'] is None:
                    t['confirmed_ns'] = time_ns
                t['last_index'] = index
            else:
                j = len(tracks)
                if j >= 64:
                    raise ValueError('颜色轨迹超过 64，停止分析')
                tracks.append({'center': center.tolist(), 'area': area, 'first_ns': time_ns,
                               'first_frame': row['png'], 'last_index': index,
                               'consecutive': 1, 'max_persistence': 1, 'confirmed_ns': None,
                               'first_transform': registration})
                new_tracks.append(j)
            visible.add(j)
        if len(new_tracks) > 1:
            issues.append('MULTIPLE_NEW_MARKS_ONE_FRAME')
        if new_tracks and evidence_count < o.max_evidence:
            prefix = o.output / f'evidence_{index:05d}'
            if not cv2.imwrite(str(prefix) + '_roi.png', roi) or not cv2.imwrite(str(prefix) + '_mask.png', mask):
                raise ValueError('证据图像写入失败')
            if o.registration_roi is not None and not cv2.imwrite(str(prefix) + '_original.png', raw_frame):
                raise ValueError('原坐标证据图像写入失败')
            evidence_count += 1
        previous_mask = mask
    if first is None or last_time < shots[-1]['end_ns']:
        issues.append('CAPTURE_COVERAGE_INCOMPLETE')
    points = []
    assigned = set()
    for shot_index, shot in enumerate(shots):
        candidates = [(j, t) for j, t in enumerate(tracks)
                      if shot['start_ns'] <= t['first_ns'] <= shot['end_ns']]
        status = 'OK'
        if len(candidates) != 1:
            status = 'MISSING_OR_OVERLAP' if not candidates else 'MULTIPLE_MARKS'
        elif (candidates[0][1]['confirmed_ns'] is None or
              candidates[0][1]['confirmed_ns'] > shot['end_ns']):
            status = 'INSUFFICIENT_PERSISTENCE'
        result = {'shot': shot_index+1, 'status': status, 'x': None, 'y': None}
        if status == 'OK':
            j, t = candidates[0]
            assigned.add(j)
            result.update(x=t['center'][0]+x, y=t['center'][1]+y,
                          first_ns=t['first_ns'], first_frame=t['first_frame'],
                          first_transform=t['first_transform'],
                          original_x=t['center'][0]+x+t['first_transform']['translation_x'],
                          original_y=t['center'][1]+y+t['first_transform']['translation_y'])
        else:
            issues.append(status)
        points.append(result)
    if len(assigned) != len(tracks):
        issues.append('UNASSIGNED_OR_INVALID_MARKS')
    metrics = None
    if all(p['status'] == 'OK' for p in points):
        xs, ys = np.array([p['x'] for p in points]), np.array([p['y'] for p in points])
        for point in points:
            point['dy_from_first'] = point['y'] - points[0]['y']
        span = float(np.ptp(xs))
        if span < o.min_x_span:
            issues.append('INSUFFICIENT_HORIZONTAL_SPAN')
        else:
            slope = float(np.dot(xs-xs.mean(), ys-ys.mean()) / np.dot(xs-xs.mean(), xs-xs.mean()))
            metrics = {'x_span': span, 'y_span': float(np.ptp(ys)), 'slope': slope,
                       'max_abs_dy_from_first': float(np.max(np.abs(ys-ys[0])))}
            if metrics['y_span'] > o.y_tolerance or abs(slope) > o.slope_tolerance:
                issues.append('GEOMETRY_OUTSIDE_TOLERANCE')
    report = {'schema': 1, 'status': 'INDETERMINATE' if issues else 'SCENE_GEOMETRY_PASS',
              'scope': '仅当前场景参考坐标弹着点几何，不证明速度为零或真实急停通过；配准仅支持已通过背景质量门的纯平移',
              'issues': sorted(set(issues)), 'points': points, 'metrics': metrics,
              'parameters': {k: str(v) if isinstance(v, Path) else v for k, v in vars(o).items()},
              'shots': shots, 'frame_rows': len(rows), 'evidence_count': evidence_count}
    report['registrations'] = registrations
    report['timing_window_basis'] = 'COMMAND_SUBMIT_CANDIDATE' if candidate_windows else 'USER_EXPLICIT_WINDOWS'
    report['timing_warning'] = '命令提交候选窗不证明游戏开枪、弹着点出现或命中时刻；不自动调整窗偏移' if candidate_windows else None
    report['coordinate_note'] = 'x/y 为首帧参考坐标；original_x/y 为使用保存变换反投影的原帧坐标，最近邻分割有亚像素量化误差'
    (o.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    try:
        report = analyze(parser().parse_args())
        print(json.dumps({'status': report['status'], 'issues': report['issues']}, ensure_ascii=False))
        raise SystemExit(0 if report['status'] == 'SCENE_GEOMETRY_PASS' else 2)
    except (ValueError, OSError, KeyError, TypeError) as error:
        raise SystemExit(f'分析失败：{error}')
