"""反向轻点入口的无设备回归；绝不传递有效物理授权。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


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
            # 正式离线入口复用生产Session；全部输入为合成命令回执，绝不连接设备。
            command_report = root / 'synthetic-result.json'
            commands = []
            for kind, value, stamp in [('wasd', 0, 1000000), ('left_button', 0, 2000000),
                    ('wasd', 2, 3000000), ('wasd', 0, 10000000), ('wasd', 8, 11000000),
                    ('wasd', 0, 16000000), ('left_button', 1, 17000000), ('left_button', 0, 22000000)]:
                commands.append(dict(kind=kind, value=value, disposition=2, submit_ns=stamp - 1,
                    ack_received_ns=stamp, backend_completed_ns=stamp + 1, returned_ns=stamp + 2))
            command_report.write_text(json.dumps({'commands': commands}), encoding='utf-8')
            evaluation_dir = root / 'evaluation'
            evaluated = subprocess.run([str(Path(args.executable).resolve()), '--evaluate-result',
                str(command_report), '--output', str(evaluation_dir)], capture_output=True, timeout=10)
            assert evaluated.returncode == 0, '正式离线重评必须通过'
            evaluation = json.loads((evaluation_dir / 'training-evaluation.json').read_text(encoding='utf-8-sig'))
            assert evaluation['physical_output'] is False and evaluation['monitor'] is None
            ack = evaluation['command_ack']
            assert ack['source'] == 'COMMAND_ACK' and ack['time_domain'] == 'LOCAL_STEADY_COMMAND_ACK'
            assert ack['game_shot_stability'] is None and ack['settled'] is None
            assert ack['physical_validation_passed'] is False and ack['invalid_receipts'] == 0
            assert ack['total_timings'] == 1 and ack['timings'][0]['delta_ns'] == 1000000
            assert ack['timings'][0]['grade'] == '完美' and ack['total_holds'] == 1, (ack['timings'], ack['total_holds'])
            assert (evaluation_dir / 'command-training' / 'manifest.txt').exists()
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
            task = json.loads((run / 'task.json').read_text(encoding='utf-8-sig'))
            plan = json.loads((run / 'plan.json').read_text(encoding='utf-8-sig'))
            # 纯采集模式不允许混入物理授权；拒绝发生在加载配置与设备之前。
            for extra in [('--allow-physical-output',), ('--confirm', 'AUTO_STOP_COUNTERPULSE'), ('--dry-run',)]:
                diagnostic = root / 'capture-must-not-exist'
                rejected = subprocess.run([str(Path(args.executable).resolve()), '--capture-check',
                    '--plan', str(run / 'plan.json'), '--config', str(config), '--output', str(diagnostic), *extra],
                    capture_output=True, timeout=10)
                assert rejected.returncode != 0 and not diagnostic.exists()
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
            slow_plan = json.loads((slow / 'plan.json').read_text(encoding='utf-8-sig'))
            assert slow_plan['shots'] == 7 and slow_plan['fire_delay_ms'] == 600
            assert '600ms' in (slow / 'TASK.md').read_text(encoding='utf-8-sig')
            slower = root / 'slower-stationary'
            invoke('-Mode', 'Prepare', '-RunDirectory', slower, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'stationary', '-Shots', 7, '-FireDelayMs', 650, ok=True)
            assert json.loads((slower / 'plan.json').read_text(encoding='utf-8-sig'))['fire_delay_ms'] == 650
            assert '650ms' in (slower / 'TASK.md').read_text(encoding='utf-8-sig')
            movement = root / 'matched-no-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', movement, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'no_counter', '-Shots', 7, '-FireDelayMs', 650,
                '-CounterDelayMs', 0, '-ShotAfterReleaseMs', 6, ok=True)
            movement_plan = json.loads((movement / 'plan.json').read_text(encoding='utf-8-sig'))
            assert movement_plan['baseline'] == 'no_counter' and movement_plan['shot_after_release_ms'] == 6
            movement_task = (movement / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '6ms' in movement_task and '迟到超过5ms' in movement_task
            long_run = root / 'twenty-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 20, '-FireDelayMs', 650, '-CounterHoldMs', 25, ok=True)
            long_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
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
            invoke(*reusable_args, ok=True)
            assert not result_dir.exists() and not (long_run / 'CONSUMED').exists()
            updated = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert updated['counter_hold_ms'] == 15 and updated['shot_after_release_ms'] == 5
            assert '最后方向键UP ACK后5ms' in (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert 'ReuseRunDirectory' in (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            invoke(*reusable_args, ok=True)
            # schema4正式文件升级后允许显式Prepare重新绑定，日常Launch仍严格验哈希。
            upgraded_task = json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))
            upgraded_task['executable_sha256'] = 'PREVIOUS_EXECUTABLE'
            upgraded_task['script_sha256'] = 'PREVIOUS_SCRIPT'
            (long_run / 'task.json').write_text(json.dumps(upgraded_task), encoding='utf-8')
            invoke(*reusable_args, ok=True)
            rebound = json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))
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
            legacy_task = json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))
            legacy_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
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
            inherited_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert inherited_plan['move_ms'] == 5 and inherited_plan['fire_delay_ms'] == 0
            assert inherited_plan['shot_after_release_ms'] == 5 and inherited_plan['fire_interval_ms'] == 0
            assert 'shot_interval_ms' not in inherited_plan and 'brake_window_ms' not in inherited_plan
            assert json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))['schema_version'] == 4
            action_args = ('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                           '-ConfigPath', config, '-ReuseRunDirectory', '-Shots', 20,
                           '-FireDelayMs', 300, '-MoveMs', 500, '-CounterHoldMs', 15)
            invoke(*action_args, '-ShotAfterReleaseMs', 5, ok=True)
            action_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
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
            assert json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))['shot_after_release_ms'] == 0
            overflow = root / 'overflow'
            invoke('-Mode', 'Prepare', '-RunDirectory', overflow, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 31, '-FireDelayMs', 650)
            assert not overflow.exists()
            held_run = root / 'long-fire-hold'
            invoke('-Mode', 'Prepare', '-RunDirectory', held_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 3, '-ShotHoldMs', 1000, '-FireIntervalMs', 2000, ok=True)
            held_plan = json.loads((held_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert held_plan['shot_hold_ms'] == 1000 and held_plan['fire_interval_ms'] == 2000
            held_task = (held_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '左键按住1000ms' in held_task and 'fire_interval_ms=2000ms' in held_task
            assert '不是精确周期' in held_task and '下一轮移动开始前' in held_task
            invoke('-Mode', 'Prepare', '-RunDirectory', held_run, '-Executable', args.executable,
                '-ConfigPath', config, '-ReuseRunDirectory', '-MoveMs', 10, ok=True)
            held_migrated = json.loads((held_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert held_migrated['shot_hold_ms'] == 1000 and held_migrated['fire_interval_ms'] == 2000
            assert held_migrated['move_ms'] == 10
            for index, extra in enumerate([('-ShotHoldMs', 0), ('-ShotHoldMs', 2001), ('-FireIntervalMs', -1), ('-FireIntervalMs', 5001)]):
                invalid_hold = root / ('invalid-hold-' + str(index))
                invoke('-Mode', 'Prepare', '-RunDirectory', invalid_hold, '-Executable', args.executable,
                    '-ConfigPath', config, *extra)
                assert not invalid_hold.exists()
            single = root / 'single-shot'
            invoke('-Mode', 'Prepare', '-RunDirectory', single, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 1, '-FireDelayMs', 300,
                '-MoveMs', 300, '-CounterDelayMs', 50, '-CounterHoldMs', 5, '-ShotAfterReleaseMs', 0, ok=True)
            assert json.loads((single / 'plan.json').read_text(encoding='utf-8-sig'))['shots'] == 1
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
            ast_query = "$t=$null;$e=$null;$a=[System.Management.Automation.Language.Parser]::ParseFile('" + str(script).replace("'", "''") + "',[ref]$t,[ref]$e);$a.FindAll({param($n) ($n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-Probe') -or ($n -is [System.Management.Automation.Language.IfStatementAst] -and $n.Clauses[0].Item1.Extent.Text.Contains(\"SessionId\"))},$true) | ForEach-Object { @{start=$_.Extent.StartOffset;end=$_.Extent.EndOffset;kind=$_.GetType().Name;name=$(if($_ -is [System.Management.Automation.Language.FunctionDefinitionAst]){$_.Name}else{''})} } | ConvertTo-Json"
            spans = json.loads(subprocess.run([shell, '-NoProfile', '-Command', ast_query],
                capture_output=True, check=True, timeout=20).stdout.decode('utf-8-sig'))
            assert len(spans) == 2
            mock = r'''function Invoke-Probe([string]$Binary, [string[]]$Arguments, [bool]$Physical) {
    if (-not $Physical) {
        $dry = $Arguments.Length -eq 4 -and $Arguments[0] -eq '--plan' -and $Arguments[2] -eq '--dry-run' -and $Arguments[3] -eq '--require-current-plan'
        $migration = $Arguments.Length -eq 4 -and $Arguments[0] -eq '--migrate-plan' -and $Arguments[2] -eq '--output'
        if (-not $dry -and -not $migration) { throw 'MOCK_REJECTED_ARGUMENTS' }
        & $Binary @Arguments > $null
        if ($LASTEXITCODE -ne 0) { throw 'MOCK_DRY_RUN_FAILED' }
        return
    }
    $snapshot = $Arguments[[Array]::IndexOf($Arguments, '--plan') + 1]
    $output = $Arguments[[Array]::IndexOf($Arguments, '--output') + 1]
    if ((Test-Path -LiteralPath (Join-Path $runPath 'edit-during-mock'))) {
        $edited = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
        $edited.move_ms = 123
        Write-Json $planPath $edited
    }
    $null = [IO.Directory]::CreateDirectory($output)
    [IO.File]::WriteAllBytes((Join-Path $output 'plan.json'), [IO.File]::ReadAllBytes($snapshot))
    [IO.File]::WriteAllText((Join-Path $output 'result.json'), 'MOCK_ONLY')
    [IO.File]::AppendAllText((Join-Path $runPath 'mock-calls'), "once`n")
}'''
            # AST偏移包括文件BOM；ParseFile视BOM为编码头，不计入源码偏移。
            for span in sorted(spans, key=lambda item: item['start'], reverse=True):
                if span['name'] == 'Invoke-Probe':
                    replacement = mock
                else:
                    replacement = 'if ($false) { throw "MOCK_SESSION_ONLY" }'
                source = source[:span['start']] + replacement + source[span['end']:]
            harness.write_bytes(source.encode('utf-8-sig'))
            repeat_run = root / 'repeatable'
            invoke('-Mode', 'Prepare', '-RunDirectory', repeat_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Repeatable', '-MoveMs', 300, '-CounterDelayMs', 50, '-CounterHoldMs', 5,
                '-ShotAfterReleaseMs', 0, '-FireDelayMs', 300, '-Shots', 20, entry=harness, ok=True)
            repeat_task = json.loads((repeat_run / 'task.json').read_text(encoding='utf-8-sig'))
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
            invoke(*launch, entry=harness, ok=True)
            repeat_plan = json.loads((repeat_run / 'plan.json').read_text(encoding='utf-8-sig'))
            repeat_plan['move_ms'] = 200
            repeat_plan['counter_hold_ms'] = 40
            repeat_plan['counter_delay_ms'] = 60
            repeat_plan['fire_delay_ms'] = 700
            repeat_plan['shots'] = 30
            (repeat_run / 'plan.json').write_text(json.dumps(repeat_plan), encoding='utf-8')
            (repeat_run / 'edit-during-mock').write_text('edit', encoding='utf-8')
            invoke(*launch, entry=harness, ok=True)
            executed = json.loads((repeat_run / 'result' / 'plan.json').read_text(encoding='utf-8-sig'))
            assert executed['move_ms'] == 200 and executed['counter_hold_ms'] == 40
            assert executed['fire_delay_ms'] == 700 and executed['shots'] == 30
            assert executed['counter_delay_ms'] == 60 and executed['shot_after_release_ms'] == 0
            assert json.loads((repeat_run / 'plan.json').read_text(encoding='utf-8-sig'))['move_ms'] == 123
            assert (repeat_run / 'execution-plan.json').read_bytes() == (repeat_run / 'result' / 'plan.json').read_bytes()
            assert (repeat_run / 'mock-calls').read_text(encoding='utf-8-sig').splitlines() == ['once', 'once']
            previous = (repeat_run / 'result' / 'plan.json').read_bytes()
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
