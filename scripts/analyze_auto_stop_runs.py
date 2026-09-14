#!/usr/bin/env python3
"""比较已完成Run，并仅对反向保持做假设重放；不连接设备，不修改输入文件。"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def read(path):
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('输入报告超出4MiB预算')
    return json.loads(path.read_text(encoding='utf-8-sig'))


def describe(values):
    return dict(count=len(values), mean=statistics.mean(values),
                stddev=statistics.pstdev(values), minimum=min(values), maximum=max(values)) if values else dict(count=0)


def summarize(analysis):
    # 首发没有移动，排除后才比较移动后的表现。
    shots = analysis['shots'][1:]
    def ratios(key):
        return [m['speed_ratio'] for s in shots for m in
                ([s['down_model']] if key == 'down' else s['samples'][:1]) if m.get('valid')]
    return dict(moving_shots=len(shots), down_q=describe(ratios('down')),
                first_sample_q=describe(ratios('first')),
                cadence_ms=describe([s['previous_down_submit_interval_ms'] for s in shots]),
                stages_ms={name: describe([stage['observed_ms'] for s in shots for stage in s['stages']
                                          if stage['name'] == name])
                           for name in ['move_hold', 'counter_gap', 'counter_hold', 'shot_after_release']},
                feedback=analysis.get('feedback'))


def change_hold(report, hold):
    """保留本次ACK延迟形状，仅延长反向UP和此后时间；这是模型假设，不是预测调度。"""
    if report.get('success') is not True or report['plan']['baseline'] != 'counter':
        raise ValueError('只允许完整counter源Run')
    value = copy.deepcopy(report)
    delta = (hold - report['plan']['counter_hold_ms']) * 1_000_000
    shift = changed = previous_mask = 0
    opposite = 8 if report['plan']['direction'] == 2 else 2
    fields = ['planned_ns', 'submit_ns', 'ack_received_ns', 'backend_completed_ns', 'returned_ns']
    for command in value['commands']:
        if command['disposition'] != 2:
            raise ValueError('源Run含非确认命令，不适用固定延迟重放')
        if command['kind'] == 'wasd':
            if command['value'] == 0 and previous_mask == opposite:
                shift += delta
                changed += 1
            previous_mask = command['value']
        for field in fields:
            command[field] += shift
    if changed != report['plan']['shots'] - 1:
        raise ValueError('反向周期数不匹配，拒绝猜测')
    value['plan']['counter_hold_ms'] = hold
    # 只保留正式分析器必要字段，避免把真实monitor/cycles等旧证据套到假设时序。
    return dict(plan=value['plan'], commands=value['commands'], sampling_settings=value['sampling_settings'],
                success=None, failure='COUNTERFACTUAL_NOT_EXECUTED', monitor_evidence=None,
                counterfactual=True, assumption='RETAIN_OBSERVED_ACK_LATENCIES_SHIFT_COUNTER_UP_AND_LATER')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--test-result', type=Path, required=True)
    parser.add_argument('--formula-result', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--candidate-hold-ms', nargs='+', type=int, default=[10, 12, 15, 20])
    args = parser.parse_args()
    if not 1 <= len(args.candidate_hold_ms) <= 12 or any(not 1 <= n <= 200 for n in args.candidate_hold_ms):
        raise ValueError('候选数量1至12，保持1至200ms')
    args.output.mkdir(exist_ok=False)
    exe = str(args.executable.resolve())
    def evaluate(source, output):
        subprocess.run([exe, '--evaluate-result', str(source.resolve()), '--output', str(output.resolve())],
                       check=True, timeout=30, capture_output=True)
        return read(output / 'sampling-analysis.json')
    result = dict(physical_validation_passed=False, observations={}, counterfactuals={},
                  note='只比较给定模型；固定本次延迟形状的假设重放不保证下一次调度或游戏表现。')
    for name, source in [('test', args.test_result), ('formula', args.formula_result)]:
        analysis = evaluate(source, args.output / name)
        result['observations'][name] = dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                            plan=analysis['actual_plan'], summary=summarize(analysis))
    original = read(args.test_result)
    for hold in sorted(set(args.candidate_hold_ms)):
        candidate = change_hold(original, hold)
        source = args.output / f'counterfactual-hold-{hold}.json'
        source.write_text(json.dumps(candidate, ensure_ascii=False, indent=2), encoding='utf-8')
        analysis = evaluate(source, args.output / f'counterfactual-hold-{hold}')
        result['counterfactuals'][str(hold)] = summarize(analysis)
    (args.output / 'comparison.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: {name: item['summary'] if key == 'observations' else item
                           for name, item in result[key].items()} for key in ['observations', 'counterfactuals']},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()
