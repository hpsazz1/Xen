"""快捷方式仅创建/回读，不启动面板、F7或设备；保护已有配置和他人入口。"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--powershell', required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'scripts/create_lineup_capture_shortcut.ps1'
    with tempfile.TemporaryDirectory(prefix='lineup-shortcut-') as tmp:
        root = Path(tmp) / "离线 空格'目录"
        root.mkdir()
        library = root / '本地数据'
        binary = root / '假Host.exe'
        binary.write_bytes(b'never execute')
        options = root / '选项.json'
        options.write_text('{}', encoding='utf-8')
        shortcut = root / '道具采集.lnk'
        base = [args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
                '-LibraryDirectory', str(library), '-Executable', str(binary), '-OptionsPath', str(options),
                '-SourceId', '本地源', '-SourceWidth', '2560', '-SourceHeight', '1440',
                '-SyncInbox', r'\\unreachable.invalid\unused\inbox', '-ShortcutPath', str(shortcut)]

        def run(command, expected=0):
            result = subprocess.run(command, capture_output=True, timeout=15)
            assert (result.returncode == 0) == (expected == 0), (result.returncode, result.stdout, result.stderr)
            return result

        run(base)
        assert shortcut.exists()
        assert list(library.iterdir()) == [library / 'config.json']
        config_bytes = (library / 'config.json').read_bytes()
        config = json.loads(config_bytes)
        assert config['source_id'] == '本地源' and config['sync_inbox'].startswith('\\\\unreachable.invalid')
        run(base)
        assert (library / 'config.json').read_bytes() == config_bytes
        query = root / 'read-link.ps1'
        query.write_text('param([string]$Path)\n[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n'
                        '$item=(New-Object -ComObject WScript.Shell).CreateShortcut($Path)\n'
                        '@{target=$item.TargetPath;arguments=$item.Arguments;description=$item.Description}|ConvertTo-Json',
                        encoding='utf-8-sig')
        result = run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(query), str(shortcut)])
        link = json.loads(result.stdout.decode('utf-8-sig'))
        assert Path(link['target']).name.lower() == 'powershell.exe'
        assert '-STA' in link['arguments'] and f'-LibraryDirectory "{library}"' in link['arguments']
        assert f'-Executable "{binary}"' in link['arguments']
        assert 'AllowPhysicalOutput' not in link['arguments'] and 'Launch' not in link['arguments']
        config['source_width'] = 1920
        (library / 'config.json').write_text(json.dumps(config, ensure_ascii=False), encoding='utf-8')
        changed = (library / 'config.json').read_bytes()
        run(base, expected=1)
        assert (library / 'config.json').read_bytes() == changed
        (library / 'config.json').write_bytes(config_bytes)
        foreign = root / '他人的入口.lnk'
        foreign.write_bytes(b'preserve this file')
        run(base[:-1] + [str(foreign)], expected=1)
        assert foreign.read_bytes() == b'preserve this file'
    print('离线快捷方式创建/引号与中文路径/回读/保留配置与他人入口通过；未启动任何程序')


if __name__ == '__main__':
    main()
