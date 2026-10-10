"""真实无输出CLI→producer→binder→evaluator，非法语义及路径替换回归。"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile


def load(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def downstream(paths, capture, commands, root, stem):
    evidence = root / f"{stem}-evidence.json"
    bound = subprocess.run([sys.executable, str(paths['binder']), '--plan', str(paths['plan']),
        '--capture', str(capture), '--commands', str(commands), '--output', str(evidence)],
        capture_output=True, text=True, encoding='utf-8')
    if bound.returncode:
        raise AssertionError('真实binder拒绝控制组: ' + bound.stderr)
    output = root / f"{stem}-evaluation.json"
    evaluated = subprocess.run([sys.executable, str(paths['evaluator']), '--evidence', str(evidence),
        '--output', str(output)], capture_output=True, text=True, encoding='utf-8')
    if evaluated.returncode or json.loads(output.read_text(encoding='utf-8'))['status'] != 'READY_FOR_SEALED_PHASE_VALIDATION':
        raise AssertionError('真实evaluator未通过: ' + evaluated.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seal-executable', type=pathlib.Path, required=True)
    parser.add_argument('--case', choices=('semantics', 'frozen-report'), required=True)
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parents[1]
    fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_producer_tests.py', 'report_chain_fixture')
    binder_fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_binder_tests.py', 'report_binder_fixture')
    producer = source / 'scripts/produce_mouse_effect_probe_b_composite_phase_ledgers.py'
    binder = source / 'scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py'
    evaluator = source / 'scripts/evaluate_mouse_effect_probe_b_composite_phase.py'
    verifier = args.seal_executable.resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix='xen-report-chain-') as temporary:
        root = pathlib.Path(temporary)
        paths = fixture.build_raw(root, binder_fixture, binder, evaluator, producer, verifier)
        paths.update(binder=binder, evaluator=evaluator)
        valid_bytes = paths['report'].read_bytes()
        valid = json.loads(valid_bytes)
        control, capture, commands = fixture.invoke(producer, paths, root, 'valid')
        fixture.expect(control.returncode == 0, '合法B真实producer失败: ' + control.stderr)
        downstream(paths, capture, commands, root, 'valid')
        print('control B: real verifier/producer/binder/evaluator accepted')
        if args.case == 'semantics':
            for case in ('missing-safety', 'false-safety', 'zero-false-safety', 'ack-before-issue', 'zero-extra-dispatch', 'nonconservation'):
                report = copy.deepcopy(valid)
                event = next(item for item in report['result']['events'] if item['dispatch_attempted'])
                if case == 'missing-safety':
                    event.pop('safety_allowed')
                elif case == 'false-safety':
                    event['safety_allowed'] = False
                elif case == 'zero-false-safety':
                    report['result']['events'][2]['safety_allowed'] = False
                elif case == 'ack-before-issue':
                    event['protocol_ack_received_at_steady_ns'] = event['issued_at_steady_ns'] - 1
                elif case == 'zero-extra-dispatch':
                    event = report['result']['events'][2]
                    fixture.expect(event['nominal_dx_counts'] == 0, '必须变异真实零response')
                    issue = event['scheduled_at_steady_ns']
                    event.update(dispatch_attempted=True, backend_succeeded=True, protocol_ack_received=True,
                        issued_at_steady_ns=issue, protocol_ack_received_at_steady_ns=issue + 1,
                        backend_completed_at_steady_ns=issue + 2, returned_at_steady_ns=issue + 3)
                else:
                    event['cumulative_requested_x_counts'] += 1
                fixture.seal_report_native(report, paths['report'], verifier)
                fixture.update_schedule_for_files(paths)
                native = subprocess.run([str(verifier), '--verify-report', str(paths['report'])],
                    capture_output=True, text=True, encoding='utf-8')
                completed, bad_capture, bad_commands = fixture.invoke(producer, paths, root, case)
                accepted = completed.returncode == 0 and bad_capture.is_file() and bad_commands.is_file()
                if accepted:
                    downstream(paths, bad_capture, bad_commands, root, case)
                print(f'{case}: native={native.returncode} producer={completed.returncode} published={accepted}')
                if native.returncode != 2 or completed.returncode != 2 or bad_capture.exists() or bad_commands.exists():
                    failures.append(case)
        else:
            # A只改响应RTT，保留B语义摘要；调度账本仍绑定B。先证明静态A会被真实CLI拒绝。
            invalid = copy.deepcopy(valid)
            invalid['result']['events'][2]['source_clock_rtt_ms'] = .005
            fixture.write_json(paths['report'], invalid)
            native = subprocess.run([str(verifier), '--verify-report', str(paths['report'])],
                capture_output=True, text=True, encoding='utf-8')
            fixture.expect(native.returncode == 2, '非法A控制组必须拒绝')
            module = load(producer, 'report_chain_producer')
            real_run = subprocess.run
            swapped = 0
            def run(arguments, **kwargs):
                nonlocal swapped
                if '--verify-report' in arguments:
                    swapped += 1
                    paths['report'].write_bytes(valid_bytes)
                return real_run(arguments, **kwargs)
            capture = root / 'race-capture.json'
            commands = root / 'race-commands.json'
            arguments = ['producer', '--plan', str(paths['plan']), '--report', str(paths['report']),
                '--schedule', str(paths['schedule']), '--safety', str(paths['safety']),
                '--manifest', str(paths['manifest']), '--human-assessment', str(paths['assessment']),
                '--report-verifier', str(verifier), '--capture-output', str(capture), '--command-output', str(commands)]
            previous = sys.argv
            try:
                subprocess.run = run
                sys.argv = arguments
                result = module.main()
            finally:
                subprocess.run = real_run
                sys.argv = previous
            fixture.expect(swapped == 1, '屏障必须穿过真实原生校验进程调用')
            if result == 0:
                downstream(paths, capture, commands, root, 'race')
                payload = json.loads(capture.read_text(encoding='utf-8'))
                mapping = payload['clock_mapping']
                print('race accepted: round_trip_max_ns=', mapping['source_clock_round_trip_max_ns'],
                      'references B=', mapping['source_timing_evidence_sha256'] == fixture.file_sha256(paths['report']))
            print(f'parse-A/verify-B: exit={result} published={capture.exists() or commands.exists()}')
            if result != 2 or capture.exists() or commands.exists():
                failures.append('parse-A/verify-B')
    if failures:
        print('FAILED: ' + ', '.join(failures), file=sys.stderr)
        return 1
    print('all real report chain regressions passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
