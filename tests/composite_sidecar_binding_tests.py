"""真实Recorder→native CLI→producer→binder→evaluator的来源绑定合同。"""
from __future__ import annotations
import argparse
import copy
import json
import pathlib
import subprocess
import tempfile
from composite_report_validation_tests import load, downstream


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seal-executable', type=pathlib.Path, required=True)
    parser.add_argument('--recorder-fixture', type=pathlib.Path, required=True)
    parser.add_argument('--producer', type=pathlib.Path)
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parents[1]
    fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_producer_tests.py', 'binding_chain_fixture')
    binder_fixture = load(source / 'tests/mouse_effect_probe_b_composite_phase_binder_tests.py', 'binding_binder_fixture')
    producer = (args.producer or source / 'scripts/produce_mouse_effect_probe_b_composite_phase_ledgers.py').resolve()
    binder = source / 'scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py'
    evaluator = source / 'scripts/evaluate_mouse_effect_probe_b_composite_phase.py'
    verifier = args.seal_executable.resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix='xen-sidecar-binding-') as temporary:
        root = pathlib.Path(temporary)
        paths = fixture.build_raw(root, binder_fixture, binder, evaluator, producer, verifier)
        paths.update(binder=binder, evaluator=evaluator)
        plan = json.loads(paths['plan'].read_bytes())
        binding = {
            'schema_version': 4, 'evidence_type': 'mouse_effect_probe_binding',
            'experiment': 'physical_b_composite_phase_calibration', 'profile': 'physical_b_composite_phase_calibration',
            'dispatch_mode': 'physical_b', 'run_role': 'calibration_deletion',
            'run_uuid': plan['run_uuid'], 'activation_epoch': plan['activation_epoch'], 'scope_id': plan['scope_id'],
            'sequence_sha256': plan['sequence_binding']['sequence_semantic_sha256'],
            'capture_source_name': plan['capture_policy']['source_name'],
            'sidecar_physical_output_capability': False, 'normal_aim_output_required': False,
            'dy_counts_required': 0, 'max_abs_pulse_counts': 1, 'max_abs_prefix_x_counts': 1,
            'production_aim_changed': False, 'new_production_gain_claimed': False,
        }
        input_binding = root / 'probe-binding.json'
        fixture.write_json(input_binding, binding)
        output = root / 'recorded'
        recorded = subprocess.run([str(args.recorder_fixture.resolve()), str(paths['manifest']), str(input_binding), str(output)],
            capture_output=True, text=True, encoding='utf-8')
        fixture.expect(recorded.returncode == 0, '真实Recorder失败: ' + recorded.stderr)
        paths['manifest'] = output / 'manifest.json'
        source_binding = output / 'source-binding.json'
        manifest = json.loads(paths['manifest'].read_bytes())
        fixture.expect(manifest['recorded_frame_count'] == 252 and manifest['source_binding']['sha256'] == fixture.file_sha256(source_binding),
                       '实际Recorder必须发布252帧与源绑定摘要')
        report = json.loads(paths['report'].read_bytes())
        report['binding']['probe_binding_sha256'] = fixture.file_sha256(source_binding)
        # 调度账本的绑定同样来自这次真实Recorder关联的源文件。
        def update_report(changed_report):
            fixture.seal_report_native(changed_report, paths['report'], verifier)
            schedule = json.loads(paths['schedule'].read_bytes())
            schedule['probe_binding_sha256'] = changed_report['binding']['probe_binding_sha256']
            fixture.write_json(paths['schedule'], schedule)
            fixture.update_schedule_for_files(paths)
        update_report(report)
        valid, capture, commands = fixture.invoke(producer, paths, root, 'valid')
        fixture.expect(valid.returncode == 0, '真实Recorder正例producer失败: ' + valid.stderr)
        downstream(paths, capture, commands, root, 'valid')
        print(recorded.stdout.strip())
        print('real Recorder/native verifier/producer/binder/evaluator positive accepted')
        for case in ('missing-binding', 'wrong-hash', 'wrong-scope', 'wrong-run', 'wrong-epoch', 'wrong-sequence', 'wrong-source', 'escape-path', 'absolute-path', 'wrong-report-hash', 'unsafe-binding', 'bool-epoch'):
            changed_manifest = copy.deepcopy(manifest)
            changed_report = copy.deepcopy(report)
            changed_binding = copy.deepcopy(binding)
            if case == 'missing-binding':
                changed_manifest.pop('source_binding')
            elif case == 'wrong-hash':
                changed_manifest['source_binding']['sha256'] = '0' * 64
            elif case == 'escape-path':
                changed_manifest['source_binding']['file'] = '../probe-binding.json'
            elif case == 'absolute-path':
                changed_manifest['source_binding']['file'] = str(source_binding.resolve())
            elif case == 'wrong-report-hash':
                changed_report['binding']['probe_binding_sha256'] = '0' * 64
            else:
                key, value = {
                    'wrong-scope': ('scope_id', 'f' * 64),
                    'wrong-run': ('run_uuid', '00000000-0000-0000-0000-000000000001'),
                    'wrong-epoch': ('activation_epoch', plan['activation_epoch'] + 1),
                    'wrong-sequence': ('sequence_sha256', '0' * 64),
                    'wrong-source': ('capture_source_name', 'different synthetic source'),
                    'unsafe-binding': ('sidecar_physical_output_capability', True),
                    'bool-epoch': ('activation_epoch', True),
                }[case]
                fixture.expect(changed_binding[key] != value or type(changed_binding[key]) is not type(value), 'negative fixture must change identity')
                changed_binding[key] = value
            fixture.write_json(source_binding, changed_binding)
            if case not in ('missing-binding', 'wrong-hash', 'escape-path', 'absolute-path', 'wrong-report-hash'):
                changed_manifest['source_binding']['sha256'] = fixture.file_sha256(source_binding)
                changed_report['binding']['probe_binding_sha256'] = fixture.file_sha256(source_binding)
            fixture.write_json(paths['manifest'], changed_manifest)
            update_report(changed_report)
            rejected, bad_capture, bad_commands = fixture.invoke(producer, paths, root, case)
            published = bad_capture.exists() or bad_commands.exists()
            if rejected.returncode == 0:
                downstream(paths, bad_capture, bad_commands, root, case)
            print(f'{case}: exit={rejected.returncode} published={published}')
            if rejected.returncode != 2 or published:
                failures.append(case)
    if failures:
        print('FAILED: ' + ', '.join(failures))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
