"""用临时四件套验证显式传送、重复跳过、冲突拒绝和半包不发布；不连接辅机。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--powershell', required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'scripts/sync_lineup_captures.ps1'
    with tempfile.TemporaryDirectory(prefix='lineup-transfer-') as tmp:
        root = Path(tmp)
        library = root / '主机离线库'
        package = library / 'captures' / 'host-fixture-1'
        package.mkdir(parents=True)
        for name in ('full.png', 'roi.png', 'overview.png'):
            (package / name).write_bytes(('传送夹具 ' + name).encode())
        (package / 'capture.json').write_text(json.dumps({
            'schema': 1, 'id': package.name, 'origin': 'desktop_duplication'}, ensure_ascii=False), encoding='utf-8')
        before = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in package.iterdir()}
        pending = library / 'captures' / '.pending-test'
        pending.mkdir()
        (pending / 'full.png').write_bytes(b'unfinished')
        inbox = root / '模拟辅机收件箱'
        inbox.mkdir()

        def send(expected=0):
            result = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', str(script), '-LibraryDirectory', str(library), '-InboxDirectory', str(inbox)],
                capture_output=True, timeout=30)
            assert (result.returncode == 0) == (expected == 0), (result.returncode, result.stdout, result.stderr)
            return result

        send()
        target = inbox / 'captures' / package.name
        assert {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in target.iterdir()} == before
        assert list((inbox / 'captures').iterdir()) == [target]
        original_times = {f.name: f.stat().st_mtime_ns for f in target.iterdir()}
        send()
        assert {f.name: f.stat().st_mtime_ns for f in target.iterdir()} == original_times
        (target / 'roi.png').write_bytes(b'conflict')
        send(expected=1)
        assert (target / 'roi.png').read_bytes() == b'conflict'
        assert {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in package.iterdir()} == before
        (target / 'roi.png').write_bytes((package / 'roi.png').read_bytes())
        invalid = library / 'captures' / 'host-fixture-2'
        invalid.mkdir()
        (invalid / 'capture.json').write_text('{}', encoding='utf-8')
        send(expected=1)
        assert not (inbox / 'captures' / invalid.name).exists()
        assert list((inbox / 'captures').iterdir()) == [target]
        (invalid / 'capture.json').unlink()
        invalid.rmdir()
        for file in target.iterdir():
            file.unlink()
        target.rmdir()
        command = [args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                   '-LibraryDirectory', str(library), '-InboxDirectory', str(inbox)]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for process in processes:
            process.communicate(timeout=30)
        assert any(process.returncode == 0 for process in processes)
        assert {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in target.iterdir()} == before
        assert list((inbox / 'captures').iterdir()) == [target]
    print('本地显式传送、重复跳过、同名冲突拒绝、原图保留、半包不发布通过；未连接辅机')


if __name__ == '__main__':
    main()
