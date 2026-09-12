"""真实子进程生命周期夹具；不导入训练框架或启动设备。"""
import argparse
import hashlib
import json
import os
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--job', required=True)
parser.add_argument('--status', required=True)
parser.add_argument('--cancel', required=True)
args = parser.parse_args()
spec = json.loads(Path(args.job).read_text(encoding='utf-8'))

def publish(state, message, result=None):
    status = Path(args.status)
    pending = status.with_suffix('.pending')
    pending.write_text(json.dumps({'schema_version': 1, 'operation': spec['operation'],
                                  'state': state, 'message': message,
                                  'result': result if result is not None else {'root': spec.get('root', '')}}, ensure_ascii=False), encoding='utf-8')
    os.replace(pending, status)

publish('RUNNING', '真实子进程已读取路径和参数')
if spec['operation'] == 'train':
    for _ in range(200):
        if Path(args.cancel).exists():
            publish('CANCELLED', '已收到取消请求')
            raise SystemExit(2)
        time.sleep(0.025)
result = {'root': spec.get('root', '')}
if spec['operation'] in {'inspect', 'evaluate'} and spec.get('model'):
    model = Path(spec['model']).resolve()
    result.update(model=str(model), model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),
                  model_class_names=['person', 'head'])
    if spec['operation'] == 'evaluate':
        dataset = Path(spec['dataset']).resolve()
        manifest = json.loads((dataset / 'dataset.json').read_text(encoding='utf-8'))
        result.update(dataset=str(dataset), dataset_sha256=hashlib.sha256((dataset / 'dataset.json').read_bytes()).hexdigest(),
                      class_names=manifest['class_names'], passed_compatibility=True, metrics={'fixture_only': 1.0})
publish('SUCCEEDED', '参数和状态往返成功', result)
