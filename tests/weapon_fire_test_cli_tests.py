"""独立射击入口离线回归；仅Prepare/Validate及必拒绝的Launch，不连接设备。"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--script', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    script = args.script or root / 'scripts/invoke_weapon_fire_test.ps1'
    engine = root / 'scripts/invoke_auto_stop_counterpulse.ps1'
    shell = shutil.which('powershell.exe')
    assert shell, '需要Windows PowerShell'
    with tempfile.TemporaryDirectory(prefix='xen-weapon-fire-') as temp:
        folder = Path(temp)
        config = folder / 'test.ini'
        config.write_text('[source_context]\nenabled=true\n', encoding='utf-8')
        run = folder / "weapon's test"
        def invoke(mode, *options, good=False, env=None):
            p = subprocess.run([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                                '-Mode', mode, '-RunDirectory', str(run), *map(str, options)],
                               capture_output=True, timeout=30, env=env)
            assert (p.returncode == 0) == good, p.stderr.decode('utf-8', 'replace')
            return p
        inputs = ['-EngineScript', engine, '-Executable', args.executable.resolve(), '-ConfigPath', config,
                  '-CredentialDirectory', folder / "credential's folder", '-Scope', 'LocalMachine']
        invoke('Launch')
        invoke('Prepare', *inputs, '-AllowPhysicalOutput')
        assert not run.exists()
        invoke('Prepare', *inputs, good=True)
        for name in ['fire-settings.json', 'edit-config.bat', 'start-test.bat', 'TASK.md']:
            assert (run / name).is_file(), name
        settings = run / 'fire-settings.json'
        original = settings.read_bytes()
        assert json.loads(original.decode('utf-8-sig')) == dict(shot_hold_ms=80, fire_interval_ms=800)
        invoke('Validate', good=True)
        preview = json.loads((run / 'preview-plan.json').read_text(encoding='utf-8-sig'))
        assert preview['baseline'] == 'stationary' and preview['shots'] == 15
        assert preview['shot_hold_ms'] == 80 and preview['fire_interval_ms'] == 800
        assert preview['counter_delay_ms'] == 0 and preview['move_during_fire_delay'] is False
        assert not (run / 'runs').exists() or not list((run / 'runs').iterdir())
        # 长按边界与无效参数验证，始终走生产配置转换及正式plan parser。
        settings.write_text(json.dumps(dict(shot_hold_ms=1000, fire_interval_ms=2000)), encoding='utf-8')
        invoke('Validate', good=True)
        preview = json.loads((run / 'preview-plan.json').read_text(encoding='utf-8-sig'))
        assert preview['shot_hold_ms'] == 1000 and preview['fire_interval_ms'] == 2000
        for value in [dict(shot_hold_ms=0, fire_interval_ms=800), dict(shot_hold_ms=80, fire_interval_ms=80),
                      dict(shot_hold_ms=True, fire_interval_ms=800), dict(shot_hold_ms=80.5, fire_interval_ms=800),
                      dict(shot_hold_ms=80, fire_interval_ms=5001), dict(shot_hold_ms=80, fire_interval_ms=3000), dict(shot_hold_ms=80),
                      dict(shot_hold_ms=80, fire_interval_ms=800, counter_hold_ms=40)]:
            settings.write_text(json.dumps(value), encoding='utf-8')
            invoke('Validate')
        settings.write_bytes(original)
        invoke('Validate', '-AllowPhysicalOutput')
        # 即便令牌正确，SSH环境也必须在任何原生Launch前拒绝。
        remote = dict(os.environ, SSH_CONNECTION='test-only-no-network')
        invoke('Launch', '-AllowPhysicalOutput', '-Confirm', 'WEAPON_FIRE_TEST', env=remote)
        assert not (run / 'runs').exists() or not list((run / 'runs').iterdir())
    print('独立射击配置/静止计划/边界/前台限制专项通过；未连接设备')


if __name__ == '__main__':
    main()
