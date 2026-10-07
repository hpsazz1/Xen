"""验证测试配置隔离、模型硬链接及 Launch 授权检查；不启动设备或截图。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def verify_auth_launch(args, script, folder):
    """通过完整生产入口启动无设备夹具，核对凭据实际进入 Worker 子进程。"""
    root = Path(folder) / '凭据启动夹具'
    root.mkdir()
    setup = root / 'fixture.ps1'
    setup.write_text(r'''
param([string]$Root)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$source = @'
using System;
using System.IO;
using System.Threading;
public class LineupLaunchFixture {
    public static void Main(string[] args) {
        if (Array.IndexOf(args, "--check-config") >= 0) {
            Console.WriteLine("{\"test_mode\":true,\"locate_virtual_key\":119,\"throw_virtual_key\":120,\"calibration_configured\":false}");
            return;
        }
        var token = Environment.GetEnvironmentVariable("XEN_SOURCE_CONTEXT_TOKEN");
        if (Array.IndexOf(args, "--web") >= 0) {
            File.WriteAllText("helper-auth-result.txt", String.IsNullOrEmpty(token) ? "absent" : "present");
            Thread.Sleep(5000);
            return;
        }
        File.WriteAllText("worker-auth-result.txt", token == new string('x', 64) ? "matched" : "missing_or_wrong");
        File.WriteAllText("worker-root-result.txt", Environment.GetEnvironmentVariable("XEN_RELEASE_ROOT") ?? "");
    }
}
'@
Add-Type -TypeDefinition $source -OutputAssembly (Join-Path $Root 'fixture-worker.exe') -OutputType ConsoleApplication
Copy-Item -LiteralPath (Join-Path $Root 'fixture-worker.exe') -Destination (Join-Path $Root 'fixture-helper.exe')
$credential = Join-Path $Root 'credentials'
[void][IO.Directory]::CreateDirectory($credential)
$plain = [Text.Encoding]::UTF8.GetBytes(('x' * 64))
try {
    $cipher = [Security.Cryptography.ProtectedData]::Protect($plain, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.File]::WriteAllBytes((Join-Path $credential 'token.dpapi'), $cipher)
} finally { [Array]::Clear($plain, 0, $plain.Length) }
''', encoding='utf-8-sig')
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() != 'XEN_SOURCE_CONTEXT_TOKEN'}
    def run_ps(*arguments, expected=0, child_environment=None):
        result = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 *map(str, arguments)], env=child_environment or environment,
                                capture_output=True, timeout=30)
        assert (result.returncode == 0) == (expected == 0), (result.returncode, result.stdout, result.stderr)
        assert b'x' * 64 not in result.stdout + result.stderr, '启动输出泄露了测试认证值'
        return result
    run_ps('-File', setup, '-Root', root)
    (root / 'models').mkdir()
    (root / 'models' / 'fixture.onnx').write_bytes(b'not a model')
    (root / 'web').mkdir()
    (root / 'web' / 'index.html').write_text('fixture', encoding='utf-8')
    config = root / 'config.ini'
    config.write_text('[detector]\nmodel_path=fixture.onnx\nbackend=cpu\n'
                      '[source_context]\nenabled=true\nhost=127.0.0.1\nport=5012\n', encoding='utf-8')
    original = config.read_bytes()
    run = root / 'run'
    run_ps('-File', script, '-Mode', 'Prepare', '-RunDirectory', run, '-XenRoot', root,
           '-WorkerExecutable', root / 'fixture-worker.exe', '-LineupExecutable', root / 'fixture-helper.exe',
           '-WebDirectory', root / 'web', '-CredentialDirectory', root / 'credentials', '-Scope', 'CurrentUser')
    task_path = run / 'task.json'
    task = json.loads(task_path.read_text(encoding='utf-8'))
    assert Path(task['credential_directory']) == root / 'credentials'
    assert task['credential_scope'] == 'CurrentUser'
    run_ps('-File', script, '-Mode', 'Check', '-RunDirectory', run)
    assert not (run / 'worker-auth-result.txt').exists() and not (run / 'helper-auth-result.txt').exists()
    cipher_path = root / 'credentials' / 'token.dpapi'
    cipher = cipher_path.read_bytes()
    try:
        cipher_path.write_bytes(b'invalid DPAPI fixture')
        run_ps('-File', script, '-Mode', 'Check', '-RunDirectory', run, expected=1)
        run_ps('-File', script, '-Mode', 'Launch', '-RunDirectory', run,
               '-AllowPhysicalOutput', '-PhysicalConfirm', 'LINEUP_TEST_F8_F9', expected=1)
        assert not (run / 'helper-auth-result.txt').exists(), '认证失败时不应留下网页服务'
    finally:
        cipher_path.write_bytes(cipher)
    run_ps('-File', script, '-Mode', 'Launch', '-RunDirectory', run,
           '-AllowPhysicalOutput', '-PhysicalConfirm', 'LINEUP_TEST_F8_F9')
    assert (run / 'worker-auth-result.txt').read_text() == 'matched', '实际 Worker 子进程缺少已有源状态凭据'
    assert (run / 'helper-auth-result.txt').read_text() == 'absent', '网页助手不应得到新加载的凭据'
    assert Path((run / 'worker-root-result.txt').read_text()) == run
    assert config.read_bytes() == original
    for name in ('task.json', 'TASK.md', 'config.ini', 'lineup-stdout.log', 'lineup-stderr.log'):
        assert b'x' * 64 not in (run / name).read_bytes(), f'{name} 泄露了测试认证值'
    # 旧 Run 缺少新字段时仍允许继承正式入口已有的环境，不要求重新安装凭据。
    task.pop('credential_directory')
    task.pop('credential_scope')
    task_path.write_text(json.dumps(task, ensure_ascii=False), encoding='utf-8')
    run_ps('-File', script, '-Mode', 'Check', '-RunDirectory', run, expected=1)
    inherited = dict(environment, XEN_SOURCE_CONTEXT_TOKEN='x' * 64)
    wrapper = root / 'inherited-launch.ps1'
    wrapper.write_text(r'''
param([string]$Entry, [string]$Run)
$ErrorActionPreference = 'Stop'
& $Entry -Mode Launch -RunDirectory $Run -AllowPhysicalOutput -PhysicalConfirm LINEUP_TEST_F8_F9
if ($env:XEN_SOURCE_CONTEXT_TOKEN -cne ('x' * 64)) { throw '调用者认证环境没有恢复' }
''', encoding='utf-8-sig')
    (run / 'worker-auth-result.txt').write_text('not_run')
    run_ps('-File', wrapper, '-Entry', script, '-Run', run, child_environment=inherited)
    assert (run / 'worker-auth-result.txt').read_text() == 'matched'
    assert (run / 'helper-auth-result.txt').read_text() == 'absent'


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
        verify_auth_launch(args, script, folder)
    print('独立配置/硬链接/原配置不变/预检/授权拒绝/已有DPAPI子进程传递/旧Run继承/网页凭据隔离/失败清理全部通过；只启动无设备夹具')


if __name__ == '__main__':
    main()
