"""跨语言生产链合同：C++采集原图 -> Python审核往返 -> 冻结YOLO数据集。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

collector, pipeline = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix='xen-data-integration-') as folder:
    base = Path(folder)
    capture = base / '中文采集 space & literal'
    subprocess.run([collector, '--fixture', str(capture)], check=True, timeout=30,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    raw = capture / 'raw'
    serial = 0

    def job(operation, expect=0, **kwargs):
        global serial
        serial += 1
        specification = dict(operation=operation, root=str(raw), class_names=['person'], **kwargs)
        job_path, status_path = base / f'{serial}.json', base / f'{serial}-status.json'
        job_path.write_text(json.dumps(specification, ensure_ascii=False), encoding='utf-8')
        result = subprocess.run([sys.executable, '-B', '-X', 'utf8', pipeline,
                                 '--job', str(job_path), '--status', str(status_path)],
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        assert result.returncode == expect, result.stdout + result.stderr
        status = json.loads(status_path.read_text(encoding='utf-8'))
        assert status['state'] == ('SUCCEEDED' if expect == 0 else 'FAILED'), status
        return status.get('result', {})

    inspected = job('inspect')
    assert inspected['samples'] == 6 and inspected['sessions'] == 3, inspected
    # 生产预标签未审核，不能直接训练。
    job('export', expect=1, output=str(base / 'unreviewed-export'))
    review = job('review_export', output=str(base / 'review'))
    manifest_path = Path(review['review_manifest'])
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    assert all(sample['state'] == 'UNKNOWN' for sample in manifest['samples'])
    manifest['reviewer'] = 'synthetic-fixture-verifier'
    for sample in manifest['samples']:
        label = manifest_path.parent / sample['label']
        # 这里只为合成夹具确认真值，不把真实模型预测升级为人工真值。
        expected_positive = sample['sample_id'] == '1'
        assert bool(label.read_text(encoding='utf-8').strip()) == expected_positive
        sample['state'] = 'VERIFIED_POSITIVE' if expected_positive else 'VERIFIED_NEGATIVE'
    manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
    imported = job('import_labels', review_manifest=str(manifest_path))
    assert imported['samples'] == 6
    exported = job('export', output=str(base / 'dataset'))
    dataset = Path(exported['dataset'])
    frozen = json.loads((dataset / 'dataset.json').read_text(encoding='utf-8'))
    assert frozen['counts'] == {'train': 2, 'val': 2, 'test': 2}, frozen
    assert frozen['states'] == {'VERIFIED_POSITIVE': 3, 'VERIFIED_NEGATIVE': 3}, frozen
    for sample in frozen['samples']:
        assert (dataset / sample['image']).is_file()
        label = dataset / sample['label']
        assert label.is_file()
        assert bool(label.read_text().strip()) == (sample['state'] == 'VERIFIED_POSITIVE')
    print('跨语言闭环通过：3会话、6张真实PNG，3正3负，未知拒绝，审核/分组导出一致；未训练或运行设备。')
