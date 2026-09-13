"""合成 PNG/CSV 的离线契约回归，不连接设备。"""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import cv2
import numpy as np

SPEC = importlib.util.spec_from_file_location('impacts', Path(__file__).resolve().parents[1] /
                                             'scripts/analyze_auto_stop_impacts.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ImpactTests(unittest.TestCase):
    def run_case(self, mode='valid', flags=()):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'frames'
            source.mkdir()
            shots = [{'start_ns': 100 + i*100, 'end_ns': 190+i*100} for i in range(7)]
            (root / 'shots.json').write_text(json.dumps({'shots': shots}), encoding='utf-8')
            use_report = mode.startswith('report_')
            (root / 'result.json').write_text(json.dumps({'success': True, 'capture_complete': mode != 'report_incomplete',
                'plan': {'shots': 7, 'shot_interval_ms': 0.00009},
                'commands': [{'kind': 'left_button', 'value': 1, 'shot_index': i+1,
                              'submit_ns': shot['start_ns']} for i, shot in enumerate(shots)]}), encoding='utf-8')
            registering = mode in ('translated', 'bad_registration')
            image = np.zeros((220, 260, 3) if registering else (80, 180, 3), np.uint8)
            if registering:
                rng = np.random.default_rng(51)
                image[110:210, 20:240] = rng.integers(10, 180, (100, 220, 1), dtype=np.uint8)
            rows = []

            def save(time):
                name = f'frame_{len(rows)}.png'
                rendered = image
                if registering:
                    displacement = min(len(rows)//2, 6)
                    rendered = cv2.warpAffine(image, np.array([[1., 0., displacement], [0., 1., 0.]]), (260, 220))
                    if mode == 'bad_registration' and len(rows) > 6:
                        rendered[120:180, 40:200] = 0
                self.assertTrue(cv2.imwrite(str(source / name), rendered))
                rows.append({'status': 'FRAME', 'png': name, 'captured_at_ns': time, 'error': '',
                             'time_basis': 'NDI' if mode == 'report_ndi' else 'DESKTOP_DUPLICATION',
                             'source_time_at_ns': time, 'source_time_valid': '1',
                             'source_clock_uncertainty_ms': 0.1})

            save(1)
            for i in range(7):
                px = 20 if mode == 'overlap' else 20+i*20
                py = 25+i*3 if mode == 'sloped' else 25
                if not (mode == 'missing' and i == 3):
                    cv2.circle(image, (px, py), 2, (255, 0, 255), -1)
                if mode == 'multi' and i == 3:
                    cv2.circle(image, (px, 55), 2, (255, 0, 255), -1)
                if mode == 'camera' and i == 4:
                    image[70, 160] = (255, 255, 255)
                save(110+i*100)
                save(150+i*100)
            save(800)
            with (source / 'frames.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
                writer.writeheader()
                writer.writerows(rows)
            roi_flags = ['--roi', '10', '10', '180', '70', '--registration-roi', '40', '125', '160', '60',
                         '--min-registration-texture', '5', '--min-registration-response', '0.5',
                         '--max-registration-residual', '8', '--max-translation', '8'] if registering else [
                             '--roi', '0', '0', '180', '80']
            timing_flags = ['--report-json', str(root / 'result.json'), '--accept-command-windows',
                            '--command-frame-clock-confirmed'] if use_report else ['--shots-json', str(root / 'shots.json')]
            args = MODULE.parser().parse_args([
                '--input', str(source), '--output', str(root / 'analysis'),
                *timing_flags, '--expected', '7',
                *roi_flags, '--hsv-low', '140', '200', '200',
                '--hsv-high', '160', '255', '255', '--y-tolerance', '2',
                '--slope-tolerance', '0.02', '--min-x-span', '30', *flags])
            result = MODULE.analyze(args)
            self.assertEqual(result, json.loads((root / 'analysis/report.json').read_text(encoding='utf-8')))
            self.assertLessEqual(len(list((root / 'analysis').glob('*.png'))), 128)
            self.assertEqual(len(list(source.glob('*.png'))), 16)
            with self.assertRaises(FileExistsError):
                MODULE.analyze(args)
            return result

    def test_confirmed_horizontal_marks_pass(self):
        result = self.run_case(flags=['--color-confirmed'])
        self.assertEqual(result['status'], 'SCENE_GEOMETRY_PASS')
        self.assertEqual(result['metrics']['slope'], 0)
        self.assertEqual(len(result['points']), 7)

    def test_unconfirmed_color_never_passes(self):
        self.assertIn('COLOR_UNCONFIRMED', self.run_case()['issues'])

    def test_missing_not_zero(self):
        result = self.run_case('missing', ['--color-confirmed'])
        self.assertIsNone(result['points'][3]['y'])
        self.assertIn('MISSING_OR_OVERLAP', result['issues'])

    def test_multiple_new_marks_rejected(self):
        self.assertIn('MULTIPLE_MARKS', self.run_case('multi', ['--color-confirmed'])['issues'])

    def test_exact_overlap_not_counted_as_new_shot(self):
        self.assertIn('MISSING_OR_OVERLAP', self.run_case('overlap', ['--color-confirmed'])['issues'])

    def test_background_change_rejected(self):
        self.assertIn('BACKGROUND_CHANGED', self.run_case('camera', ['--color-confirmed'])['issues'])

    def test_sloped_line_not_corrected_by_rotation(self):
        self.assertIn('GEOMETRY_OUTSIDE_TOLERANCE', self.run_case('sloped', ['--color-confirmed'])['issues'])

    def test_insufficient_span_rejected(self):
        self.assertIn('INSUFFICIENT_HORIZONTAL_SPAN', self.run_case(flags=[
            '--color-confirmed', '--min-x-span', '150'])['issues'])

    def test_background_translation_uses_independent_texture(self):
        result = self.run_case('translated', ['--color-confirmed'])
        self.assertEqual(result['status'], 'SCENE_GEOMETRY_PASS', result['issues'])
        self.assertGreater(result['registrations'][-1]['translation_x'], 5)
        self.assertGreater(result['points'][-1]['original_x'], result['points'][-1]['x'])

    def test_bad_registration_refuses_geometry(self):
        result = self.run_case('bad_registration', ['--color-confirmed'])
        self.assertEqual(result['status'], 'INDETERMINATE')
        self.assertTrue(any(issue.startswith('REGISTRATION_') for issue in result['issues']))

    def test_report_dxgi_candidates_preserve_timing_warning(self):
        result = self.run_case('report_dxgi', ['--color-confirmed'])
        self.assertEqual(result['status'], 'SCENE_GEOMETRY_PASS')
        self.assertEqual(result['timing_window_basis'], 'COMMAND_SUBMIT_CANDIDATE')
        self.assertIsNotNone(result['timing_warning'])

    def test_incomplete_capture_refuses_command_windows(self):
        with self.assertRaisesRegex(ValueError, '不完整'):
            self.run_case('report_incomplete', ['--color-confirmed'])

    def test_report_ndi_requires_source_uncertainty_gate(self):
        with self.assertRaisesRegex(ValueError, 'NDI'):
            self.run_case('report_ndi', ['--color-confirmed'])
        result = self.run_case('report_ndi', ['--color-confirmed', '--max-source-clock-uncertainty-ms', '0.2'])
        self.assertEqual(result['status'], 'SCENE_GEOMETRY_PASS')

    def test_report_ndi_excess_uncertainty_rejected(self):
        with self.assertRaisesRegex(ValueError, 'NDI'):
            self.run_case('report_ndi', ['--color-confirmed', '--max-source-clock-uncertainty-ms', '0.01'])


if __name__ == '__main__':
    unittest.main()
