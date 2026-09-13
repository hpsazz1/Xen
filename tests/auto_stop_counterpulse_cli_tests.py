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
            assert plan['counter_hold_ms'] == 30 and plan['shot_interval_ms'] == 280
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
