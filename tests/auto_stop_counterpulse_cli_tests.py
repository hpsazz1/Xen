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
            commented = root / 'commented-plan.json'
            commented.write_text('{\n  "shots": 3, // 子弹数\n  "capture_enabled": false, /* 人工观察 */\n'
                                 '  "shot_interval_ms": 0, "fire_delay_ms": 300,\n'
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
            assert plan['capture_enabled'] is True
            assert plan['fire_delay_ms'] == 0
            assert plan['counter_delay_ms'] == 0
            assert plan['counter_hold_ms'] == 30 and plan['shot_interval_ms'] == 280
            slow = root / 'slow-stationary'
            invoke('-Mode', 'Prepare', '-RunDirectory', slow, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'stationary', '-Shots', 7, '-ShotIntervalMs', 600, ok=True)
            slow_plan = json.loads((slow / 'plan.json').read_text(encoding='utf-8-sig'))
            assert slow_plan['shots'] == 7 and slow_plan['shot_interval_ms'] == 600
            assert '600ms' in (slow / 'TASK.md').read_text(encoding='utf-8-sig')
            slower = root / 'slower-stationary'
            invoke('-Mode', 'Prepare', '-RunDirectory', slower, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'stationary', '-Shots', 7, '-ShotIntervalMs', 650, ok=True)
            assert json.loads((slower / 'plan.json').read_text(encoding='utf-8-sig'))['shot_interval_ms'] == 650
            assert '650ms' in (slower / 'TASK.md').read_text(encoding='utf-8-sig')
            movement = root / 'matched-no-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', movement, '-Executable', args.executable,
                '-ConfigPath', config, '-Baseline', 'no_counter', '-Shots', 7, '-ShotIntervalMs', 650,
                '-BrakeWindowMs', 60, ok=True)
            movement_plan = json.loads((movement / 'plan.json').read_text(encoding='utf-8-sig'))
            assert movement_plan['baseline'] == 'no_counter' and movement_plan['brake_window_ms'] == 60
            movement_task = (movement / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '60ms' in movement_task and '迟到超过5ms' in movement_task
            long_run = root / 'twenty-counter'
            invoke('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 20, '-ShotIntervalMs', 650, '-CounterHoldMs', 25, ok=True)
            long_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert long_plan['shots'] == 20 and long_plan['counter_hold_ms'] == 25
            # 复用只 Prepare，不触发任何设备。验证失败时上组证据必须完整保留。
            reusable_args = ('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                             '-ConfigPath', config, '-ReuseRunDirectory', '-Shots', 20,
                             '-ShotIntervalMs', 650, '-CounterHoldMs', 15, '-ShotAfterReleaseMs', 5)
            result_dir = long_run / 'result'
            result_dir.mkdir()
            (result_dir / 'result.json').write_text('previous-result', encoding='utf-8')
            (long_run / 'CONSUMED').write_text('consumed', encoding='utf-8')
            old_plan = (long_run / 'plan.json').read_bytes()
            old_task = (long_run / 'task.json').read_bytes()
            # 旧固定窗口模式下反向保持不小于窗口，必须在清理前由 dry-run 拒绝。
            invoke(*reusable_args[:-2], '-BrakeWindowMs', 10, '-ShotAfterReleaseMs', 0)
            assert (long_run / 'plan.json').read_bytes() == old_plan
            assert (long_run / 'task.json').read_bytes() == old_task
            assert (result_dir / 'result.json').read_text() == 'previous-result'
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
                    assert (external / 'keep.txt').read_text() == 'external'
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
            foreign = root / 'foreign'
            foreign.mkdir()
            (foreign / 'keep.txt').write_text('keep', encoding='utf-8')
            invoke('-Mode', 'Prepare', '-RunDirectory', foreign, '-Executable', args.executable,
                   '-ConfigPath', config, '-ReuseRunDirectory')
            assert list(foreign.iterdir()) == [foreign / 'keep.txt']
            # schema 1 的已绑定目录可显式迁移；迁移仍要求原文件哈希匹配。
            legacy_task = json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))
            legacy_task['schema_version'] = 1
            legacy_task.pop('owner')
            legacy_task.pop('run_directory')
            (long_run / 'task.json').write_text(json.dumps(legacy_task), encoding='utf-8')
            invoke(*reusable_args, ok=True)
            assert json.loads((long_run / 'task.json').read_text(encoding='utf-8-sig'))['schema_version'] == 2
            action_args = ('-Mode', 'Prepare', '-RunDirectory', long_run, '-Executable', args.executable,
                           '-ConfigPath', config, '-ReuseRunDirectory', '-Shots', 20,
                           '-ShotIntervalMs', 0, '-MoveMs', 500, '-CounterHoldMs', 15, '-NoCapture')
            invoke(*action_args, '-ShotAfterReleaseMs', 5, ok=True)
            action_plan = json.loads((long_run / 'plan.json').read_text(encoding='utf-8-sig'))
            assert action_plan['capture_enabled'] is False
            assert action_plan['shot_interval_ms'] == 0 and action_plan['move_ms'] == 500
            assert action_plan['counter_hold_ms'] == 15 and action_plan['shot_after_release_ms'] == 5
            action_task = (long_run / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '不采集图像，以人工观察判断' in action_task and '图像逐帧保存' not in action_task
            assert '不设最小枪间隔' in action_task and '500ms' in action_task
            assert '间隔0ms' not in action_task and '枪间至少0ms' not in action_task
            diagnostic = root / 'disabled-capture-must-not-exist'
            rejected = subprocess.run([str(Path(args.executable).resolve()), '--capture-check',
                '--plan', str(long_run / 'plan.json'), '--config', str(config), '--output', str(diagnostic)],
                capture_output=True, timeout=10)
            assert rejected.returncode != 0 and not diagnostic.exists(), '关闭采集的计划不可进入采集诊断'
            valid_action_plan = (long_run / 'plan.json').read_bytes()
            invoke(*action_args, '-ShotAfterReleaseMs', 5, '-Baseline', 'stationary')
            assert (long_run / 'plan.json').read_bytes() == valid_action_plan
            invoke(*action_args, '-ShotAfterReleaseMs', 0)
            assert (long_run / 'plan.json').read_bytes() == valid_action_plan
            overflow = root / 'overflow'
            invoke('-Mode', 'Prepare', '-RunDirectory', overflow, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 31, '-ShotIntervalMs', 650)
            assert not overflow.exists()
            single = root / 'single-shot'
            invoke('-Mode', 'Prepare', '-RunDirectory', single, '-Executable', args.executable,
                '-ConfigPath', config, '-Shots', 1, '-ShotIntervalMs', 0, '-FireDelayMs', 300,
                '-MoveMs', 300, '-CounterDelayMs', 50, '-CounterHoldMs', 5, '-ShotAfterReleaseMs', 0, '-NoCapture', ok=True)
            assert json.loads((single / 'plan.json').read_text())['shots'] == 1
            single_task = (single / 'TASK.md').read_text(encoding='utf-8-sig')
            assert '上一枪左键UP ACK后开始300ms间隔' in single_task and '较晚者' in single_task
            assert 'UP ACK后等待50ms' in single_task and '反向轻点5ms' in single_task
            assert 'UP ACK后立即计划开枪' in single_task
            for index, extra in enumerate([('-Baseline', 'stationary', '-ShotIntervalMs', 0, '-ShotAfterReleaseMs', 5),
                          ('-ShotIntervalMs', 650, '-ShotAfterReleaseMs', 5),
                          ('-Baseline', 'no_counter', '-ShotIntervalMs', 0, '-ShotAfterReleaseMs', 0, '-CounterDelayMs', 50)]):
                incompatible = root / ('incompatible-fire-delay-' + str(index))
                invoke('-Mode', 'Prepare', '-RunDirectory', incompatible, '-Executable', args.executable,
                    '-ConfigPath', config, '-FireDelayMs', 300, '-NoCapture', *extra)
                assert not (incompatible / 'task.json').exists()
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
        if ($Arguments.Length -ne 3 -or $Arguments[0] -ne '--plan' -or $Arguments[2] -ne '--dry-run') { throw 'MOCK_REJECTED_ARGUMENTS' }
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
                '-ShotAfterReleaseMs', 0, '-ShotIntervalMs', 0, '-FireDelayMs', 300, '-Shots', 20, '-NoCapture', entry=harness, ok=True)
            repeat_task = json.loads((repeat_run / 'task.json').read_text())
            assert repeat_task['schema_version'] == 3 and repeat_task['repeatable'] is True
            launch = ('-Mode', 'Launch', '-RunDirectory', repeat_run,
                      '-AllowPhysicalOutput', '-Confirm', 'AUTO_STOP_COUNTERPULSE')
            invoke(*launch, entry=harness, ok=True)
            repeat_plan = json.loads((repeat_run / 'plan.json').read_text())
            repeat_plan['move_ms'] = 200
            repeat_plan['counter_hold_ms'] = 40
            repeat_plan['counter_delay_ms'] = 60
            repeat_plan['fire_delay_ms'] = 700
            repeat_plan['shots'] = 30
            (repeat_run / 'plan.json').write_text(json.dumps(repeat_plan), encoding='utf-8')
            (repeat_run / 'edit-during-mock').write_text('edit', encoding='utf-8')
            invoke(*launch, entry=harness, ok=True)
            executed = json.loads((repeat_run / 'result' / 'plan.json').read_text())
            assert executed['move_ms'] == 200 and executed['counter_hold_ms'] == 40
            assert executed['fire_delay_ms'] == 700 and executed['shots'] == 30
            assert executed['counter_delay_ms'] == 60 and executed['shot_after_release_ms'] == 0
            assert json.loads((repeat_run / 'plan.json').read_text())['move_ms'] == 123
            assert (repeat_run / 'execution-plan.json').read_bytes() == (repeat_run / 'result' / 'plan.json').read_bytes()
            assert (repeat_run / 'mock-calls').read_text().splitlines() == ['once', 'once']
            previous = (repeat_run / 'result' / 'plan.json').read_bytes()
            for invalid in ['{"move_ms":0}', '{"shots":0}', '{"fire_delay_ms":2001}', '{"counter_delay_ms":201}', ' ' * 16385]:
                (repeat_run / 'plan.json').write_text(invalid, encoding='utf-8')
                rejected = invoke(*launch, entry=harness)
                assert b'PLAN_VALIDATION_FAILED' in rejected.stderr
                assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
                assert (repeat_run / 'CONSUMED').exists()
                assert (repeat_run / 'mock-calls').read_text().splitlines() == ['once', 'once']
            (repeat_run / 'plan.json').write_text(json.dumps(repeat_plan), encoding='utf-8')
            live_task = dict(repeat_task)
            live_task['executable'] = str(Path(shell).resolve())
            live_task['executable_sha256'] = hashlib.sha256(Path(shell).read_bytes()).hexdigest().upper()
            (repeat_run / 'task.json').write_text(json.dumps(live_task), encoding='utf-8')
            invoke(*launch, entry=harness)
            assert (repeat_run / 'result' / 'plan.json').read_bytes() == previous
            assert (repeat_run / 'mock-calls').read_text().splitlines() == ['once', 'once']
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
