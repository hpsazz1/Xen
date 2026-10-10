"""使用真实禁输出 CLI 验证 completion 时间戳的 int64 边界。"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import uuid
from aim_production_red_producer_integration_tests import make_plan


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ('completion-overflow-' + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    config, measured = root / 'config.ini', root / 'measured.txt'
    config.write_text('[aim]\nperson_class_ids=0,2\nhead_class_ids=1,3\n', encoding='utf-8')
    measured.write_text('合成边界夹具，无真实测量验收\n', encoding='utf-8')
    maximum = (1 << 63) - 1
    cases = [
        ('normal', 2_003_000_000, 1, False, True),
        ('maximum-valid', maximum - 100_000, 1, False, True),
        ('one-over', maximum - 99_999, 1, False, False),
        ('half-delay-over', maximum - 50_000, 1, False, False),
        ('maximum', maximum, 1, False, False),
        ('second-row-over', maximum - 99_999, 2, False, False),
        ('visible-over', maximum - 99_999, 1, True, False),
        ('second-row-valid', maximum - 100_000, 2, False, True),
    ]
    results = []
    for name, last_control, count, visible, accepted in cases:
        plan = copy.deepcopy(make_plan())
        block = plan['blocks'][0]
        block['samples'] = block['samples'][:count]
        block.update(score_begin=0, score_end=count)
        for index, sample in enumerate(block['samples']):
            control = last_control - (count - index - 1) * 4_166_700
            sample.update(control_at_ns=control, captured_at_ns=control - 3_000_000,
                          controller_dt_ns=4_166_700, visible=visible)
        directory = root / name
        directory.mkdir()
        path, bundle = directory / 'plan.json', directory / 'bundle'
        path.write_text(json.dumps(plan), encoding='utf-8')
        result = subprocess.run([
            str(args.producer.resolve()), '--plan', str(path), '--config', str(config),
            '--measured-reference-source', str(measured), '--output-directory', str(bundle),
        ], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        rows = []
        if (bundle / 'manifest.json').is_file():
            manifest = json.loads((bundle / 'manifest.json').read_text(encoding='utf-8'))
            for trace in manifest['traces']:
                rows += [json.loads(line) for line in (bundle / trace['relative_path']).read_text(encoding='utf-8').splitlines()]
        passed = (result.returncode == 0 and rows and
                  'physical_output_capability=false' in result.stdout and
                  all(row['backend_completed_at_ns'] == row['control_at_ns'] + 100_000 and
                      row['backend_completed_at_ns'] <= maximum for row in rows)) if accepted else (
                          result.returncode == 2 and not bundle.exists())
        record = dict(case=name, passed=bool(passed), expected_accept=accepted,
                      exit_code=result.returncode, bundle_exists=bundle.exists(),
                      control_times=[row['control_at_ns'] for row in rows],
                      completion_times=[row['backend_completed_at_ns'] for row in rows],
                      stdout=result.stdout, stderr=result.stderr)
        results.append(record)
    (root / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item['passed'] for item in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
