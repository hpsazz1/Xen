"""反向轻点入口的无设备回归；绝不传递有效物理授权。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def read_generated_json(path):
    # Prepare只生成独立整行注释；通用JSONC接受/拒绝由正式CLI回归验证。
    text = path.read_text(encoding='utf-8-sig')
    return json.loads('\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('//')))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', default=os.environ.get('XEN_COUNTERPULSE_EXE'))
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'scripts' / 'invoke_auto_stop_counterpulse.ps1'
    shell = shutil.which('pwsh') or shutil.which('powershell')
    assert shell, '需要PowerShell执行入口回归'
    # 子程序切换代码页后，父Shell不能继续用缓存的GBK写UTF-8控制台。
    legacy = shutil.which('powershell.exe')
    if legacy:
        command = "[Console]::OutputEncoding=[Text.Encoding]::GetEncoding(936); & '" + str(script).replace("'", "''") + "' -Mode Launch -RunDirectory 'C:/missing-counterpulse-regression'"
        encoded = subprocess.run([legacy, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                                 capture_output=True, timeout=20)
        assert encoded.returncode != 0
        decoded = encoded.stderr.decode('utf-8', errors='strict')
        assert 'COUNTERPULSE' in decoded
    syntax = "$tokens=$null;$errors=$null;[System.Management.Automation.Language.Parser]::ParseFile('" + str(script).replace("'", "''") + "',[ref]$tokens,[ref]$errors)>$null;if($errors.Count){exit 1}"
    subprocess.run([shell, '-NoProfile', '-Command', syntax], check=True, timeout=20)

    def invoke(*values, ok=False, entry=script):
        result = subprocess.run([shell, '-NoProfile', '-File', str(entry), *map(str, values)],
                                capture_output=True, timeout=30)
        assert (result.returncode == 0) == ok, '入口返回值不符合预期'
        return result

    with tempfile.TemporaryDirectory(prefix='xen-counterpulse-cli-') as folder:
        root = Path(folder)
        run = root / 'run'
        invoke('-Mode', 'Launch', '-RunDirectory', run)
        assert not run.exists(), '缺授权不能创建Run'
        invoke('-Mode', 'Prepare', '-RunDirectory', run)
        assert not run.exists(), '缺文件不能创建Run'
        invoke('-Mode', 'Prepare', '-RunDirectory', run, '-AllowPhysicalOutput')
        invoke('-Mode', 'Prepare', '-RunDirectory', run, '-Confirm', 'AUTO_STOP_COUNTERPULSE')
        assert not run.exists(), 'Prepare拒绝混入物理授权'
        if args.executable:
            # 复用已校准的false分支必须完成整个Prepare，包括TASK文本与快捷入口生成。
            sequential_run = root / 'sequential-movement'
            sequential_config = root / 'sequential-private.ini'
            sequential_config.write_text('[source_context]\nenabled=true\n', encoding='utf-8')
            sequential_args = ('-Mode', 'Prepare', '-RunDirectory', sequential_run, '-Executable', args.executable,
                               '-ConfigPath', sequential_config, '-Repeatable', '-CredentialDirectory',
                               root / "credential's directory", '-Scope', 'LocalMachine')
            invoke(*sequential_args, ok=True)
            # 新入口仅通过进程启动桩验证；绝不执行真实人工监听或打开HUD。
            manual_entry = sequential_run / 'record-manual.ps1'
            hud_entry = sequential_run / 'show-hud.ps1'
            for name in ('start-recording.bat', 'record-manual.ps1', 'show-hud.bat', 'show-hud.ps1'):
                assert (sequential_run / name).is_file(), 'Prepare须生成录制及常驻查看入口'
            original_manual = manual_entry.read_bytes()
            original_hud = hud_entry.read_bytes()
            original_task = (sequential_run / 'task.json').read_bytes()
            manual_text = original_manual.decode('utf-8-sig')
            assert '--record-manual' in manual_text and '--recording-duration-ms' in manual_text and '120000' in manual_text
            assert '--allow-physical-output' not in manual_text and '--confirm' not in manual_text
            assert 'ProtectedData' not in manual_text and 'token.dpapi' not in manual_text
            assert 'WaitForExit()' in manual_text and '.Kill(' not in manual_text
            remote_env = dict(os.environ, SSH_CONNECTION='mock-remote')
            for entry in (manual_entry, hud_entry):
                denied = subprocess.run([shell, '-NoProfile', '-File', str(entry)], env=remote_env,
                                        capture_output=True, timeout=10)
                assert denied.returncode != 0, '人工入口拒绝SSH'
            # 移除前台检查的测试副本只用于纯文件/参数模拟。
            session_guard = "if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT)"
            process_start = '$child = [Diagnostics.Process]::Start($info)'
            stub_start = """if ($action -eq 'record') {
    if (-not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($output))) { throw '录制父目录必须先准备' }
    $null = [IO.Directory]::CreateDirectory($output)
    [IO.File]::WriteAllText((Join-Path $output 'mock-arguments.json'), (ConvertTo-Json -InputObject @($arguments)))
} else { [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'mock-hud.json'), (ConvertTo-Json -InputObject @($arguments))) }
exit 0
"""
            binding = read_generated_json(sequential_run / 'task.json')
            for entry, field in ((manual_entry, 'record_manual_sha256'), (hud_entry, 'show_hud_sha256')):
                text = entry.read_text(encoding='utf-8-sig').replace(session_guard, 'if ($false)')
                assert process_start in text
                text = text.replace(process_start, stub_start)
                entry.write_bytes(text.encode('utf-8-sig'))
                binding[field] = hashlib.sha256(entry.read_bytes()).hexdigest().upper()
            (sequential_run / 'task.json').write_text(json.dumps(binding), encoding='utf-8')
            invoke(entry=hud_entry)  # 无报告必须失败，不能启动空HUD。
            invoke(entry=manual_entry, ok=True)
            invoke(entry=manual_entry, ok=True)
            manual_runs = list((sequential_run / 'manual-recordings').iterdir())
            assert len(manual_runs) == 2 and manual_runs[0].name != manual_runs[1].name
            for manual_run in manual_runs:
                values = read_generated_json(manual_run / 'mock-arguments.json')
                assert values[0] == '--record-manual' and '--allow-physical-output' not in values
                assert values[values.index('--output') + 1] == str(manual_run)
                assert values[values.index('--config') + 1] == str(sequential_config)
                assert values[values.index('--recording-duration-ms') + 1] == '120000'
            report = manual_runs[-1] / 'sampling-analysis.json'
            report.write_text('{}', encoding='utf-8')
            invoke(entry=hud_entry, ok=True)
            assert read_generated_json(sequential_run / 'mock-hud.json') == ['--show-hud', str(report)]
            # 原绑定文件变化时，在进程桩前拒绝，不能只验证脚本自摘要。
            saved_config = sequential_config.read_bytes()
            sequential_config.write_bytes(saved_config + b'\n')
            invoke(entry=manual_entry)
            assert len(list((sequential_run / 'manual-recordings').iterdir())) == 2
            sequential_config.write_bytes(saved_config)
            manual_entry.write_bytes(original_manual); hud_entry.write_bytes(original_hud)
            (sequential_run / 'task.json').write_bytes(original_task)
            # 离线素材入口：仅模拟分析进程和前台编辑器，Prepare/dry-run使用正式程序。
            analyze_entry = sequential_run / 'analyze-recording.ps1'
            assert (sequential_run / 'analyze-recording.bat').is_file()
            analyze_original = analyze_entry.read_bytes()
            analyze_text = analyze_original.decode('utf-8-sig')
            assert 'analyze_recording_sha256' in analyze_text
            assert '--evaluate-manual' in analyze_text and '--require-current-plan' in analyze_text
            assert "Mode='Prepare'" in analyze_text and "Mode='Launch'" not in analyze_text
            report.write_text(json.dumps(dict(source='KMBOX_MONITOR', analysis_mode='MANUAL_RECEIVE_INPUT_MODEL')), encoding='utf-8')
            excluded_recording = sequential_run / 'manual-recordings' / 'excluded-latest'
            excluded_recording.mkdir()
            excluded_report = excluded_recording / 'sampling-analysis.json'
            excluded_report.write_bytes(report.read_bytes())
            os.utime(excluded_report, (1900000000, 1900000000))
            excluded_labels = excluded_recording / 'labels.json'
            excluded_labels.write_text(json.dumps({'recording_usable': False, 'exclusion_reason': '用户指出HUD问题导致数据不准确'}), encoding='utf-8')
            auto_result = sequential_run / 'result'
            auto_result.mkdir()
            (auto_result / 'sampling-analysis.json').write_text('{}', encoding='utf-8')
            process_end = '$child.WaitForExit(); $code = $child.ExitCode; $child.Dispose()'
            start = analyze_text.index(process_start)
            end = analyze_text.index(process_end, start) + len(process_end)
            fixture_process = """$null = [IO.Directory]::CreateDirectory($output)
[IO.File]::WriteAllText((Join-Path $output 'mock-evaluate.json'), (ConvertTo-Json -InputObject @($arguments)))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'mock-proposals.json') -Destination (Join-Path $output 'manual-plan-proposals.json')
$used = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json
@{settings=$used} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'sampling-analysis.json')
$code = 0
"""
            mocked = (analyze_text[:start] + fixture_process + analyze_text[end:]).replace(session_guard, 'if ($false)')
            editor = "Start-Process -FilePath 'notepad.exe' -ArgumentList ('\"' + $editable + '\"') | Out-Null"
            assert editor in mocked
            mocked = mocked.replace(editor, "# 测试不打开编辑器")
            mocked = mocked.replace("Start-Process -FilePath (Join-Path $output 'debug-report.html') | Out-Null", '# 测试不打开浏览器')
            mocked = "function Read-Host { param($prompt) if ($prompt.StartsWith('输入候选')) { '1' } else { '' } }\n" + mocked
            analyze_entry.write_bytes(mocked.encode('utf-8-sig'))
            binding = json.loads(original_task.decode('utf-8-sig'))
            binding['analyze_recording_sha256'] = hashlib.sha256(analyze_entry.read_bytes()).hexdigest().upper()
            (sequential_run / 'task.json').write_text(json.dumps(binding), encoding='utf-8')
            fixture = sequential_run / 'mock-proposals.json'
            fixture.write_text(json.dumps({'groups': [{'baseline': 'unsupported', 'candidate_plan': None,
                'proposed_plan': None, 'reasons': ['合成不支持原因']}]}), encoding='utf-8')
            invoke(entry=analyze_entry, ok=True)
            reviews = list((sequential_run / 'manual-reviews').iterdir())
            assert len(reviews) == 1 and not (reviews[0] / 'test').exists()
            evaluate_args = read_generated_json(reviews[0] / 'mock-evaluate.json')
            assert evaluate_args[:2] == ['--evaluate-manual', str(report.parent)]
            assert excluded_report.exists(), '排除推荐不删除诊断报告'
            saved_labels = excluded_labels.read_bytes()
            excluded_labels.write_bytes(b' ' * (16 * 1024 + 1))
            invoke(entry=analyze_entry)
            assert len(list((sequential_run / 'manual-reviews').iterdir())) == 1, '超限标签须在分析进程之前拒绝'
            excluded_labels.write_bytes(saved_labels)
            candidate = read_generated_json(sequential_run / 'plan.json')
            candidate.update(shots=8, fire_delay_ms=310, fire_interval_ms=550, move_during_fire_delay=False,
                move_ms=280, counter_hold_ms=23, counter_delay_ms=3, shot_after_release_ms=2,
                shot_hold_ms=12, late_tolerance_ms=7, direction=8)
            fixture.write_text(json.dumps({'groups': [{'baseline': 'counter', 'direction': 'D', 'samples': 4,
                'candidate_plan': candidate, 'proposed_plan': candidate}]}), encoding='utf-8')
            invoke(entry=analyze_entry, ok=True)
            reviews = list((sequential_run / 'manual-reviews').iterdir())
            assert len(reviews) == 2
            prepared = next(item / 'test' for item in reviews if (item / 'test').is_dir())
            assert read_generated_json(prepared / 'plan.json') == candidate, '候选全部参数须传到正式Prepare'
            prepared_task = read_generated_json(prepared / 'task.json')
            assert prepared_task['executable'] == binding['executable'] and prepared_task['config'] == binding['config']
            assert prepared_task['status'] == 'PREPARED_NOT_LAUNCHED' and prepared_task['repeatable'] is True
            nested_analyze = (prepared / 'analyze-recording.ps1').read_text(encoding='utf-8-sig')
            assert "credential''s directory'" in nested_analyze and "$originalScope = 'LocalMachine'" in nested_analyze
            assert read_generated_json(prepared / 'sampling-settings.json') == read_generated_json(sequential_run / 'sampling-settings.json')
            assert (prepared / 'edit-config.bat').is_file() and (prepared / 'start-test.bat').is_file()
            assert not (prepared / 'result').exists()
            # 动态候选必须经正式Prepare保留开关，不能退回固定move_ms执行。
            dynamic_candidate = dict(candidate, overlap_fire_interval=True, fire_delay_ms=0, move_ms=500,
                                     restart_interval_ms=150)
            fixture.write_text(json.dumps({'groups': [{'baseline': 'counter', 'direction': 'D', 'samples': 4,
                'candidate_plan': dynamic_candidate, 'proposed_plan': dynamic_candidate}]}), encoding='utf-8')
            invoke(entry=analyze_entry, ok=True)
            dynamic_prepared = next(item / 'test' for item in (sequential_run / 'manual-reviews').iterdir()
                if item not in reviews and (item / 'test').is_dir())
            assert read_generated_json(dynamic_prepared / 'plan.json') == dynamic_candidate
            invoke('-Mode', 'Prepare', '-RunDirectory', dynamic_prepared, '-Executable', args.executable,
                '-ConfigPath', sequential_config, '-ReuseRunDirectory', ok=True)
            assert read_generated_json(dynamic_prepared / 'plan.json') == dynamic_candidate, '复用必须继承动态开关与重新起步间隔'
            assert '动态移动占用' in (dynamic_prepared / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '下一正向DOWN提交至少150ms' in (dynamic_prepared / 'TASK.md').read_text(encoding='utf-8-sig')
            invoke('-Mode', 'Prepare', '-RunDirectory', dynamic_prepared, '-Executable', args.executable,
                '-ConfigPath', sequential_config, '-ReuseRunDirectory', '-RestartIntervalMs', 200, ok=True)
            assert read_generated_json(dynamic_prepared / 'plan.json') == dict(dynamic_candidate, restart_interval_ms=200)
            invoke('-Mode', 'Prepare', '-RunDirectory', dynamic_prepared, '-Executable', args.executable,
                '-ConfigPath', sequential_config, '-ReuseRunDirectory', '-RestartIntervalMs', 0, ok=True)
            without_restart = dict(dynamic_candidate)
            without_restart.pop('restart_interval_ms')
            assert read_generated_json(dynamic_prepared / 'plan.json') == without_restart, '显式0清除可选字段并恢复原行为'
            # 超界提案必须保留原值进入编辑，不可clamp；选择0退出仍不Prepare。
            invalid_candidate = dict(candidate, counter_delay_ms=-5)
            fixture.write_text(json.dumps({'groups': [{'baseline': 'counter', 'direction': 'D', 'samples': 4,
                'candidate_plan': None, 'proposed_plan': invalid_candidate,
                'reasons': ['重叠动作'], 'validation_errors': ['counter_delay_ms不能为负']}]}), encoding='utf-8')
            cancelled = "$script:editPrompts = 0\n" + mocked.replace("else { '' }",
                "else { $script:editPrompts++; if ($script:editPrompts -eq 1) { '' } else { '0' } }")
            analyze_entry.write_bytes(cancelled.encode('utf-8-sig'))
            binding['analyze_recording_sha256'] = hashlib.sha256(analyze_entry.read_bytes()).hexdigest().upper()
            (sequential_run / 'task.json').write_text(json.dumps(binding), encoding='utf-8')
            invoke(entry=analyze_entry, ok=True)
            pending = [item for item in (sequential_run / 'manual-reviews').iterdir()
                       if (item / 'editable-plan.json').exists() and not (item / 'test').exists()]
            assert len(pending) == 1 and read_generated_json(pending[0] / 'editable-plan.json')['counter_delay_ms'] == -5
            # 常驻查看必须包含离线重评，使用报告冻结的参数，不回退到原始录制。
            review_report = pending[0] / 'sampling-analysis.json'
            reviewed = read_generated_json(review_report)
            reviewed['settings']['fire_sample_delay_ms'] = 47
            review_report.write_text(json.dumps(reviewed), encoding='utf-8')
            os.utime(review_report, (2000000000, 2000000000))
            nested = pending[0] / 'test' / 'raw'
            nested.mkdir(parents=True)
            nested_report = nested / 'sampling-analysis.json'
            nested_report.write_text('{}', encoding='utf-8')
            os.utime(nested_report, (2100000000, 2100000000))
            hud_mock = original_hud.decode('utf-8-sig').replace(session_guard, 'if ($false)').replace(process_start, stub_start)
            hud_entry.write_bytes(hud_mock.encode('utf-8-sig'))
            binding['show_hud_sha256'] = hashlib.sha256(hud_entry.read_bytes()).hexdigest().upper()
            (sequential_run / 'task.json').write_text(json.dumps(binding), encoding='utf-8')
            invoke(entry=hud_entry, ok=True)
            selected = read_generated_json(sequential_run / 'mock-hud.json')
            assert selected == ['--show-hud', str(review_report)], '重评须优先旧录制，且不能递归选中test/raw'
            assert read_generated_json(Path(selected[1]))['settings']['fire_sample_delay_ms'] == 47
            hud_entry.write_bytes(original_hud)
            analyze_entry.write_bytes(analyze_original)
            (sequential_run / 'task.json').write_bytes(original_task)
            # 默认方案独立于人工数据；只模拟推导进程，正式Prepare及dry-run不接设备。
            defaults_entry = sequential_run / 'prepare-default-test.ps1'
            assert (sequential_run / 'prepare-default-test.bat').is_file()
            defaults_original = defaults_entry.read_bytes()
            defaults_text = defaults_original.decode('utf-8-sig')
            assert 'prepare_default_test_sha256' in defaults_text
            start = defaults_text.index(process_start)
            end = defaults_text.index(process_end, start) + len(process_end)
            defaults_stub = """$null = [IO.Directory]::CreateDirectory($output)
[IO.File]::WriteAllText((Join-Path $output 'mock-derive.json'), (ConvertTo-Json -InputObject @($arguments)))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'mock-default-plan.json') -Destination (Join-Path $output 'plan.json')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'mock-default-settings.json') -Destination (Join-Path $output 'sampling-settings.json')
'{"derived":{},"assumptions":{}}' | Set-Content -LiteralPath (Join-Path $output 'default-baseline.json')
$code = 0
"""
            defaults_text = (defaults_text[:start] + defaults_stub + defaults_text[end:]).replace(session_guard, 'if ($false)')
            defaults_entry.write_bytes(defaults_text.encode('utf-8-sig'))
            default_binding = json.loads(original_task.decode('utf-8-sig'))
            default_binding['prepare_default_test_sha256'] = hashlib.sha256(defaults_entry.read_bytes()).hexdigest().upper()
            (sequential_run / 'task.json').write_text(json.dumps(default_binding), encoding='utf-8')
            (sequential_run / 'mock-default-plan.json').write_text(json.dumps(candidate), encoding='utf-8')
            seed = read_generated_json(sequential_run / 'sampling-settings.json')
            seed['fire_sample_delay_ms'] = 19
            (sequential_run / 'mock-default-settings.json').write_text(json.dumps(seed), encoding='utf-8')
            excluded_labels.write_bytes(b' ' * (16 * 1024 + 1))  # 若默认入口读取人工记录会失败。
            invoke(entry=defaults_entry, ok=True)
            default_runs = list((sequential_run / 'default-baselines').iterdir())
            assert len(default_runs) == 1
            default_run = default_runs[0]
            assert read_generated_json(default_run / 'mock-derive.json') == ['--derive-defaults', '--output', str(default_run)]
            assert read_generated_json(default_run / 'test' / 'plan.json') == candidate
            assert read_generated_json(default_run / 'test' / 'sampling-settings.json') == seed
            assert read_generated_json(default_run / 'test' / 'task.json')['status'] == 'PREPARED_NOT_LAUNCHED'
            assert not (default_run / 'test' / 'result').exists()
            assert all((default_run / 'test' / name).exists() for name in ('edit-config.bat', 'edit-sampling.bat', 'start-test.bat'))
            inherited = (default_run / 'test' / 'prepare-default-test.ps1').read_text(encoding='utf-8-sig')
            assert "credential''s directory'" in inherited and "$originalScope = 'LocalMachine'" in inherited
            override = sequential_run / 'mock-default-settings.json'
            (sequential_run / 'mock-default-plan.json').write_text(json.dumps(dynamic_candidate), encoding='utf-8')
            invoke('-SamplingSettingsPath', override, entry=defaults_entry, ok=True)
            second = next(item for item in (sequential_run / 'default-baselines').iterdir() if item != default_run)
            assert read_generated_json(second / 'mock-derive.json')[-2:] == ['--sampling-settings', str(override)]
            assert read_generated_json(second / 'test' / 'plan.json') == dynamic_candidate, '默认派生也必须保留动态开关与重新起步间隔'
            defaults_entry.write_bytes(defaults_original)
            excluded_labels.write_bytes(saved_labels)
            (sequential_run / 'task.json').write_bytes(original_task)
            sequential_plan = read_generated_json(sequential_run / 'plan.json')
            sequential_plan['move_during_fire_delay'] = False
            (sequential_run / 'plan.json').write_text(json.dumps(sequential_plan), encoding='utf-8')
            invoke(*sequential_args, '-ReuseRunDirectory', ok=True)
            assert read_generated_json(sequential_run / 'plan.json')['move_during_fire_delay'] is False
            sequential_task = (sequential_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '静止等待300ms，然后按A保持300ms' in sequential_task
            assert (sequential_run / 'launch-test.ps1').is_file()
            # 正式离线入口复用生产Session；全部输入为合成命令回执，绝不连接设备。
            command_report = root / 'synthetic-result.json'
            commands = []
            for kind, value, stamp in [('wasd', 0, 1000000), ('left_button', 0, 2000000),
                    ('wasd', 2, 3000000), ('wasd', 0, 10000000), ('wasd', 8, 11000000),
                    ('wasd', 0, 16000000), ('left_button', 1, 17000000), ('left_button', 0, 22000000)]:
                commands.append(dict(kind=kind, value=value, disposition=2, submit_ns=stamp - 1,
                    ack_received_ns=stamp, backend_completed_ns=stamp + 1, returned_ns=stamp + 2))
            actual_plan = dict(schema_version=2, capture_enabled=False, baseline='counter', shots=1,
                fire_delay_ms=300, fire_interval_ms=2000, move_during_fire_delay=True, move_ms=7,
                counter_hold_ms=5, counter_delay_ms=1, shot_after_release_ms=1, shot_hold_ms=5,
                late_tolerance_ms=5, direction=2)
            original_sampling_settings = dict(max_move_speed=1.0, clean_shot_speed_ratio=0.34,
                accel_per_sec=5.5, natural_decel_per_sec=2.5, counter_strafe_accel_per_sec=14.0,
                fire_sample_delay_ms=25, tap_max_hold_ms=90, auto_fire_interval_ms=100, hud_enabled=True)
            hostile_failure = '</script><script>alert(1)</script>'
            command_report.write_text(json.dumps({'commands': commands, 'plan': actual_plan,
                'sampling_settings': original_sampling_settings,
                'success': False, 'failure': hostile_failure}), encoding='utf-8')
            evaluation_dir = root / 'evaluation'
            evaluated = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(command_report), '--output', str(evaluation_dir)], capture_output=True, timeout=10)
            assert evaluated.returncode == 0, '正式离线重评必须通过'
            evaluation = read_generated_json(evaluation_dir / 'training-evaluation.json')
            assert evaluation['physical_output'] is False and evaluation['monitor'] is None
            ack = evaluation['command_ack']
            assert ack['source'] == 'COMMAND_ACK' and ack['time_domain'] == 'LOCAL_STEADY_COMMAND_ACK'
            assert ack['game_shot_stability'] is None and ack['settled'] is None
            assert ack['physical_validation_passed'] is False and ack['invalid_receipts'] == 0
            assert ack['total_timings'] == 1 and ack['timings'][0]['delta_ns'] == 1000000
            assert ack['timings'][0]['grade'] == '完美' and ack['total_holds'] == 1, (ack['timings'], ack['total_holds'])
            assert (evaluation_dir / 'command-training' / 'manifest.txt').exists()
            sampling = read_generated_json(evaluation_dir / 'sampling-analysis.json')
            assert (evaluation_dir / 'debug-report.html').is_file()
            rendered_html = (evaluation_dir / 'debug-report.html').read_text(encoding='utf-8-sig')
            assert hostile_failure not in rendered_html
            assert r'\u003c/script' in rendered_html
            assert sampling['actual_plan'] == actual_plan
            assert sampling['settings']['fire_sample_delay_ms'] == 25
            assert sampling['shots'][0]['samples'][0]['time_ns'] == 42000000
            assert sampling['source'] == 'COMMAND_ACK_PROXY' and sampling['samples_are_bullets'] is False
            assert sampling['physical_validation_passed'] is False and sampling['game_shot_stability'] is None
            assert sampling['execution_success'] is False and sampling['execution_failure'] == hostile_failure
            assert sampling['initial_velocity_assumption'] == 'ZERO_NOT_PHYSICALLY_VERIFIED'
            assert sampling['quality_issues'] == [] and sampling['analysis_complete'] is True
            assert sampling['shots'][0]['planned_down_interval_ms'] == 2000
            override_settings = dict(original_sampling_settings, fire_sample_delay_ms=18, hud_enabled=False)
            override_path = root / 'sampling-override.json'
            override_path.write_text(json.dumps(override_settings), encoding='utf-8')
            override_output = root / 'evaluation-override'
            override_run = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(command_report), '--output', str(override_output), '--sampling-settings', str(override_path)],
                capture_output=True, timeout=10)
            assert override_run.returncode == 0
            overridden = read_generated_json(override_output / 'sampling-analysis.json')
            assert overridden['settings']['fire_sample_delay_ms'] == 18
            assert overridden['sampling_settings_overridden'] is True
            assert overridden['original_sampling_settings'] == original_sampling_settings
            assert overridden['shots'][0]['samples'][0]['time_ns'] == 35000000
            # 一秒按住形成多个模型采样点，但只有一次开火按住，绝不把点数说成子弹数。
            long_report = root / 'synthetic-long-hold.json'
            long_commands = [dict(item) for item in commands]
            for key, offset in [('submit_ns', -1), ('ack_received_ns', 0), ('backend_completed_ns', 1), ('returned_ns', 2)]:
                long_commands[-1][key] = 1017000000 + offset
            long_plan = dict(actual_plan, shot_hold_ms=1000)
            long_report.write_text(json.dumps({'commands': long_commands, 'plan': long_plan}), encoding='utf-8')
            long_output = root / 'evaluation-long-hold'
            long_evaluation = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(long_report), '--output', str(long_output)], capture_output=True, timeout=10)
            assert long_evaluation.returncode == 0
            long_sampling = read_generated_json(long_output / 'sampling-analysis.json')
            assert long_sampling['shot_count'] == 1 and long_sampling['samples_are_bullets'] is False
            assert long_sampling['first_sample_count'] == 1 and long_sampling['held_sample_count'] == 9
            assert long_sampling['total_sample_count'] == 10 and long_sampling['actual_plan']['shot_hold_ms'] == 1000
            assert long_sampling['shots'][0]['observed_hold_ms'] == 1000
            assert long_sampling['shots'][0]['samples'][0]['kind'] == 'FIRST_MODEL_SAMPLE'
            assert all(item['kind'] == 'HELD_MODEL_SAMPLE' for item in long_sampling['shots'][0]['samples'][1:])
            missing_plan_report = root / 'synthetic-missing-plan.json'
            missing_plan_report.write_text(json.dumps({'commands': commands}), encoding='utf-8')
            missing_plan_output = root / 'evaluation-missing-plan'
            missing_plan_run = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(missing_plan_report), '--output', str(missing_plan_output)], capture_output=True, timeout=10)
            assert missing_plan_run.returncode == 0
            missing_sampling = read_generated_json(missing_plan_output / 'sampling-analysis.json')
            assert missing_sampling['analysis_complete'] is False
            assert 'PLAN_MISSING:fire_interval_ms' in missing_sampling['quality_issues']
            assert (missing_plan_output / 'debug-report.html').is_file()
            saved_evaluation = (evaluation_dir / 'training-evaluation.json').read_bytes()
            duplicate = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(command_report), '--output', str(evaluation_dir)], capture_output=True, timeout=10)
            assert duplicate.returncode != 0
            assert (evaluation_dir / 'training-evaluation.json').read_bytes() == saved_evaluation
            for index, extra in enumerate([('--allow-physical-output',), ('--confirm', 'AUTO_STOP_COUNTERPULSE'),
                    ('--config', 'missing.ini'), ('--plan', 'missing.json'), ('--dry-run',), ('--capture-check',)]):
                forbidden = root / ('evaluation-conflict-' + str(index))
                conflict = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                    str(command_report), '--output', str(forbidden), *extra], capture_output=True, timeout=10)
                assert conflict.returncode != 0 and not forbidden.exists()
            commented = root / 'commented-plan.json'
            commented.write_text('{\n  "shots": 3, // 子弹数\n  "capture_enabled": false, /* 人工观察 */\n'
                                 '  "schema_version": 2, "fire_delay_ms": 300,\n'
                                 '  "move_ms": 300, "counter_delay_ms": 50, "counter_hold_ms": 5,\n'
                                 '  "shot_after_release_ms": 0\n}\n', encoding='utf-8')
            parsed = subprocess.run([str(Path(args.executable).resolve()), '--plan', str(commented), '--dry-run'],
                                    capture_output=True, timeout=10)
            assert parsed.returncode == 0, '计划行末中文注释及块注释应被正式解析器接受'
            commented.write_text('{"shots": 0 // 越界仍须拒绝\n}', encoding='utf-8')
            rejected = subprocess.run([str(Path(args.executable).resolve()), '--plan', str(commented), '--dry-run'],
                                      capture_output=True, timeout=10)
            assert rejected.returncode != 0, '允许注释不得绕过数值边界'
            commented.write_text('{"shots": 3, /* 未闭合注释', encoding='utf-8')
            rejected = subprocess.run([str(Path(args.executable).resolve()), '--plan', str(commented), '--dry-run'],
                                      capture_output=True, timeout=10)
            assert rejected.returncode != 0, '损坏的注释仍须拒绝'
            config = root / 'private.ini'
            config.write_text('[source_context]\nenabled=true\n', encoding='utf-8')
            for authorization in [('-AllowPhysicalOutput',), ('-Confirm', 'AUTO_STOP_COUNTERPULSE')]:
                invoke('-Mode', 'Prepare', '-RunDirectory', run, '-Executable', Path(args.executable).resolve(),
                       '-ConfigPath', config, *authorization)
                assert not run.exists(), '完整Prepare仍必须拒绝授权混用'
            invoke('-Mode', 'Prepare', '-RunDirectory', run, '-Executable', Path(args.executable).resolve(),
                   '-ConfigPath', config, ok=True)
            assert not (run / 'result').exists()
            assert not (run / 'CONSUMED').exists()
            assert not (run / 'config.ini').exists()
            task = read_generated_json(run / 'task.json')
            plan = read_generated_json(run / 'plan.json')
            # 纯采集模式不允许混入物理授权；拒绝发生在加载配置与设备之前。
            for extra in [('--allow-physical-output',), ('--confirm', 'AUTO_STOP_COUNTERPULSE'), ('--dry-run',)]:
                diagnostic = root / 'capture-must-not-exist'
                rejected = subprocess.run([str(Path(args.executable).resolve()), '--capture-check',
                    '--plan', str(run / 'plan.json'), '--config', str(config), '--output', str(diagnostic), *extra],
                    capture_output=True, timeout=10)
                assert rejected.returncode != 0 and not diagnostic.exists()
            for leaf in ('start-test.bat', 'edit-config.bat', 'launch-test.ps1', 'PARAMETERS.md', 'open-report.bat', 'open-report.ps1', 'sampling-settings.json', 'edit-sampling.bat'):
                assert (run / leaf).is_file()
            start_bat = (run / 'start-test.bat').read_text(encoding='ascii')
            edit_bat = (run / 'edit-config.bat').read_text(encoding='ascii')
            assert 'DisableDelayedExpansion' in start_bat and '"%~dp0launch-test.ps1"' in start_bat
            assert 'DisableDelayedExpansion' in edit_bat and '"%~dp0plan.json"' in edit_bat
            assert r'%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' in start_bat
            assert r'%SystemRoot%\System32\notepad.exe' in edit_bat
            assert 'set "testExitCode=%errorlevel%"' in start_bat and 'exit /b %testExitCode%' in start_bat
            assert '-Mode Launch' not in start_bat and '-AllowPhysicalOutput' not in start_bat
            report_bat = (run / 'open-report.bat').read_text(encoding='ascii')
            assert 'DisableDelayedExpansion' in report_bat and '"%~dp0open-report.ps1"' in report_bat
            assert r'%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' in report_bat
            assert 'set "reportExitCode=%errorlevel%"' in report_bat and 'exit /b %reportExitCode%' in report_bat
            report_script = (run / 'open-report.ps1').read_text(encoding='utf-8-sig')
            assert "Join-Path $PSScriptRoot 'result/debug-report.html'" in report_script
            assert 'Test-Path -LiteralPath' in report_script and '$env:SSH_CONNECTION' in report_script
            missing_report = subprocess.run([shell, '-NoProfile', '-File', str(run / 'open-report.ps1')],
                                            capture_output=True, timeout=10)
            assert missing_report.returncode != 0
            assert '报告尚未生成' in missing_report.stderr.decode('utf-8-sig')
            assert not (run / 'result').exists()
            sampling_settings = read_generated_json(run / 'sampling-settings.json')
            assert sampling_settings == dict(max_move_speed=1.0, clean_shot_speed_ratio=0.34,
                accel_per_sec=5.5, natural_decel_per_sec=2.5, counter_strafe_accel_per_sec=14.0,
                fire_sample_delay_ms=18, tap_max_hold_ms=90, auto_fire_interval_ms=100, hud_enabled=True)
            assert '"%~dp0sampling-settings.json"' in (run / 'edit-sampling.bat').read_text(encoding='ascii')
            assert '// ' in (run / 'plan.json').read_text(encoding='utf-8-sig')
            assert '| shot_hold_ms | 5 |' in (run / 'PARAMETERS.md').read_text(encoding='utf-8-sig')
            assert task['status'] == 'PREPARED_NOT_LAUNCHED'
            assert task['schema_version'] == 4
            assert plan['schema_version'] == 2
            assert plan['capture_enabled'] is False
            assert 'shot_interval_ms' not in plan and 'brake_window_ms' not in plan
            assert task['executable'] == str(Path(args.executable).resolve())
            assert plan['fire_delay_ms'] == 300 and plan['fire_interval_ms'] == 0
            assert plan['move_during_fire_delay'] is True
            assert plan['counter_delay_ms'] == 50
            assert plan['counter_hold_ms'] == 5 and plan['shots'] == 20 and plan['move_ms'] == 300
            slow = root / 'slow-stationary'
            invoke('-Mode', 'Prepare', '-RunDirectory', slow, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'stationary', '-Shots', 7, '-FireDelayMs', 600, ok=True)
            slow_plan = read_generated_json(slow / 'plan.json')
            assert slow_plan['shots'] == 7 and slow_plan['fire_delay_ms'] == 600
            assert '600ms' in (slow / 'TASK.md').read_text(encoding='utf-8-sig')
            slower = root / 'slower-stationary'
            invoke('-Mode', 'Prepare', '-RunDirectory', slower, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'stationary', '-Shots', 7, '-FireDelayMs', 650, ok=True)
            assert read_generated_json(slower / 'plan.json')['fire_delay_ms'] == 650
            assert '650ms' in (slower / 'TASK.md').read_text(encoding='utf-8-sig')
            movement = root / 'matched-no-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', movement, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'no_counter', '-Shots', 7, '-FireDelayMs', 650,
                '-CounterDelayMs', 0, '-ShotAfterReleaseMs', 6, ok=True)
            movement_plan = read_generated_json(movement / 'plan.json')
            assert movement_plan['baseline'] == 'no_counter' and movement_plan['shot_after_release_ms'] == 6
            movement_task = (movement / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '6ms' in movement_task and '迟到超过5ms' in movement_task
            long_run = root / 'twenty-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 20, '-FireDelayMs', 650, '-CounterHoldMs', 25, ok=True)
            long_plan = read_generated_json(long_run / 'plan.json')
            assert long_plan['shots'] == 20 and long_plan['counter_hold_ms'] == 25
            # 复用只 Prepare，不触发任何设备。验证失败时上组证据必须完整保留。
            reusable_args = ('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                             '-ConfigPath', config, '-ReuseRunDirectory', '-Shots', 20,
                             '-FireDelayMs', 650, '-CounterHoldMs', 15, '-ShotAfterReleaseMs', 5)
            result_dir = long_run / 'result'
            result_dir.mkdir()
            (result_dir / 'result.json').write_text('previous-result', encoding='utf-8')
            (long_run / 'CONSUMED').write_text('consumed', encoding='utf-8')
            old_plan = (long_run / 'plan.json').read_bytes()
            old_task = (long_run / 'task.json').read_bytes()
            # 旧参数已移除，必须在清理前拒绝。
            invoke(*reusable_args[:-2], '-BrakeWindowMs', 10, '-ShotAfterReleaseMs', 0)
            assert (long_run / 'plan.json').read_bytes() == old_plan
            assert (long_run / 'task.json').read_bytes() == old_task
            assert (result_dir / 'result.json').read_text(encoding='utf-8-sig') == 'previous-result'
            assert (long_run / 'CONSUMED').exists()
            assert not list(long_run.glob('*.candidate.json'))
            if os.name == 'nt':
                import ctypes
                from ctypes import wintypes
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                               wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
                kernel.CreateFileW.restype = wintypes.HANDLE
                kernel.CloseHandle.argtypes = [wintypes.HANDLE]
                handle = kernel.CreateFileW(str(long_run / '.counterpulse.lock'), 0xC0000000, 0, None, 3, 0, None)
                assert handle != ctypes.c_void_p(-1).value
                try:
                    invoke(*reusable_args)
                    assert (long_run / 'plan.json').read_bytes() == old_plan
                    assert (result_dir / 'result.json').exists()
                finally:
                    kernel.CloseHandle(handle)
            if os.name == 'nt':
                external = root / 'external-evidence'
                external.mkdir()
                (external / 'keep.txt').write_text('external', encoding='utf-8')
                junction = result_dir / 'linked'
                escaped_link = str(junction).replace("'", "''")
                escaped_target = str(external).replace("'", "''")
                subprocess.run([shell, '-NoProfile', '-Command',
                    "New-Item -ItemType Junction -Path '" + escaped_link + "' -Target '" + escaped_target + "' | Out-Null"],
                    check=True, capture_output=True, timeout=20)
                try:
                    invoke(*reusable_args)
                    assert (external / 'keep.txt').read_text(encoding='utf-8-sig') == 'external'
                    assert (long_run / 'plan.json').read_bytes() == old_plan
                    assert (result_dir / 'result.json').exists()
                finally:
                    # 仅删除这个已核对的目录链接，不递归或遍历链接目标。
                    os.rmdir(junction)
                # 新报告入口也必须在覆盖前拒绝重解析点，不能写入外部目录。
                for leaf in ('open-report.bat', 'open-report.ps1', 'sampling-settings.json', 'edit-sampling.bat'):
                    shortcut_path = long_run / leaf
                    shortcut_bytes = shortcut_path.read_bytes()
                    shortcut_path.unlink()
                    escaped_shortcut = str(shortcut_path).replace("'", "''")
                    subprocess.run([shell, '-NoProfile', '-Command',
                        "New-Item -ItemType Junction -Path '" + escaped_shortcut + "' -Target '" + escaped_target + "' | Out-Null"],
                        check=True, capture_output=True, timeout=20)
                    try:
                        invoke(*reusable_args)
                        assert (long_run / 'plan.json').read_bytes() == old_plan
                        assert (result_dir / 'result.json').read_text(encoding='utf-8-sig') == 'previous-result'
                        assert list(external.iterdir()) == [external / 'keep.txt']
                    finally:
                        os.rmdir(shortcut_path)
                        shortcut_path.write_bytes(shortcut_bytes)
            invoke(*reusable_args, ok=True)
            assert not result_dir.exists() and not (long_run / 'CONSUMED').exists()
            updated = read_generated_json(long_run / 'plan.json')
            assert updated['counter_hold_ms'] == 15 and updated['shot_after_release_ms'] == 5
            assert '最后方向键UP ACK后5ms' in (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert 'ReuseRunDirectory' in (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            invoke(*reusable_args, ok=True)
            # schema4正式文件升级后允许显式Prepare重新绑定，日常Launch仍严格验哈希。
            upgraded_task = read_generated_json(long_run / 'task.json')
            upgraded_task['executable_sha256'] = 'PREVIOUS_EXECUTABLE'
            upgraded_task['script_sha256'] = 'PREVIOUS_SCRIPT'
            (long_run / 'task.json').write_text(json.dumps(upgraded_task), encoding='utf-8')
            invoke(*reusable_args, ok=True)
            rebound = read_generated_json(long_run / 'task.json')
            assert rebound['executable_sha256'] == hashlib.sha256(Path(args.executable).read_bytes()).hexdigest().upper()
            assert rebound['script_sha256'] == hashlib.sha256(script.read_bytes()).hexdigest().upper()
            # 原配置完整性仍是迁移门禁，失败必须保留计划。
            rebound['config_sha256'] = 'INVALID_CONFIG_HASH'
            (long_run / 'task.json').write_text(json.dumps(rebound), encoding='utf-8')
            kept = (long_run / 'plan.json').read_bytes()
            invoke(*reusable_args)
            assert (long_run / 'plan.json').read_bytes() == kept
            rebound['config_sha256'] = hashlib.sha256(config.read_bytes()).hexdigest().upper()
            (long_run / 'task.json').write_text(json.dumps(rebound), encoding='utf-8')
            foreign = root / 'foreign'
            foreign.mkdir()
            (foreign / 'keep.txt').write_text('keep', encoding='utf-8')
            invoke('-Mode', 'Prepare', '-RunDirectory', foreign, '-Executable', args.executable,
                   '-ConfigPath', config, '-ReuseRunDirectory')
            assert list(foreign.iterdir()) == [foreign / 'keep.txt']
            # schema 1 的已绑定目录可显式迁移；迁移仍要求原文件哈希匹配。
            legacy_task = read_generated_json(long_run / 'task.json')
            legacy_plan = read_generated_json(long_run / 'plan.json')
            legacy_plan.pop('schema_version')
            legacy_plan.pop('fire_interval_ms')
            legacy_plan.update(move_ms=5, fire_delay_ms=0, shot_interval_ms=280, brake_window_ms=60, shot_after_release_ms=5)
            (long_run / 'plan.json').write_text(json.dumps(legacy_plan), encoding='utf-8')
            legacy_task['plan_sha256'] = hashlib.sha256((long_run / 'plan.json').read_bytes()).hexdigest().upper()
            legacy_task['schema_version'] = 1
            legacy_task['executable_sha256'] = 'OLD_EXECUTABLE_HASH'
            legacy_task['script_sha256'] = 'OLD_SCRIPT_HASH'
            legacy_task.pop('owner')
            legacy_task.pop('run_directory')
            (long_run / 'task.json').write_text(json.dumps(legacy_task), encoding='utf-8')
            invoke('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                '-ConfigPath', config, '-ReuseRunDirectory', ok=True)
            inherited_plan = read_generated_json(long_run / 'plan.json')
            assert inherited_plan['move_ms'] == 5 and inherited_plan['fire_delay_ms'] == 0
            assert inherited_plan['shot_after_release_ms'] == 5 and inherited_plan['fire_interval_ms'] == 0
            assert 'shot_interval_ms' not in inherited_plan and 'brake_window_ms' not in inherited_plan
            assert read_generated_json(long_run / 'task.json')['schema_version'] == 4
            action_args = ('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                           '-ConfigPath', config, '-ReuseRunDirectory', '-Shots', 20,
                           '-FireDelayMs', 300, '-MoveMs', 500, '-CounterHoldMs', 15)
            invoke(*action_args, '-ShotAfterReleaseMs', 5, ok=True)
            action_plan = read_generated_json(long_run / 'plan.json')
            assert action_plan['capture_enabled'] is False
            assert 'shot_interval_ms' not in action_plan and action_plan['move_ms'] == 500
            assert action_plan['counter_hold_ms'] == 15 and action_plan['shot_after_release_ms'] == 5
            action_task = (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '默认不采集图像' in action_task and '图像逐帧保存' not in action_task
            assert 'fire_interval_ms=0ms' in action_task and '500ms' in action_task
            assert '间隔0ms' not in action_task and '枪间至少0ms' not in action_task
            diagnostic = root / 'disabled-capture-must-not-exist'
            rejected = subprocess.run([str(Path(args.executable).resolve()), '--capture-check',
                '--plan', str(long_run / 'plan.json'), '--config', str(config), '--output', str(diagnostic)],
                capture_output=True, timeout=10)
            assert rejected.returncode != 0 and not diagnostic.exists(), '关闭采集的计划不可进入采集诊断'
            valid_action_plan = (long_run / 'plan.json').read_bytes()
            for removed in [('-ShotIntervalMs', 0), ('-BrakeWindowMs', 60), ('-NoCapture',)]:
                invoke(*action_args, *removed)
                assert (long_run / 'plan.json').read_bytes() == valid_action_plan
            invoke(*action_args, '-ShotAfterReleaseMs', 0, ok=True)
            assert read_generated_json(long_run / 'plan.json')['shot_after_release_ms'] == 0
            overflow = root / 'overflow'
            invoke('-Mode', 'Prepare', '-RunDirectory', overflow, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 31, '-FireDelayMs', 650)
            assert not overflow.exists()
            held_run = root / 'long-fire-hold'
            invoke('-Mode', 'Prepare', '-RunDirectory', held_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 3, '-ShotHoldMs', 1000, '-FireIntervalMs', 2000, ok=True)
            held_plan = read_generated_json(held_run / 'plan.json')
            user_settings = read_generated_json(held_run / 'sampling-settings.json')
            user_settings['accel_per_sec'] = 6.0
            (held_run / 'sampling-settings.json').write_text(json.dumps(user_settings), encoding='utf-8')
            user_settings_bytes = (held_run / 'sampling-settings.json').read_bytes()
            assert held_plan['shot_hold_ms'] == 1000 and held_plan['fire_interval_ms'] == 2000
            assert 'restart_interval_ms' not in held_plan, '旧默认计划保持原形，不加入零值字段'
            held_task = (held_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '左键按住1000ms' in held_task and 'fire_interval_ms=2000ms' in held_task
            assert '不是精确周期' in held_task and '下一轮移动开始前' in held_task
            invoke('-Mode', 'Prepare', '-RunDirectory', held_run, '-Executable', args.executable,
                '-ConfigPath', config, '-ReuseRunDirectory', '-MoveMs', 10, ok=True)
            held_migrated = read_generated_json(held_run / 'plan.json')
            assert held_migrated['shot_hold_ms'] == 1000 and held_migrated['fire_interval_ms'] == 2000
            assert held_migrated['move_ms'] == 10
            assert 'restart_interval_ms' not in held_migrated, '复用旧计划缺省间隔必须为0'
            assert (held_run / 'sampling-settings.json').read_bytes() == user_settings_bytes
            parameters = (held_run / 'PARAMETERS.md').read_text(encoding='utf-8-sig')
            assert '| shot_hold_ms | 1000 |' in parameters and '| fire_interval_ms | 2000 |' in parameters
            assert '| move_ms | 10 |' in parameters
            for baseline in ('no_counter', 'stationary'):
                invalid_restart = root / ('invalid-restart-' + baseline)
                invoke('-Mode', 'Prepare', '-RunDirectory', invalid_restart, '-Executable', args.executable,
                    '-ConfigPath', config, '-Baseline', baseline, '-RestartIntervalMs', 100)
                assert not (invalid_restart / 'TASK.md').exists(), '非反向基线不能准备非零重新起步间隔'
            for index, extra in enumerate([('-ShotHoldMs', 0), ('-ShotHoldMs', 2001), ('-FireIntervalMs', -1), ('-FireIntervalMs', 5001),
                                          ('-RestartIntervalMs', -1), ('-RestartIntervalMs', 2001)]):
                invalid_hold = root / ('invalid-hold-' + str(index))
                invoke('-Mode', 'Prepare', '-RunDirectory', invalid_hold, '-Executable', args.executable,
                    '-ConfigPath', config, *extra)
                assert not invalid_hold.exists()
            single = root / 'single-shot'
            invoke('-Mode', 'Prepare', '-RunDirectory', single, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 1, '-FireDelayMs', 300,
                '-MoveMs', 300, '-CounterDelayMs', 50, '-CounterHoldMs', 5, '-ShotAfterReleaseMs', 0, ok=True)
            assert read_generated_json(single / 'plan.json')['shots'] == 1
            single_task = (single / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '上一轮左键UP ACK后开始300ms间隔' in single_task and '较晚者' in single_task
            assert 'UP ACK后等待50ms' in single_task and '反向轻点5ms' in single_task
            assert 'UP ACK后立即计划开枪' in single_task
            for index, extra in enumerate([('-ShotIntervalMs', 0), ('-BrakeWindowMs', 60), ('-NoCapture',)]):
                incompatible = root / ('obsolete-parameter-' + str(index))
                invoke('-Mode', 'Prepare', '-RunDirectory', incompatible, '-Executable', args.executable,
                    '-ConfigPath', config, *extra)
                assert not incompatible.exists()
            text = (run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert text.count('-Mode Launch') == 1 and '-AllowPhysicalOutput' in text
            invoke('-Mode', 'Prepare', '-RunDirectory', run, '-Executable', args.executable, '-ConfigPath', config)
            (run / 'plan.json').write_text('{}', encoding='utf-8')
            # 将绑定程序替成PowerShell本身，门禁发生回归也绝不运行设备探针。
            task['executable'] = str(Path(shell).resolve())
            task['executable_sha256'] = hashlib.sha256(Path(shell).read_bytes()).hexdigest().upper()
            (run / 'task.json').write_text(json.dumps(task), encoding='utf-8')
            invoke('-Mode', 'Launch', '-RunDirectory', run, '-AllowPhysicalOutput', '-Confirm', 'AUTO_STOP_COUNTERPULSE')
            assert not (run / 'CONSUMED').exists() and not (run / 'result').exists()
            task['plan_sha256'] = hashlib.sha256((run / 'plan.json').read_bytes()).hexdigest().upper()
            (run / 'task.json').write_text(json.dumps(task), encoding='utf-8')
            (run / 'CONSUMED').write_text('already-consumed', encoding='utf-8')
            invoke('-Mode', 'Launch', '-RunDirectory', run, '-AllowPhysicalOutput', '-Confirm', 'AUTO_STOP_COUNTERPULSE')
            assert (run / 'CONSUMED').read_text(encoding='utf-8') == 'already-consumed'
            assert not (run / 'result').exists()
            # AST仅改测试副本：物理分支完全替为写文件桩，真实exe只接收--dry-run。
            harness = root / 'invoke_auto_stop_counterpulse-r99.ps1'
            source = script.read_bytes().decode('utf-8-sig')
            ast_query = "$t=$null;$e=$null;$a=[System.Management.Automation.Language.Parser]::ParseFile('" + str(script).replace("'", "''") + "',[ref]$t,[ref]$e);$a.FindAll({param($n) ($n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -in @('Invoke-Probe','Invoke-ReportHud')) -or ($n -is [System.Management.Automation.Language.IfStatementAst] -and $n.Clauses[0].Item1.Extent.Text.Contains(\"SessionId\"))},$true) | ForEach-Object { @{start=$_.Extent.StartOffset;end=$_.Extent.EndOffset;kind=$_.GetType().Name;name=$(if($_ -is [System.Management.Automation.Language.FunctionDefinitionAst]){$_.Name}else{''})} } | ConvertTo-Json"
            spans = json.loads(subprocess.run([shell, '-NoProfile', '-Command', ast_query],
                capture_output=True, check=True, timeout=20).stdout.decode('utf-8-sig'))
            assert len(spans) == 3
            mock = r'''function Invoke-Probe([string]$Binary, [string[]]$Arguments, [bool]$Physical) {
    if (-not $Physical) {
        $dry = $Arguments.Length -eq 6 -and $Arguments[0] -eq '--plan' -and $Arguments[2] -eq '--dry-run' -and $Arguments[3] -eq '--require-current-plan' -and $Arguments[4] -eq '--sampling-settings'
        $migration = $Arguments.Length -eq 4 -and $Arguments[0] -eq '--migrate-plan' -and $Arguments[2] -eq '--output'
        if (-not $dry -and -not $migration) { throw 'MOCK_REJECTED_ARGUMENTS' }
        & $Binary @Arguments > $null
        if ($LASTEXITCODE -ne 0) { throw 'MOCK_DRY_RUN_FAILED' }
        return
    }
    $snapshot = $Arguments[[Array]::IndexOf($Arguments, '--plan') + 1]
    $settingsSnapshot = $Arguments[[Array]::IndexOf($Arguments, '--sampling-settings') + 1]
    $output = $Arguments[[Array]::IndexOf($Arguments, '--output') + 1]
    if ((Test-Path -LiteralPath (Join-Path $runPath 'edit-during-mock'))) {
        $edited = (Get-Content -LiteralPath $planPath -Encoding UTF8 | Where-Object { -not $_.TrimStart().StartsWith('//') }) -join "`n" | ConvertFrom-Json
        $edited.move_ms = 123
        Write-Json $planPath $edited
        $editedSettings = (Get-Content -LiteralPath $settingsSnapshot -Encoding UTF8 | Where-Object { -not $_.TrimStart().StartsWith('//') }) -join "`n" | ConvertFrom-Json
        $editedSettings.accel_per_sec = 7.0
        Write-Json $samplingPath $editedSettings
    }
    $null = [IO.Directory]::CreateDirectory($output)
    [IO.File]::WriteAllBytes((Join-Path $output 'plan.json'), [IO.File]::ReadAllBytes($snapshot))
    [IO.File]::WriteAllBytes((Join-Path $output 'sampling-settings.json'), [IO.File]::ReadAllBytes($settingsSnapshot))
    [IO.File]::WriteAllText((Join-Path $output 'result.json'), 'MOCK_ONLY')
    [IO.File]::AppendAllText((Join-Path $runPath 'mock-calls'), "once`n")
}'''
            # AST偏移包括文件BOM；ParseFile视BOM为编码头，不计入源码偏移。
            for span in sorted(spans, key=lambda item: item['start'], reverse=True):
                if span['name'] == 'Invoke-Probe':
                    replacement = mock
                elif span['name'] == 'Invoke-ReportHud':
                    replacement = "function Invoke-ReportHud([string]$Binary, [string]$Report) { [IO.File]::AppendAllText((Join-Path $runPath 'mock-hud-calls'), 'once') }"
                else:
                    replacement = 'if ($false) { throw "MOCK_SESSION_ONLY" }'
                source = source[:span['start']] + replacement + source[span['end']:]
            harness.write_bytes(source.encode('utf-8-sig'))
            repeat_run = root / "repeatable % ! & ' 中文"
            invoke('-Mode', 'Prepare', '-RunDirectory', repeat_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Repeatable', '-MoveMs', 300, '-CounterDelayMs', 50, '-CounterHoldMs', 5,
                '-ShotAfterReleaseMs', 0, '-FireDelayMs', 300, '-Shots', 20, entry=harness, ok=True)
            repeat_task = read_generated_json(repeat_run / 'task.json')
            assert repeat_task['schema_version'] == 4 and repeat_task['repeatable'] is True
            launch = ('-Mode', 'Launch', '-RunDirectory', repeat_run,
                      '-AllowPhysicalOutput', '-Confirm', 'AUTO_STOP_COUNTERPULSE')
            # 旧绑定即使所有哈希一致也不得Launch；测试物理分支已完全替成桩。
            for old_schema in (1, 2, 3):
                old_binding = dict(repeat_task, schema_version=old_schema)
                (repeat_run / 'task.json').write_text(json.dumps(old_binding), encoding='utf-8')
                denied = invoke(*launch, entry=harness)
                assert b'LEGACY_RUN_REQUIRES_PREPARE' in denied.stderr
                assert not (repeat_run / 'mock-calls').exists()
                assert not (repeat_run / 'result').exists() and not (repeat_run / 'CONSUMED').exists()
            (repeat_run / 'task.json').write_text(json.dumps(repeat_task), encoding='utf-8')
            shortcut = subprocess.run([shell, '-NoProfile', '-File', str(repeat_run / 'launch-test.ps1')],
                                      capture_output=True, timeout=30)
            assert shortcut.returncode == 0, '带特殊字符路径的生成PS快捷入口须可调用物理桩'

            repeat_plan = read_generated_json(repeat_run / 'plan.json')
            repeat_plan['move_ms'] = 200
            repeat_plan['counter_hold_ms'] = 40
            repeat_plan['counter_delay_ms'] = 60
            repeat_plan['fire_delay_ms'] = 700
            repeat_plan['shots'] = 30
            (repeat_run / 'plan.json').write_text(json.dumps(repeat_plan), encoding='utf-8')
            (repeat_run / 'edit-during-mock').write_text('edit', encoding='utf-8')
            invoke(*launch, entry=harness, ok=True)
            executed = read_generated_json(repeat_run / 'result' / 'plan.json')
            assert executed['move_ms'] == 200 and executed['counter_hold_ms'] == 40
            assert executed['fire_delay_ms'] == 700 and executed['shots'] == 30
            assert executed['counter_delay_ms'] == 60 and executed['shot_after_release_ms'] == 0
            assert read_generated_json(repeat_run / 'plan.json')['move_ms'] == 123
            assert (repeat_run / 'execution-plan.json').read_bytes() == (repeat_run / 'result' / 'plan.json').read_bytes()
            assert (repeat_run / 'mock-calls').read_text(encoding='utf-8-sig').splitlines() == ['once', 'once']
            assert read_generated_json(repeat_run / 'sampling-settings.json')['accel_per_sec'] == 7.0
            assert read_generated_json(repeat_run / 'result' / 'sampling-settings.json')['accel_per_sec'] == 5.5
            assert (repeat_run / 'execution-sampling-settings.json').read_bytes() == (repeat_run / 'result' / 'sampling-settings.json').read_bytes()
            previous = (repeat_run / 'result' / 'plan.json').read_bytes()
            valid_settings_bytes = (repeat_run / 'sampling-settings.json').read_bytes()
            (repeat_run / 'sampling-settings.json').write_text('{"auto_fire_interval_ms":0}', encoding='utf-8')
            denied_settings = invoke(*launch, entry=harness)
            assert b'PLAN_VALIDATION_FAILED' in denied_settings.stderr
            assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
            assert (repeat_run / 'mock-calls').read_text(encoding='utf-8-sig').splitlines() == ['once', 'once']
            assert not list(repeat_run.glob('*.candidate.json'))
            (repeat_run / 'sampling-settings.json').write_bytes(valid_settings_bytes)
            for invalid in [json.dumps(dict(repeat_plan, **bad)) for bad in [
                    {'move_ms': 0}, {'shots': 0}, {'fire_delay_ms': 2001}, {'counter_delay_ms': 201},
                    {'shot_interval_ms': 280}, {'brake_window_ms': 60}, {'schema_version': 1},
                    {'shot_hold_ms': 0}, {'shot_hold_ms': 2001}, {'fire_interval_ms': -1}, {'fire_interval_ms': 5001}]] + ['{"shots":3}', ' ' * 16385]:
                (repeat_run / 'plan.json').write_text(invalid, encoding='utf-8')
                rejected = invoke(*launch, entry=harness)
                assert b'PLAN_VALIDATION_FAILED' in rejected.stderr
                assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
                assert (repeat_run / 'CONSUMED').exists()
                assert (repeat_run / 'mock-calls').read_text(encoding='utf-8-sig').splitlines() == ['once', 'once']
            (repeat_run / 'plan.json').write_text(json.dumps(repeat_plan), encoding='utf-8')
            live_task = dict(repeat_task)
            live_task['executable'] = str(Path(shell).resolve())
            live_task['executable_sha256'] = hashlib.sha256(Path(shell).read_bytes()).hexdigest().upper()
            (repeat_run / 'task.json').write_text(json.dumps(live_task), encoding='utf-8')
            invoke(*launch, entry=harness)
            assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
            assert (repeat_run / 'mock-calls').read_text(encoding='utf-8-sig').splitlines() == ['once', 'once']
            (repeat_run / 'task.json').write_text(json.dumps(repeat_task), encoding='utf-8')
            if os.name == 'nt':
                handle = kernel.CreateFileW(str(repeat_run / '.counterpulse.lock'), 0xC0000000, 0, None, 3, 0, None)
                assert handle != ctypes.c_void_p(-1).value
                try:
                    invoke(*launch, entry=harness)
                    assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
                finally:
                    kernel.CloseHandle(handle)
    print('反向轻点CLI无设备回归通过')


if __name__ == '__main__':
    main()
