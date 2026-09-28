"""自动整理与原生人工审核：合成数据专项，不代表模型质量。"""
import copy
import json
import unittest
from PIL import Image
import model_data_pipeline_tests as fixture

pipeline = fixture.pipeline


class ReviewTests(unittest.TestCase):
    setUp = fixture.PipelineTests.setUp
    tearDown = fixture.PipelineTests.tearDown
    job = fixture.PipelineTests.job
    context = fixture.PipelineTests.context

    def curate(self, name='curate', **kwargs):
        result = pipeline.review_curate(self.context('review_curate', output=str(self.base / name), **kwargs))
        return result, pipeline.read_json(result['triage_report'])

    def manifest(self, report, indices=(0,)):
        items = [copy.deepcopy(report['samples'][i]) for i in indices]
        for item in items:
            item['state'] = 'VERIFIED_POSITIVE' if item['detections'] else 'VERIFIED_NEGATIVE'
        return dict(format='xen-native-review-v1', schema_version=1, class_names=self.names,
                    root_identity=report['root_identity'], samples=items)

    def apply(self, manifest):
        path = self.base / 'decisions.json'
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return pipeline.import_labels(self.context('import_labels', review_manifest=str(path)))

    def test_curate_preserves_all_unknown_and_originals(self):
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        result, report = self.curate()
        self.assertEqual(len(report['samples']), 6)
        self.assertEqual(result['original_count'], 6)
        self.assertEqual(result['exact_unique_count'], 6)
        self.assertEqual(result['near_duplicate_group_count'], 3)
        self.assertTrue(all(i['state'] == 'UNKNOWN' for i in report['samples']))
        self.assertFalse((self.root / 'reviews').exists())
        self.assertNotIn('review_manifest', result)
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        for item in report['samples']:
            with Image.open(self.base / 'curate' / item['image']) as image:
                self.assertLessEqual(max(image.size), 640)
            self.assertEqual((item['width'], item['height']), (64, 48))
        self.assertIn('review_curate', pipeline.OPERATIONS)

    def test_native_append_and_per_sample_conflict(self):
        _, report = self.curate()
        first = self.manifest(report)
        self.apply(first)
        with self.assertRaisesRegex(pipeline.PipelineError, '过期'):
            self.apply(first)
        self.apply(self.manifest(report, (1,)))
        _, updated = self.curate('updated')
        self.assertEqual(sum(i['state'] != 'UNKNOWN' for i in updated['samples']), 2)
        self.assertEqual(len(list((self.root / 'reviews').glob('*/revision.json'))), 2)

    def test_invalid_native_is_atomic(self):
        _, report = self.curate()
        original = self.manifest(report)
        mutations = [lambda m: m.update(root_identity='foreign'),
                     lambda m: m['samples'][0].update(image_sha256='bad'),
                     lambda m: m['samples'][0].update(base_review_token='old'),
                     lambda m: m['samples'][0].update(state='UNKNOWN'),
                     lambda m: m['samples'][0].update(state='VERIFIED_POSITIVE', detections=[]),
                     lambda m: m['samples'][0].update(state='EXCLUDED', detections=[dict(class_id=0,x1=1,y1=1,x2=2,y2=2)]),
                     lambda m: m['samples'][0].update(state='VERIFIED_POSITIVE', detections=[dict(class_id=0,x1=float('nan'),y1=1,x2=2,y2=2)]),
                     lambda m: m['samples'].append(copy.deepcopy(m['samples'][0]))]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                manifest = copy.deepcopy(original)
                mutate(manifest)
                with self.assertRaises(pipeline.PipelineError):
                    self.apply(manifest)
                self.assertEqual(list((self.root / 'reviews').glob('*/revision.json')), [])

    def test_prelabels_are_hints_and_hash_checked(self):
        _, samples = pipeline.load_samples(self.context('inspect'))
        item = next(iter(samples.values()))
        prediction = dict(schema_version=1, class_names=self.names, samples=[dict(session_id=item['session_id'], sample_id=item['sample_id'], image_sha256=item['image_sha256'], detections=[])])
        path = self.base / 'prelabels.json'
        pipeline.write_json(path, prediction)
        _, report = self.curate(prelabels=str(path))
        selected = next(i for i in report['samples'] if i['session_id'] == item['session_id'] and i['sample_id'] == item['sample_id'])
        self.assertEqual(selected['state'], 'UNKNOWN')
        self.assertTrue(any('分歧' in reason for reason in selected['reasons']))
        prediction['samples'][0]['image_sha256'] = 'bad'
        pipeline.write_json(path, prediction)
        with self.assertRaises(pipeline.PipelineError):
            self.curate('bad', prelabels=str(path))

    def test_html_json_is_script_safe(self):
        self.names = ['</script><script>alert(1)</script>']
        for header in self.root.glob('*/session.json'):
            data = pipeline.read_json(header)
            data['class_names'] = self.names
            pipeline.write_json(header, data)
        result, report = self.curate()
        text = (self.base / 'curate' / 'review.html').read_text(encoding='utf-8')
        self.assertNotIn(self.names[0], text)
        self.assertIn('\\u003c/script>', text)
        self.assertEqual(report['class_names'], self.names)

    def test_no_implicit_cap_and_exact_group(self):
        source = self.root / 'session-0' / 'samples' / '1.json'
        data = pipeline.read_json(source)
        for index in range(300):
            pipeline.write_json(source.parent / f'copy{index}.json', dict(data, sample_id=f'copy{index}'))
        result, report = self.curate()
        self.assertEqual(result['original_count'], 306)
        self.assertEqual(result['exact_unique_count'], 6)
        selected = [i for i in report['samples'] if i['image_sha256'] == data['image_sha256']]
        self.assertEqual(len(selected), 301)
        self.assertEqual(len({i['group'] for i in selected}), 1)
        self.assertTrue(all(i['state'] == 'UNKNOWN' for i in selected))

    def test_native_review_enters_existing_training_export(self):
        _, report = self.curate()
        self.apply(self.manifest(report, tuple(range(6))))
        pipeline.export_dataset(self.context('export', output=str(self.base / 'dataset')))
        manifest = pipeline.validate_dataset(self.base / 'dataset')
        self.assertEqual(manifest['states']['VERIFIED_NEGATIVE'], 3)
        self.assertEqual(manifest['states']['VERIFIED_POSITIVE'], 3)

    def test_clock_rollback_still_appends_latest(self):
        from datetime import datetime, timezone
        from unittest.mock import patch
        _, report = self.curate()
        self.apply(self.manifest(report))
        _, current = self.curate('current')
        same = next(i for i, item in enumerate(current['samples']) if item['state'] != 'UNKNOWN')
        manifest = self.manifest(current, (same,))
        manifest['samples'][0].update(state='EXCLUDED', detections=[])
        class Earlier(datetime):
            @classmethod
            def now(cls, tz=None):
                return datetime(2001, 1, 1, tzinfo=timezone.utc)
        with patch.object(pipeline, 'datetime', Earlier):
            self.apply(manifest)
        names, samples = pipeline.load_samples(self.context('inspect'))
        reviews = pipeline.latest_reviews(self.root, samples, names)
        key = (manifest['samples'][0]['session_id'], manifest['samples'][0]['sample_id'])
        self.assertEqual(reviews[key]['state'], 'EXCLUDED')

    def test_review_page_rejects_per_image_limits_before_pair_comparison(self):
        from unittest.mock import patch
        source = self.root / 'session-0' / 'samples' / '1.json'
        original = pipeline.read_json(source)
        boxes = [dict(class_id=0, x1=3, y1=4, x2=28, y2=38)] * 1025
        for kind in ('original', 'predicted', 'reviewed'):
            with self.subTest(kind=kind):
                kwargs = {}
                pipeline.write_json(source, dict(original, detections=boxes) if kind == 'original' else original)
                if kind == 'predicted':
                    path = self.base / 'oversized-prediction.json'
                    pipeline.write_json(path, dict(schema_version=1, class_names=self.names, samples=[dict(original, detections=boxes)]))
                    kwargs['prelabels'] = str(path)
                reviewed = {(original['session_id'], original['sample_id']): dict(state='VERIFIED_POSITIVE', detections=boxes)} if kind == 'reviewed' else {}
                with patch.object(pipeline, 'latest_reviews', return_value=reviewed), patch.object(pipeline, 'box_iou', side_effect=AssertionError('不应进入两两框比较')):
                    with self.assertRaisesRegex(pipeline.PipelineError, '1024'):
                        self.curate('limit-' + kind, **kwargs)
                self.assertFalse((self.base / ('limit-' + kind)).exists())
        pipeline.write_json(source, original)
        for axis in ('width', 'height'):
            with self.subTest(axis=axis):
                image = self.root / 'session-0' / 'images' / '1.png'
                Image.new('RGB', (16385, 1) if axis == 'width' else (1, 16385)).save(image)
                pipeline.write_json(source, dict(original, width=16385 if axis == 'width' else 1, height=16385 if axis == 'height' else 1, detections=[], image_sha256=pipeline.sha256(image)))
                with self.assertRaisesRegex(pipeline.PipelineError, '16384'):
                    self.curate('limit-' + axis)
                self.assertFalse((self.base / ('limit-' + axis)).exists())

    def test_review_page_accepts_inclusive_limits(self):
        source = self.root / 'session-0' / 'samples' / '1.json'
        original = pipeline.read_json(source)
        image = self.root / 'session-0' / 'images' / '1.png'
        Image.new('RGB', (16384, 48)).save(image)
        pipeline.write_json(source, dict(original, width=16384, image_sha256=pipeline.sha256(image), detections=original['detections'] * 1024))
        _, report = self.curate()
        selected = next(i for i in report['samples'] if i['session_id'] == 'session-0' and i['sample_id'] == '1')
        self.assertEqual(len(selected['detections']), 1024)
        self.assertEqual(selected['width'], 16384)

    def test_existing_lock_not_removed(self):
        _, report = self.curate()
        directory = self.root / 'reviews'
        directory.mkdir()
        lock = directory / '.import.lock'
        lock.write_text('other')
        with self.assertRaisesRegex(pipeline.PipelineError, '并发'):
            self.apply(self.manifest(report))
        self.assertEqual(lock.read_text(), 'other')


if __name__ == '__main__':
    unittest.main()
