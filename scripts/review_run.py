#!/usr/bin/env python3
"""只读复用 Runtime/Aim 已有报告，生成单 Run 的可复现离线复盘。"""
import argparse
import hashlib
import html
import json
import math
from pathlib import Path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def distribution(values):
    values = sorted(v for v in values if finite(v) and v >= 0)
    def percentile(q):
        position = (len(values) - 1) * q
        low, high = math.floor(position), math.ceil(position)
        return values[low] + (values[high] - values[low]) * (position - low)
    return {'count': len(values), **({k: percentile(q) for k, q in [('p50', .5), ('p95', .95), ('p99', .99)]} if values else {}), 'maximum': max(values) if values else None}


def review(run):
    run = Path(run).resolve(strict=True)
    issues, inventory, documents, anchors = [], [], {}, []
    files = sorted(run.rglob('*'))
    if len(files) > 50000:
        raise ValueError('Run 超出 50000 项预算；请选择一个 Run')
    for path in files:
        name = path.relative_to(run).as_posix()
        if path.is_symlink() or not path.resolve().is_relative_to(run):
            issues.append({'file': name, 'reason': 'external_link_skipped'})
            continue
        if not path.is_file():
            continue
        inventory.append({'file': name, 'bytes': path.stat().st_size, 'sha256': digest(path)})
        if path.suffix.lower() == '.json':
            try:
                if path.stat().st_size > 128 * 1024 * 1024:
                    raise ValueError('JSON exceeds 128 MiB budget')
                documents[name] = json.loads(path.read_text(encoding='utf-8-sig'))
            except (ValueError, OSError) as error:
                issues.append({'file': name, 'reason': 'invalid_json', 'detail': str(error)})
    hashes = {item['file']: item['sha256'] for item in inventory}
    task = documents.get('task.json', {})
    if not isinstance(task, dict):
        task = {}
    config = {'file': 'config.ini', 'sha256': hashes.get('config.ini'), 'expected_sha256': (task.get('config') or {}).get('sha256') if isinstance(task.get('config'), dict) else None}
    config['status'] = 'unknown' if not config['sha256'] else 'unbound'
    if config['sha256'] and config['expected_sha256']:
        config['status'] = 'verified' if config['sha256'].lower() == str(config['expected_sha256']).lower() else 'mismatch'
    if config['status'] in ('unknown', 'mismatch'):
        issues.append({'file': 'config.ini', 'reason': 'config_' + config['status']})
    # 复用 capture_evidence 的 PNG 哈希合同；绝不按不同进程的 sequence 猜测关联。
    for name, document in documents.items():
        if not isinstance(document, dict) or not isinstance(document.get('frames'), list):
            continue
        frames = document['frames']
        if document.get('recorded_frame_count', len(frames)) != len(frames):
            issues.append({'file': name, 'reason': 'frame_count_mismatch'})
        if document.get('requested_frame_count', len(frames)) != len(frames):
            issues.append({'file': name, 'reason': 'capture_incomplete'})
        for frame in frames:
            if not isinstance(frame, dict) or not isinstance(frame.get('file'), str):
                issues.append({'file': name, 'reason': 'invalid_frame_entry'})
                continue
            path = (run / name).parent / frame['file']
            resolved = path.resolve()
            if not resolved.is_relative_to(run):
                issues.append({'file': name, 'reason': 'frame_path_outside_run'})
                continue
            relative = resolved.relative_to(run).as_posix()
            expected = frame.get('png_sha256')
            actual = hashes.get(relative)
            status = 'missing' if not actual else 'unverified' if not expected else 'verified' if actual.lower() == str(expected).lower() else 'hash_mismatch'
            anchor = {'file': relative, 'manifest': name, 'integrity': status, 'sequence': frame.get('sequence'), 'source_timecode': frame.get('source_timecode'), 'source_clock_session_id': frame.get('source_clock_session_id'), 'source_timecode_valid': frame.get('source_timecode_valid', False)}
            anchors.append(anchor)
            if status != 'verified':
                issues.append({'file': relative, 'reason': status})
    segments = []
    anchor_index = {}
    for anchor in anchors:
        if anchor['integrity'] == 'verified' and anchor['source_timecode_valid'] and anchor['source_clock_session_id']:
            key = (str(anchor['source_clock_session_id']), str(anchor['source_timecode']))
            anchor_index.setdefault(key, []).append(anchor['file'])
    for name, document in documents.items():
        rows = document.get('samples') if isinstance(document, dict) else None
        if not isinstance(rows, list):
            continue
        if any(not isinstance(row, dict) for row in rows):
            issues.append({'file': name, 'reason': 'invalid_sample_entry'})
            continue
        metadata = document if isinstance(document, dict) else {}
        if metadata.get('sample_count', len(rows)) != len(rows):
            issues.append({'file': name, 'reason': 'sample_count_mismatch'})
        omitted = metadata.get('report_samples_dropped')
        if omitted:
            issues.append({'file': name, 'reason': 'retained_window_only', 'omitted': omitted})
        events, performance = [], {}
        for metric in ('processing_ms', 'total_ms', 'capture_to_control_ms', 'source_to_control_ms'):
            values = [row.get(metric) for row in rows if (metric != 'source_to_control_ms' or row.get('source_timing_valid') is True) and (metric != 'capture_to_control_ms' or row.get('control_timing_valid') is True)]
            performance[metric] = distribution(values)
        stage_metrics = sorted({key for row in rows if isinstance(row.get('performance'), dict) for key in row['performance'] if key.endswith('_ms')})
        for metric in stage_metrics:
            performance['stage.' + metric] = distribution([row.get('performance', {}).get(metric) for row in rows if isinstance(row.get('performance'), dict)])
        # ns 字符串保留整数精度。只有本段明确有效的接收时间用于区间，不推断录像时间。
        def time_ns(row):
            value = row.get('capture_steady_ns')
            if row.get('capture_steady_ns_valid') is not True:
                return None
            try:
                return int(value)
            except (ValueError, TypeError):
                return None
        origin = next((time_ns(row) for row in rows if time_ns(row) is not None), None)
        for index, row in enumerate(rows):
            reasons = []
            if row.get('success') is False:
                reasons.append('sample_failed')
            if row.get('source_timing_valid') is False:
                reasons.append('source_clock_unknown')
            if row.get('control_timing_valid') is False:
                reasons.append('control_timing_unknown')
            if not reasons:
                continue
            matching = []
            if row.get('source_timecode_valid') and row.get('source_clock_session_id'):
                matching = anchor_index.get((str(row['source_clock_session_id']), str(row.get('source_timecode'))), [])
            relative_time = (time_ns(row) - origin) / 1e6 if origin is not None and time_ns(row) is not None else None
            event = {'first_index': index, 'last_index': index, 'first_sequence': row.get('sequence'), 'last_sequence': row.get('sequence'), 'start_ms': relative_time, 'end_ms': relative_time, 'reasons': reasons, 'anchors': matching, 'alignment': 'source_timecode_same_session' if matching else 'unknown'}
            if events and events[-1]['last_index'] == index - 1 and events[-1]['reasons'] == reasons:
                previous = events[-1]
                previous.update(last_index=index, last_sequence=row.get('sequence'), end_ms=relative_time)
                previous['anchors'] = sorted(set(previous['anchors'] + matching))
                if not matching:
                    previous['alignment'] = 'unknown'
            else:
                events.append(event)
        segments.append({'file': name, 'session_id': metadata.get('session_id'), 'sample_count': len(rows), 'omitted_samples': omitted, 'time_basis': 'segment_relative_receiver_steady_clock' if origin is not None else 'unknown', 'performance_ms': performance, 'anomaly_intervals': events})
    if not segments:
        issues.append({'file': '.', 'reason': 'no_supported_samples'})
    summaries = [name for name in documents if name.endswith(('summary.json', 'sampling-analysis.json'))]
    if not anchors:
        issues.append({'file': '.', 'reason': 'no_capture_manifest_anchors'})
    identity = hashlib.sha256(json.dumps(inventory, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'schema': 1, 'run_id': task.get('run_id', run.name), 'input_identity_sha256': identity, 'reuse': {'existing_reports': summaries, 'policy': '只读复用已归档 Runtime/Aim 报告，不重新采集或改写证据。'}, 'integrity': {'status': 'partial_or_unknown' if issues else 'checked', 'issues': issues}, 'configuration': config, 'package_commit': task.get('package_commit'), 'segments': segments, 'anchors': anchors, 'inventory': inventory, 'limits': ['离线报告，不代表真实设备、游戏效果或辅机性能验收。', '无显式同源时钟身份的截图关联为 unknown；不按文件时间、序号或采样率伪造逐帧真值。', '性能仅统计有记录且有效的值；缺失计量不按零计入。', '异常区间是留存样本的连续索引范围，不证明中间未留样画面连续。']}


def write_review(run, output):
    run, output = Path(run).resolve(strict=True), Path(output).resolve()
    if output == run or output.is_relative_to(run) or run.is_relative_to(output):
        raise ValueError('输出必须位于原 Run 之外，不能覆盖或包含归档')
    if output.exists():
        raise ValueError('输出目录已存在，请指定新的目录')
    result = review(run)
    output.mkdir(parents=True, exist_ok=False)
    (output / 'review.json').write_text(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    lines = ['# 单 Run 离线复盘', '', f"Run：{result['run_id']}", '', f"输入身份：{result['input_identity_sha256']}", '', f"完整性：{result['integrity']['status']}；配置：{result['configuration']['status']}", '', *result['limits'], '', '## 完整性问题', '', '```json', json.dumps(result['integrity']['issues'], ensure_ascii=False, indent=2), '```', '', '## 性能与异常区间', '']
    for segment in result['segments']:
        lines += [f"### {segment['file']}", '', '```json', json.dumps(segment, ensure_ascii=False, indent=2), '```', '']
    lines += ['## 已有报告与画面锚点', '']
    for name in result['reuse']['existing_reports'] + [a['file'] for a in result['anchors']]:
        lines.append(f'- [{name}]({(run / name).as_uri()})')
    (output / 'REVIEW.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    links = ''.join(f'<li><a href="{html.escape((run / a["file"]).as_uri(), quote=True)}">{html.escape(a["file"])}</a> — {a["integrity"]}</li>' for a in result['anchors'])
    (output / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Run 离线复盘</title><style>body{max-width:1100px;margin:2rem auto;font:16px system-ui}pre{white-space:pre-wrap;overflow-wrap:anywhere}a{color:#1467ad}</style><h1>Run 离线复盘</h1><p>原始画面只读链接；完整机器可读数据见 review.json。</p><ul>' + links + '</ul><pre>' + html.escape('\n'.join(lines)) + '</pre>', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = write_review(args.run, args.output)
    except (ValueError, OSError) as error:
        parser.exit(2, f'{error}\n')
    print(json.dumps({'output': str(args.output), 'integrity': result['integrity']['status'], 'segments': len(result['segments']), 'anchors': len(result['anchors'])}))


if __name__ == '__main__':
    main()
