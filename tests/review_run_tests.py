import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('review_run', Path(__file__).resolve().parents[1] / 'scripts/review_run.py')
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class ReviewRunTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.run = self.root / 'run'
        self.run.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def put(self, name, value):
        path = self.run / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding='utf-8')

    def test_missing_degrades_and_is_reproducible(self):
        a = module.write_review(self.run, self.root / 'one')
        b = module.write_review(self.run, self.root / 'two')
        self.assertEqual(a, b)
        self.assertEqual((self.root / 'one/review.json').read_bytes(), (self.root / 'two/review.json').read_bytes())
        self.assertEqual(a['integrity']['status'], 'partial_or_unknown')

    def test_no_archive_overwrite(self):
        for output in [self.run, self.run / 'review', self.root]:
            with self.assertRaises(ValueError):
                module.write_review(self.run, output)

    def test_runtime_intervals_metrics_and_unknown_alignment(self):
        self.put('automatic/runtime.json', {'sample_count': 3, 'samples': [
            {'sequence': i, 'capture_steady_ns': str(1000000 + i * 1000000), 'capture_steady_ns_valid': True, 'source_timing_valid': False, 'total_ms': i + 1, 'source_to_control_ms': 0} for i in range(3)]})
        result = module.review(self.run)
        segment = result['segments'][0]
        self.assertEqual(len(segment['anomaly_intervals']), 1)
        self.assertEqual(segment['anomaly_intervals'][0]['end_ms'], 2)
        self.assertEqual(segment['anomaly_intervals'][0]['alignment'], 'unknown')
        self.assertEqual(segment['performance_ms']['total_ms']['p50'], 2)
        self.assertEqual(segment['performance_ms']['source_to_control_ms']['count'], 0)

    def test_frame_hash_and_same_source_identity(self):
        image = self.run / 'frame.png'
        image.write_bytes(b'original evidence')
        frame = {'file': 'frame.png', 'png_sha256': module.digest(image), 'source_timecode': 123, 'source_clock_session_id': 456, 'source_timecode_valid': True}
        self.put('manifest.json', {'frames': [frame]})
        self.put('runtime.json', {'samples': [{'success': False, 'source_timecode': '123', 'source_clock_session_id': '456', 'source_timecode_valid': True}]})
        result = module.review(self.run)
        self.assertEqual(result['segments'][0]['anomaly_intervals'][0]['anchors'], ['frame.png'])
        image.write_bytes(b'changed evidence')
        result = module.review(self.run)
        self.assertEqual(result['segments'][0]['anomaly_intervals'][0]['anchors'], [])
        self.assertIn('hash_mismatch', [i['reason'] for i in result['integrity']['issues']])

    def test_config_identity_without_secret_values(self):
        config = self.run / 'config.ini'
        config.write_text('token=TOP_SECRET', encoding='utf-8')
        self.put('task.json', {'config': {'sha256': module.digest(config)}})
        result = module.review(self.run)
        self.assertEqual(result['configuration']['status'], 'verified')
        self.assertNotIn('TOP_SECRET', json.dumps(result))

    def test_malformed_and_path_escape(self):
        (self.run / 'broken.json').write_text('{', encoding='utf-8')
        self.put('manifest.json', {'frames': [{'file': '../secret.png'}]})
        reasons = [i['reason'] for i in module.review(self.run)['integrity']['issues']]
        self.assertIn('invalid_json', reasons)
        self.assertIn('frame_path_outside_run', reasons)

    def test_lineup_missing_time_and_nonfinite_performance(self):
        self.put('observations.json', [{'status': 'LOST', 'sequence': 1, 'processing_ms': 0}, {'status': 'VALID', 'processing_ms': float('nan')}])
        segment = module.review(self.run)['segments'][0]
        self.assertEqual(segment['time_basis'], 'unknown')
        self.assertEqual(segment['performance_ms']['processing_ms']['count'], 1)
        self.assertIsNone(segment['anomaly_intervals'][0]['start_ms'])

    def test_source_unchanged(self):
        self.put('observations.json', [{'status': 'VALID', 'processing_ms': 12}])
        before = module.digest(self.run / 'observations.json')
        module.write_review(self.run, self.root / 'review')
        self.assertEqual(before, module.digest(self.run / 'observations.json'))
        self.assertEqual(len(list(self.run.iterdir())), 1)

    def test_service_export_frame_id_and_lowercase_valid(self):
        (self.run / 'one-raw.png').write_bytes(b'raw')
        self.put('observations.json', [{'frame_id': 'one', 'status': 'lost', 'recipe_id': 'recipe-1', 'performance': {'load_ms': 2}}, {'status': 'valid'}])
        result = module.review(self.run)
        segment = result['segments'][0]
        self.assertEqual(segment['anomaly_intervals'][0]['alignment'], 'export_frame_id')
        self.assertEqual(segment['anomaly_intervals'][0]['last_index'], 0)
        self.assertEqual(segment['performance_ms']['stage.load_ms']['p50'], 2)
        self.assertEqual(segment['reference_identities'], [{'recipe_id': 'recipe-1'}])


if __name__ == '__main__':
    unittest.main()
