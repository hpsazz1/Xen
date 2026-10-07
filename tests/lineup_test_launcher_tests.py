"""验证测试配置隔离、模型硬链接及 Launch 授权检查；不启动设备或截图。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--powershell', required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'scripts' / 'start_lineup_test.ps1'
    exe = Path(args.exe).resolve()
    with tempfile.TemporaryDirectory(prefix='xen-lineup-launch-') as folder:
        root = Path(folder) / '原配置'
        root.mkdir()
        (root / 'models').mkdir()
        model = root / 'models' / 'fixture.onnx'
        model.write_bytes(b'not a model: Prepare must never load it')
        config = root / 'config.ini'
        source = ('[detector]\nmodel_path=fixture.onnx\nbackend=cpu\n'
                  '[capture]\nbackend=ndi\nndi_source_name=launcher-fixture\n'
                  'ndi_source_width=640\nndi_source_height=480\nndi_frame_layout=center_crop_1_to_1\n'
                  '[mouse]\nallow_send_input=false\n'
                  '[keyboard]\nruntime_toggle_virtual_keys=119\nanomaly_mark_virtual_keys=120\n'
                  'aim_hold_virtual_keys=2,5\n[unrelated]\nkeep=保留\n')
        config.write_text(source, encoding='utf-8')
        before = hashlib.sha256(config.read_bytes()).hexdigest()
        worker = root / 'fixture-worker.exe'
        worker.write_bytes(b'never execute this fixture')
        web = root / 'web'
        web.mkdir()
        (web / 'index.html').write_text('fixture', encoding='utf-8')
        run = Path(folder) / '独立测试'
        def invoke(*extra, expected=0, directory=run):
            result = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', str(script), '-RunDirectory', str(directory), *map(str, extra)],
                capture_output=True, timeout=30)
            assert (result.returncode == 0) == (expected == 0), (result.returncode, result.stdout, result.stderr)
            return result
        invoke('-Mode', 'Prepare', '-XenRoot', root, '-WorkerExecutable', worker,
               '-LineupExecutable', exe, '-WebDirectory', web)
        assert hashlib.sha256(config.read_bytes()).hexdigest() == before
        candidate = (run / 'config.ini').read_text(encoding='utf-8')
        for text in ('test_mode=true', 'locate_virtual_key=119', 'throw_virtual_key=120',
                     'runtime_toggle_virtual_keys=\n', 'anomaly_mark_virtual_keys=\n',
                     'aim_hold_virtual_keys=135', 'emergency_virtual_keys=35', 'keep=保留'):
            assert text in candidate, text
        assert os.path.samefile(model, run / 'models' / model.name)
        task = json.loads((run / 'task.json').read_text(encoding='utf-8'))
        assert task['state'] == 'PREPARED_NOT_LAUNCHED' and not task['real_verified']
        assert not task['calibration_configured']
        assert '-AllowPhysicalOutput -PhysicalConfirm LINEUP_TEST_F8_F9' in (run / 'TASK.md').read_text(encoding='utf-8')
        invoke('-Mode', 'Check')
        invalid_run = Path(folder) / '无效监听'
        invoke('-Mode', 'Prepare', '-XenRoot', root, '-WorkerExecutable', worker,
               '-LineupExecutable', exe, '-WebDirectory', web, '-BindAddress', '0.0.0.0',
               expected=1, directory=invalid_run)
        assert not (invalid_run / 'task.json').exists()
        task_path = run / 'task.json'
        original_task = task_path.read_bytes()
        try:
            invalid_task = dict(task, bind='0.0.0.0')
            task_path.write_text(json.dumps(invalid_task, ensure_ascii=False), encoding='utf-8')
            invoke('-Mode', 'Check', expected=1)
        finally:
            task_path.write_bytes(original_task)
        invoke('-Mode', 'Launch', expected=1)
        assert not (run / 'lineup-stdout.log').exists()
        invoke('-Mode', 'Prepare', '-XenRoot', root, '-WorkerExecutable', worker,
               '-LineupExecutable', exe, '-WebDirectory', web, expected=1)
        (run / 'config.ini').write_text(candidate + '\n; changed\n', encoding='utf-8')
        invoke('-Mode', 'Check', expected=1)
        assert hashlib.sha256(config.read_bytes()).hexdigest() == before
    print('独立配置/模型硬链接/原配置不变/空标定提示/监听地址预检/重复Prepare/变更拒绝/未授权Launch全部通过；未启动设备')


if __name__ == '__main__':
    main()
