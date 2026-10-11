"""固定版本声明副本回归；所有安装命令均为替身，不访问网络或真实环境。"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
from unittest.mock import patch
import uuid


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve() / ('version-contract-' + uuid.uuid4().hex)
    root.mkdir(parents=True, exist_ok=False)
    scripts = Path(__file__).resolve().parents[1] / 'scripts'
    original = (scripts / 'model_training_requirements.txt').read_text(encoding='utf-8')
    expected = dict(line.split('==') for line in original.splitlines() if line and not line.startswith('#'))
    cases = [
        ('healthy', original, expected, False),
        ('trainer-copy-change', original.replace('8.3.203', '8.3.204'), dict(expected, ultralytics='8.3.204'), False),
        ('onnx-copy-change', original.replace('1.19.0', '1.19.1'), dict(expected, onnx='1.19.1'), False),
        ('duplicate', original + '\nultralytics==8.3.203\n', {}, True),
        ('range', original.replace('ultralytics==8.3.203', 'ultralytics>=8.3.203'), {}, True),
        ('missing', original.replace('Pillow==' + expected['Pillow'], ''), {}, True),
        ('extra', original + '\nrequests==2.32.4\n', {}, True),
        ('wheel-mismatch', original.replace('torch==2.8.0', 'torch==2.8.1'), {}, True),
    ]
    results = []
    for name, requirements, versions, reject in cases:
        directory = root / name
        directory.mkdir()
        for filename in ('model_training_environment.py', 'model_data_pipeline.py'):
            shutil.copy2(scripts / filename, directory / filename)
        path = directory / 'model_training_requirements.txt'
        path.write_text(requirements, encoding='utf-8')
        error = ''
        observed = {}
        try:
            environment = load(directory / 'model_training_environment.py', 'version_environment_' + name)
            pipeline = load(directory / 'model_data_pipeline.py', 'version_pipeline_' + name)
            observed = dict(environment=environment.VERSIONS, trainer=pipeline.TRAINER_VERSION)
            metadata_versions = expected if reject else versions
            with patch.object(environment.importlib.metadata, 'version', side_effect=lambda package: metadata_versions[package]):
                matched = environment.package_versions()
            passed = not reject and matched == versions and environment.VERSIONS == versions and pipeline.TRAINER_VERSION == versions['ultralytics']
        except Exception as exc:
            error = f'{type(exc).__name__}: {exc}'
            # 拒绝须来自声明合同，而非安装包元数据碰巧不匹配。
            passed = reject and 'REQUIREMENTS_CONTRACT' in error
        results.append(dict(case=name, passed=passed, error=error, observed=observed))

    directory = root / 'install-source-change'
    directory.mkdir()
    shutil.copy2(scripts / 'model_training_environment.py', directory / 'model_training_environment.py')
    path = directory / 'model_training_requirements.txt'
    path.write_text(original, encoding='utf-8')
    environment = load(directory / 'model_training_environment.py', 'version_install_snapshot')
    before = path.read_bytes()
    pip_bytes = []
    def fake_run(ctx, arguments, log, stage):
        arguments = list(map(str, arguments))
        Path(log).write_text('标准库替身，未执行外部命令\n', encoding='utf-8')
        if 'venv' in arguments:
            python = Path(arguments[-1]) / 'Scripts/python.exe'
            python.parent.mkdir(parents=True)
            python.write_bytes(b'not-an-executable')
            path.write_text(original.replace('1.19.0', '1.19.1'), encoding='utf-8')
        if '--report' in arguments:
            environment.write_json(arguments[arguments.index('--report') + 1], dict(install=[]))
        if '-r' in arguments:
            pip_bytes.append(Path(arguments[arguments.index('-r') + 1]).read_bytes())
        if '--worker-result' in arguments:
            environment.write_json(arguments[arguments.index('--worker-result') + 1],
                dict(schema_version=1, operation='env_check', ready=True, fixture_only=True))
    job = dict(operation='env_install', environment_root=str(directory / 'environments'), output=str(directory / 'output'))
    with patch.object(environment.Context, 'run', fake_run), patch.object(environment.sys, 'version_info', (3, 12, 0)):
        code = environment.execute(job, directory / 'status.json')
    record = json.loads(next((directory / 'environments').glob('*/environment.json')).read_text(encoding='utf-8'))
    passed = code == 0 and len(pip_bytes) == 1 and pip_bytes[0] == before and record['requirements_sha256'] == hashlib.sha256(pip_bytes[0]).hexdigest()
    results.append(dict(case='install-source-change', passed=passed, code=code, recorded_sha=record['requirements_sha256'],
                        installed_sha=hashlib.sha256(pip_bytes[0]).hexdigest() if pip_bytes else None))
    (root / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(root=str(root), results=results), ensure_ascii=False, indent=2))
    return 0 if all(item['passed'] for item in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
