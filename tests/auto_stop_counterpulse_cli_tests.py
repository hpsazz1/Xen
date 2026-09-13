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

    def invoke(*values, ok=False):
        result = subprocess.run([shell, '-NoProfile', '-File', str(script), *map(str, values)],
                                capture_output=True, timeout=30)
        assert (result.returncode == 0) == ok, '入口返回值不符合预期'

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
                '-ConfigPath', config, '-Shots', 21, '-ShotIntervalMs', 650)
            assert not overflow.exists()
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
    print('反向轻点CLI无设备回归通过')


if __name__ == '__main__':
    main()
