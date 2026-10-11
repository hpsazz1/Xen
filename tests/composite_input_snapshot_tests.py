"""Production entries must bind the bytes they parsed, despite later path replacement."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import pathlib
import sys
import tempfile
from unittest.mock import patch


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seal-executable', type=pathlib.Path, required=True)
    parser.add_argument('--source-root', type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument('--output-root', type=pathlib.Path)
    args = parser.parse_args()
    source = args.source_root.resolve()
    sys.path.insert(0, str(source / 'tests'))
    from composite_report_validation_tests import load, downstream

    producer_path = source / 'scripts/produce_mouse_effect_probe_b_composite_phase_ledgers.py'
    binder_path = source / 'scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py'
    evaluator_path = source / 'scripts/evaluate_mouse_effect_probe_b_composite_phase.py'
    fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_producer_tests.py', 'snapshot_fixture')
    binder_fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_binder_tests.py', 'snapshot_binder_fixture')
    producer = load(producer_path, 'snapshot_producer')
    binder = load(binder_path, 'snapshot_binder')
    verifier = args.seal_executable.resolve()
    if args.output_root:
        args.output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='xen-frozen-input-', dir=args.output_root) as temporary:
        root = pathlib.Path(temporary)
        paths = fixture.build_raw(root, binder_fixture, binder_path, evaluator_path, producer_path, verifier)
        paths.update(binder=binder_path, evaluator=evaluator_path)
        control, capture, commands = fixture.invoke(producer_path, paths, root, 'control')
        fixture.expect(control.returncode == 0, 'control producer: ' + control.stderr)
        downstream(paths, capture, commands, root, 'control')
        expected_capture = json.loads(capture.read_bytes())
        expected_commands = json.loads(commands.read_bytes())
        expected_evidence = json.loads((root / 'control-evidence.json').read_bytes())
        results = []

        def replaced_entry(module, arguments, target):
            # TEMP 可使用 ADMINI~1；生产入口 resolve 后必须仍命中同一测试文件。
            target = target.resolve()
            original_bytes = target.read_bytes()
            replacement = b'{"replaced_after_original_read":true}\n'
            assert original_bytes != replacement
            calls = 0
            real_bytes = pathlib.Path.read_bytes
            real_text = pathlib.Path.read_text

            def after_read(path, value):
                nonlocal calls
                if path == target:
                    calls += 1
                    if calls == 1:
                        target.write_bytes(replacement)
                return value

            def read_bytes(path):
                return after_read(path, real_bytes(path))

            def read_text(path, *a, **kw):
                return after_read(path, real_text(path, *a, **kw))

            captured = io.StringIO()
            try:
                with patch.object(pathlib.Path, 'read_bytes', read_bytes), \
                     patch.object(pathlib.Path, 'read_text', read_text), \
                     patch.object(sys, 'argv', arguments), \
                     contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
                    code = module.main()
                fixture.expect(calls == 1 and target.read_bytes() == replacement,
                               'replacement barrier must run against the actual input')
                return code, captured.getvalue(), hashlib.sha256(original_bytes).hexdigest(), calls
            finally:
                target.write_bytes(original_bytes)

        for key in ('schedule', 'plan', 'safety'):
            out_capture = root / f'{key}-capture.json'
            out_commands = root / f'{key}-commands.json'
            arguments = ['producer', '--plan', str(paths['plan']), '--report', str(paths['report']),
                         '--schedule', str(paths['schedule']), '--safety', str(paths['safety']),
                         '--manifest', str(paths['manifest']), '--human-assessment', str(paths['assessment']),
                         '--report-verifier', str(verifier), '--capture-output', str(out_capture),
                         '--command-output', str(out_commands)]
            code, detail, expected_hash, reads = replaced_entry(producer, arguments, paths[key])
            published = out_capture.exists() and out_commands.exists()
            matched = (published and json.loads(out_capture.read_bytes()) == expected_capture and
                       json.loads(out_commands.read_bytes()) == expected_commands)
            results.append(dict(entry='producer', input=key, exit=code, published=published,
                                matched_frozen_control=matched, expected_file_sha256=expected_hash,
                                path_reads=reads, detail=detail))

        for key, target in [('plan', paths['plan']), ('capture', capture), ('command', commands)]:
            output = root / f'binder-{key}.json'
            arguments = ['binder', '--plan', str(paths['plan']), '--capture', str(capture),
                         '--commands', str(commands), '--output', str(output)]
            code, detail, expected_hash, reads = replaced_entry(binder, arguments, target)
            published = output.exists()
            matched = published and json.loads(output.read_bytes()) == expected_evidence
            actual_hash = (json.loads(output.read_bytes())['input_bindings'][key + '_file_sha256']
                           if published else None)
            results.append(dict(entry='binder', input=key, exit=code, published=published,
                                matched_frozen_control=matched, expected_file_sha256=expected_hash,
                                actual_file_sha256=actual_hash, path_reads=reads, detail=detail))

        for row in results:
            print(json.dumps(row, ensure_ascii=False))
        failures = [r for r in results if r['exit'] != 0 or not r['matched_frozen_control']]
        print(f'input snapshot cases: {len(results) - len(failures)}/{len(results)} passed')
        return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
