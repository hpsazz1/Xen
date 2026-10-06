"""Read-only config and legacy CLI tests: no capture, GSI or web listener is started."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    args = parser.parse_args()
    exe = str(Path(args.exe).resolve())
    def check(*argv, expected=0):
        result = subprocess.run([exe, *argv, '--check-config'], capture_output=True, encoding='utf-8')
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
        return json.loads(result.stdout)
    assert subprocess.check_output([exe, '--version'], encoding='utf-8').strip() == 'lineup-increment-20261005-2'
    legacy = check('--ndi-source', 'legacy-source', '--source-width', '1280', '--source-height', '720')
    assert legacy['source'] == 'legacy-source' and legacy['source_width'] == 1280
    assert legacy['size_origin'] == 'legacy_override'
    assert legacy['gsi_mode'] == 'shared' and not legacy['config_loaded']
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        config = root / 'shared.ini'
        config.write_text('[capture]\nbackend=ndi\nndi_source_name=shared-fixture\nndi_source_width=2560\nndi_source_height=1440\nndi_frame_layout=center_crop_1_to_1\nndi_clock_sync_url=udp://127.0.0.1:5011\n', encoding='utf-8')
        digest = hashlib.sha256(config.read_bytes()).hexdigest()
        shared = check('--config', str(config))
        assert shared['config_loaded'] and shared['source'] == 'shared-fixture'
        assert shared['size_origin'] == 'shared_ini'
        assert shared['source_width'] == 2560 and shared['source_height'] == 1440
        assert shared['clock_sync_url'] == 'udp://127.0.0.1:5011'
        assert shared['capture_error'] == ''
        assert hashlib.sha256(config.read_bytes()).hexdigest() == digest
        override = check('--source-width', '1280', '--config', str(config))
        assert override['source_width'] == 1280 and override['size_origin'] == 'legacy_override'
        synthetic = check('--config', str(config), '--synthetic')
        assert synthetic['source'] == 'synthetic-test-fixture'
        assert shared['geometry_mode'] == 'shared_capture_roi'
        assert shared['configured_roi_width'] == 320
        assert shared['locate_virtual_key'] == 0 and shared['locate_hotkey_status'] == 'unbound'
        config.write_text(config.read_text() + '\n[lineup]\nlocate_virtual_key=120\n', encoding='utf-8')
        assert check('--config', str(config), expected=1)['config_error']  # F9 already diagnostic
        config.write_text(config.read_text().replace('locate_virtual_key=120', 'locate_virtual_key=121'), encoding='utf-8')
        bound = check('--config', str(config))
        assert bound['locate_virtual_key'] == 121 and bound['locate_hotkey_status'] == 'requires_running_runtime'
        gsi = root / 'gsi.ini'
        gsi.write_text('[gsi]\nbind_address=0.0.0.0\n', encoding='utf-8')
        assert check('--gsi-config', str(gsi), expected=1)['gsi_config_error']
        missing = root / 'missing.ini'
        assert check('--config', str(missing), expected=1)['config_error']
        assert not missing.exists()
        config.write_text('[capture]\nndi_source_width=-5\n', encoding='utf-8')
        assert check('--config', str(config), expected=1)['config_error']
    print('lineup_config_tests: shared INI, unchanged bytes, precedence, ROI metadata pending, missing/invalid, legacy and synthetic passed')

if __name__ == '__main__':
    main()
