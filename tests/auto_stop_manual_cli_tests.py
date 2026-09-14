import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    exe = str(Path(parser.parse_args().executable).resolve())
    def run(*args, good=True):
        p = subprocess.run([exe, *map(str, args)], capture_output=True, timeout=15)
        assert (p.returncode == 0) == good, (p.returncode, p.stderr.decode('utf-8', 'replace'))
    with tempfile.TemporaryDirectory(prefix='xen-manual-cli-') as temp:
        root = Path(temp)
        baseline = root/'baseline'
        run('--derive-defaults','--output',baseline)
        defaults=json.loads((baseline/'default-baseline.json').read_text(encoding='utf-8'))
        plan=json.loads((baseline/'plan.json').read_text(encoding='utf-8'))
        assert defaults['source']=='FORMULA_ONLY_NO_MANUAL_DATA'
        assert defaults['settings_source']=='REFERENCE_INITIAL_ASSUMPTIONS'
        assert [plan[k] for k in ['move_ms','counter_hold_ms','shot_after_release_ms','fire_delay_ms']]==[182,72,2,13]
        run('--plan',baseline/'plan.json','--dry-run','--require-current-plan')
        run('--derive-defaults','--output',baseline,good=False)
        for flag in ['--allow-physical-output','--capture-check','--record-manual','--dry-run','--require-current-plan']:
            run('--derive-defaults','--output',root/'bad-defaults',flag,good=False)
            assert not (root/'bad-defaults').exists()
        recording = root / 'recording'
        raw = recording / 'raw'
        raw.mkdir(parents=True)
        settings = dict(max_move_speed=1, clean_shot_speed_ratio=.34, accel_per_sec=5.5,
                        natural_decel_per_sec=2.5, counter_strafe_accel_per_sec=14,
                        fire_sample_delay_ms=18, tap_max_hold_ms=90, auto_fire_interval_ms=100, hud_enabled=True)
        (recording / 'sampling-settings.json').write_text(json.dumps(settings), encoding='utf-8')
        (recording / 'sampling-analysis.json').write_text(json.dumps(dict(time_ns=230000000)), encoding='utf-8')
        labels = dict(schema_version=1, recording_id='recording', qualified_shot_ranges=[[1,1]], rejected_shot_ranges=[])
        (recording / 'labels.json').write_text(json.dumps(labels), encoding='utf-8')
        columns = 'epoch,sequence,received_at_ns,dx,dy,held_mask,left_down,state_valid,motion_valid,gap,physical_motion_verified,source_loss_verifiable,timing_uncertainty_ns,raw_report_valid,datagram_size,raw_report'
        events = [(1,0,0), (10,2,0), (110,0,0), (200,0,1), (205,0,0), (230,0,0)]
        rows = [','.join(map(str,[1,i+1,ms*1000000,0,0,mask,left,1,0,0,0,0,-1,0,0,'00'*20]))
                for i,(ms,mask,left) in enumerate(events)]
        data = (columns+'\n'+'\n'.join(rows)+'\n').encode()
        (raw/'events-0.csv').write_bytes(data)
        (raw/'manifest.txt').write_bytes(f'XEN_INPUT_TRAINING_V1\n1 6 0 {len(data)} 3 0 262144\n'.encode('ascii'))
        output = root/'evaluated'
        run('--evaluate-manual',recording,'--output',output)
        report=json.loads((output/'sampling-analysis.json').read_text(encoding='utf-8'))
        assert report['source']=='KMBOX_MONITOR' and report['shot_count']==1
        assert report['shots'][0]['samples'][0]['sample_id']=='1:1'
        assert report['shots'][0]['human_label']=='QUALIFIED'
        assert report['calibration_envelope']['settings_applied'] is False
        assert report['calibration_envelope']['fit_status']=='INSUFFICIENT_LABEL_CLASSES'
        assert report['archive_complete'] is True and (output/'debug-report.html').is_file()
        assert report['feedback']['shooting']['hold_count']==1
        assert report['feedback']['baseline']['default_sampling']['clean_shot_speed_ratio']==.34
        assert (output/'operation-intervals.json').is_file() and (output/'manual-plan-proposals.json').is_file()
        assert json.loads((output/'operation-intervals.json').read_text(encoding='utf-8'))['recording_id']=='recording'
        assert report['manual_plan_proposals']['recording_id']=='recording'
        assert report['operation_intervals']['metrics']['hold_A']['mean_ms']==100
        assert report['operation_intervals']['metrics']['fire_hold']['mean_ms']==5
        assert report['operation_intervals']['metrics']['release_to_fire']['mean_ms']==90
        assert report['manual_plan_proposals']['groups'][0]['candidate_plan'] is None
        assert report['manual_plan_proposals']['groups'][0]['mean_ms']['shot_after_release_ms']==90
        assert (raw/'events-0.csv').read_bytes()==data
        changed=dict(settings, fire_sample_delay_ms=30)
        override=root/'override.json';override.write_text(json.dumps(changed),encoding='utf-8')
        run('--derive-defaults','--output',root/'changed-defaults','--sampling-settings',override)
        changed_defaults=json.loads((root/'changed-defaults'/'default-baseline.json').read_text(encoding='utf-8'))
        assert changed_defaults['candidate_plan']['fire_delay_ms']==25
        assert changed_defaults['settings_source']=='USER_SUPPLIED_ASSUMPTIONS'
        run('--evaluate-manual',recording,'--output',root/'override-result','--sampling-settings',override)
        other=json.loads((root/'override-result'/'sampling-analysis.json').read_text(encoding='utf-8'))
        assert other['original_sampling_settings']==settings and other['sampling_settings_overridden'] is True
        assert other['shots'][0]['samples'][0]['time_ns']==230000000
        run('--evaluate-manual',recording,'--output',output,good=False)
        for flag in ['--allow-physical-output','--capture-check','--record-manual']:
            run('--evaluate-manual',recording,'--output',root/'bad',flag,good=False)
            assert not (root/'bad').exists()
        run('--record-manual','--config',root/'missing.ini','--output',root/'bad',good=False)
        run('--record-manual','--config',root/'missing.ini','--output',root/'bad','--allow-physical-output',good=False)
        run('--show-hud',output/'sampling-analysis.json','--allow-physical-output',good=False)
        # 未标注的无效尾部也不能在离线重评中变成完整档案。
        invalid_row = ','.join(map(str,[1,7,240000000,0,0,0,0,0,0,0,0,0,-1,0,0,'00'*20]))
        invalid_data = data + (invalid_row+'\n').encode()
        (recording/'sampling-analysis.json').write_text(json.dumps(dict(time_ns=250000000)),encoding='utf-8')
        (raw/'events-0.csv').write_bytes(invalid_data)
        (raw/'manifest.txt').write_bytes(f'XEN_INPUT_TRAINING_V1\n1 7 0 {len(invalid_data)} 3 0 262144\n'.encode('ascii'))
        run('--evaluate-manual',recording,'--output',root/'invalid-result')
        invalid_report=json.loads((root/'invalid-result'/'sampling-analysis.json').read_text(encoding='utf-8'))
        assert invalid_report['archive_complete'] is False
        assert invalid_report['calibration_envelope']['fit_status']=='EVIDENCE_INCOMPLETE'
        labels['recording_usable']=False
        (recording/'labels.json').write_text(json.dumps(labels),encoding='utf-8')
        run('--evaluate-manual',recording,'--output',root/'user-excluded',good=False)
        assert not (root/'user-excluded').exists()
        (raw/'events-0.csv').unlink()
        run('--evaluate-manual',recording,'--output',root/'broken',good=False)
        assert not (root/'broken').exists()
    print('人工重评与入口边界专项通过；未连接设备或打开HUD')


if __name__ == '__main__':
    main()
