"""验证离线面板生产函数；只用本地配置与假 Host，不显示 GUI 或注册热键。"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--powershell', required=True)
    args = parser.parse_args()
    panel = Path(__file__).resolve().parents[1] / 'scripts' / 'lineup_capture_panel.ps1'
    assert panel.read_bytes().startswith(b'\xef\xbb\xbf'), 'PowerShell 5 中文入口必须保留 UTF-8 BOM'
    with tempfile.TemporaryDirectory(prefix='xen-panel-') as folder:
        root = Path(folder)
        library = root / '中文 空格 & (采集)'
        library.mkdir()
        exe = root / '只读 替身.exe'
        exe.write_bytes(b'CheckOnly must never execute this file')
        options_file = root / 'options.json'
        action = {'schema': 1, 'type': 'phases', 'movement_profile': 'stationary', 'phases': [
            {'buttons': ['left'], 'movement': [], 'jump': False, 'duration_ms': None},
            {'buttons': [], 'movement': [], 'jump': False, 'duration_ms': None}]}
        w_jump = {'schema': 1, 'type': 'phases', 'movement_profile': 'step', 'phases': [
            {'buttons': ['left'], 'movement': [], 'jump': False, 'duration_ms': None},
            {'buttons': ['left'], 'movement': ['forward'], 'jump': True, 'duration_ms': None},
            {'buttons': [], 'movement': ['forward'], 'jump': True, 'duration_ms': None},
            {'buttons': [], 'movement': [], 'jump': False, 'duration_ms': None}]}
        options = {'schema': 1,
                   'maps': [{'id': 'de_dust2', 'text': '炙热沙城 2'}],
                   'teams': [{'id': 'CT', 'text': 'CT'}, {'id': 'T', 'text': 'T'}],
                   'grenades': [{'id': '烟雾弹', 'text': '烟雾弹'}],
                   'methods': [{'id': 'stationary', 'text': '原地投掷'}, {'id': 'step', 'text': '一步投掷'},
                               {'id': 'w_left_jump', 'text': 'W＋左键跳投（同步起跳）',
                                'fixed_buttons': 'left', 'fixed_direction': 'forward'}],
                   'buttons': [{'id': 'left', 'text': '左键'}, {'id': 'right', 'text': '右键'}],
                   'directions': [{'id': 'forward', 'text': '前'}, {'id': 'left', 'text': '左'}],
                   'presets': [{'method': 'stationary', 'buttons': 'left', 'direction': 'forward',
                                'throw_instructions': '原地左键；时长待填写', 'throw_action': action},
                               {'method': 'w_left_jump', 'buttons': 'left', 'direction': 'forward',
                                'throw_instructions': 'W 与跳跃同一阶段，随后松左键；时长待填写', 'throw_action': w_jump}]}
        write_json(options_file, options)
        config_file = library / 'config.json'
        config = {'schema': 1, 'source_id': '本机逻辑来源', 'source_width': 640, 'source_height': 480,
                  'sync_script': str(root / '未启动的同步.ps1'), 'sync_inbox': r'\\invalid.invalid\never-access\inbox'}
        write_json(config_file, config)
        initial_files = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*') if p.is_file()}

        def invoke(*extra, expected=0, lib=library, program=exe, options_path=options_file):
            result = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(panel), '-LibraryDirectory', str(lib),
                                     '-Executable', str(program), '-OptionsPath', str(options_path),
                                     '-CheckOnly', *map(str, extra)], capture_output=True, timeout=20)
            assert (result.returncode == 0) == (expected == 0), (result.returncode, result.stdout, result.stderr)
            return result

        invoke()
        assert initial_files == {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*') if p.is_file()}
        assert sorted(p.name for p in library.iterdir()) == ['config.json']
        for key in ('lib', 'program', 'options_path'):
            invoke(expected=1, **{key: r'\\invalid.invalid\never-access\file'})
        invoke(expected=1, lib='relative-library')
        write_json(config_file, dict(config, source_width=319))
        invoke(expected=1)
        write_json(config_file, dict(config, source_height='480'))
        invoke(expected=1)
        write_json(config_file, config)
        action['phases'][0]['duration_ms'] = 50
        write_json(options_file, options)
        invoke(expected=1)
        action['phases'][0]['duration_ms'] = None
        write_json(options_file, options)
        options['presets'][1]['buttons'] = 'right'
        write_json(options_file, options)
        invoke(expected=1)
        options['presets'][1]['buttons'] = 'left'
        write_json(options_file, options)

        selection = {'map': 'de_dust2', 'team': 'CT', 'grenade': '烟雾弹', 'method': 'stationary',
                     'buttons': 'left', 'direction': 'left', 'name': '中文参考', 'instructions': '背靠箱子；手动站定',
                     'source_id': '不可覆盖本地来源', 'source_width': 9999}
        selection_file = root / 'selection.json'
        write_json(selection_file, selection)
        harness = root / 'check-functions.ps1'
        harness.write_text(r"""param($Panel, $Library, $Executable, $Options, $SelectionFile)
$ErrorActionPreference = 'Stop'
$null = . $Panel -LibraryDirectory $Library -Executable $Executable -OptionsPath $Options -CheckOnly
function Assert-Panel([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$state = $script:LineupPanelState
$empty = Get-LineupSelection $state
Assert-Panel ($empty.map -eq '' -and $empty.team -eq '' -and $empty.grenade -eq '') '首次启动不得默认地图阵营道具'
Assert-Panel (-not [bool]('System.Windows.Forms.Form' -as [type])) 'CheckOnly 不得加载 WinForms'
$selection = Get-Content -LiteralPath $SelectionFile -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($field in @('map','team','grenade','method','buttons')) {
    $candidate = $selection | ConvertTo-Json | ConvertFrom-Json
    $candidate.$field = ''
    $rejected = $false
    try { Save-LineupCaptureProfile $state $candidate } catch { $rejected = $true }
    Assert-Panel $rejected "空选择应拒绝：$field"
    Assert-Panel (-not [IO.File]::Exists($state.ProfilePath)) '非法设置不得写入 profile'
}
Save-LineupCaptureProfile $state $selection
$saved = Get-Content -LiteralPath $state.ProfilePath -Encoding UTF8 -Raw | ConvertFrom-Json
Assert-Panel ($saved.source_id -eq $state.Config.source_id -and $saved.source_width -eq 640) '来源由本地配置确定'
Assert-Panel ($saved.panel_selection.direction -eq 'forward') '原地预设必须归一为前方'
Assert-Panel ($saved.target -eq '当前瞄点' -and $saved.instructions -eq $selection.instructions) '靠位说明不得代替目标字段'
Assert-Panel ($null -eq $saved.throw_action.phases[0].duration_ms) '不得生成投掷时序'
$selection.name = '第二次原子替换'
Save-LineupCaptureProfile $state $selection
$restoredState = New-LineupPanelState $Library $Executable $Options
$restored = Get-LineupSelection $restoredState
Assert-Panel ($restored.name -eq $selection.name -and $restored.team -eq 'CT' -and $restored.method -eq 'stationary') '完整设置应恢复'
Assert-Panel (@(Get-ChildItem -LiteralPath $Library -Filter '*.tmp').Count -eq 0) '原子保存不留临时文件'
$old = Get-Content -LiteralPath $state.ProfilePath -Encoding UTF8 -Raw | ConvertFrom-Json
$old.PSObject.Properties.Remove('panel_selection')
$old.throw_action = [pscustomobject]@{phases=$old.throw_action.phases; type='phases'; schema=1; movement_profile='stationary'}
$state.Profile = $old
$legacy = Get-LineupSelection $state
Assert-Panel ($legacy.method -eq 'stationary' -and $legacy.buttons -eq 'left') '旧 profile 的动作对象顺序不影响预设恢复'
$wSelection = $selection | ConvertTo-Json | ConvertFrom-Json
$wSelection.method = 'w_left_jump'; $wSelection.direction = 'forward'
Save-LineupCaptureProfile $state $wSelection
$wState = New-LineupPanelState $Library $Executable $Options
$wRestored = Get-LineupSelection $wState
Assert-Panel ($wRestored.method -eq 'w_left_jump' -and $wRestored.buttons -eq 'left' -and $wRestored.direction -eq 'forward') 'W 同步跳投的面板选择应完整恢复'
$wPreset = $state.Options.presets | Where-Object { $_.method -eq 'w_left_jump' }
Assert-Panel ((ConvertTo-LineupCanonicalJson $wState.Profile.throw_action) -ceq (ConvertTo-LineupCanonicalJson $wPreset.throw_action)) 'W 同步跳投应原样保存共享四阶段与空时长'
Assert-Panel ($wState.Profile.throw_action.phases.Count -eq 4 -and $null -eq $wState.Profile.throw_action.phases[1].duration_ms) '四阶段不得退化或猜测时序'
$wState.Profile.PSObject.Properties.Remove('panel_selection')
Assert-Panel ((Get-LineupSelection $wState).method -eq 'w_left_jump') '无面板字段时仍由完整四阶段恢复独立预设'
$beforeInvalid = [IO.File]::ReadAllText($state.ProfilePath)
foreach ($change in @(@('buttons','right'), @('direction','left'))) {
    $candidate = $wSelection | ConvertTo-Json | ConvertFrom-Json
    $candidate.($change[0]) = $change[1]
    $rejected = $false
    try { Save-LineupCaptureProfile $state $candidate } catch { $rejected = $true }
    Assert-Panel $rejected 'W 同步跳投拒绝右键或侧向组合'
    Assert-Panel ([IO.File]::ReadAllText($state.ProfilePath) -ceq $beforeInvalid) '非法组合不得改写既有设置'
}
# 只创建未显示的控件，复用真实联动函数，不创建窗体或消息循环。
Add-Type -AssemblyName System.Windows.Forms
$controls = @{}
try {
    foreach ($mapping in @(@('method','methods'), @('buttons','buttons'), @('direction','directions'))) {
        $combo = [Windows.Forms.ComboBox]::new()
        foreach ($choice in $state.Options.($mapping[1])) { $null = $combo.Items.Add($choice) }
        $controls[$mapping[0]] = $combo
    }
    $controls.method.SelectedIndex = 2; $controls.buttons.SelectedIndex = 1; $controls.direction.SelectedIndex = 1
    Set-LineupMethodControls $controls $false
    Assert-Panel ($controls.buttons.SelectedItem.id -eq 'left' -and $controls.direction.SelectedItem.id -eq 'forward') '选择固定预设时自动改为左键向前'
    Assert-Panel (-not $controls.buttons.Enabled -and -not $controls.direction.Enabled) '固定预设的两个控件应禁用'
    $controls.method.SelectedIndex = 1
    Set-LineupMethodControls $controls $false
    Assert-Panel ($controls.buttons.Enabled -and $controls.direction.Enabled) '切回普通移动投掷恢复选择'
    Set-LineupMethodControls $controls $true
    Assert-Panel (-not $controls.buttons.Enabled -and -not $controls.direction.Enabled) '采集中继续锁定两个控件'
} finally { foreach ($control in $controls.Values) { $control.Dispose() } }
Save-LineupCaptureProfile $state $selection
$configPath = Join-Path $Library 'config.json'
$config = Get-Content -LiteralPath $configPath -Encoding UTF8 -Raw | ConvertFrom-Json
$config | Add-Member -MemberType NoteProperty -Name default_profile -Value ([pscustomobject]@{map='de_dust2'; team='T'; grenade='烟雾弹'})
[IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
$priority = Get-LineupSelection (New-LineupPanelState $Library $Executable $Options)
Assert-Panel ($priority.team -eq 'CT') '已存 profile 优先于默认设置'
$localLink = Join-Path (Split-Path $Library) 'local-junction'
$null = New-Item -ItemType Junction -Path $localLink -Target $Library
try {
    $rejected = $false
    try { $null = New-LineupPanelState $localLink $Executable $Options } catch { $rejected = $true }
    Assert-Panel $rejected '本地重解析目录必须拒绝'
} finally { [IO.Directory]::Delete($localLink) }
$capturePath = Join-Path $Library 'captures'
$null = [IO.Directory]::CreateDirectory($capturePath)
foreach ($id in @('saved_one', '.pending-write', 'incomplete')) {
    $path = Join-Path $capturePath $id
    $null = [IO.Directory]::CreateDirectory($path)
    foreach ($file in @('capture.json','full.png','roi.png','overview.png')) {
        if ($id -ne 'incomplete' -or $file -ne 'overview.png') { [IO.File]::WriteAllText((Join-Path $path $file), 'fixture') }
    }
}
$summary = Get-LineupCaptureSummary $state
Assert-Panel ($summary.Count -eq 1 -and $summary.Latest -eq 'saved_one') '只统计最终目录中的完整四件套'
[IO.File]::Delete($Executable)
$source = @'
using System;
using System.IO;
using System.Threading;
public static class PanelFakeHost {
    public static void Main(string[] args) {
        if (args.Length != 2 || args[0] != "--local-library") throw new ArgumentException("unexpected arguments");
        Console.OutputEncoding = new System.Text.UTF8Encoding(false);
        Console.WriteLine("旧日志：已注册 F7");
        Console.WriteLine("F7 未完成：CS2 需要在前台，中文失败原因应可见");
        Console.Out.Flush();
        File.WriteAllLines(Path.Combine(args[1], "fake-host-args.txt"), args);
        Thread.Sleep(60000);
    }
}
'@
Add-Type -TypeDefinition $source -OutputAssembly $Executable -OutputType ConsoleApplication
$otherLibrary = Join-Path (Split-Path $Library) 'other-child'
$null = [IO.Directory]::CreateDirectory($otherLibrary)
$other = Start-Process -FilePath $Executable -ArgumentList ('--local-library ' + (ConvertTo-LineupArgument $otherLibrary)) -PassThru -WindowStyle Hidden
$previousEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [Text.Encoding]::GetEncoding(936)
    Start-LineupCapture $state $selection
    $owned = $state.CaptureProcess
    $marker = Join-Path $Library 'fake-host-args.txt'
    for ($attempt = 0; $attempt -lt 40 -and -not [IO.File]::Exists($marker); $attempt++) { Start-Sleep -Milliseconds 50 }
    Assert-Panel (Test-LineupProcess $owned) '假 Host 应保持运行'
    $latest = ''
    for ($attempt = 0; $attempt -lt 40 -and -not $latest.Contains('F7 未完成'); $attempt++) {
        $latest = Get-LineupCaptureLog $state
        if (-not $latest.Contains('F7 未完成')) { Start-Sleep -Milliseconds 50 }
    }
    Assert-Panel ($latest -ceq 'F7 未完成：CS2 需要在前台，中文失败原因应可见') '运行中应读取最新一条中文失败日志，不拼接旧行'
    $errorLog = Join-Path $Library 'fake-error.log'
    [IO.File]::WriteAllText($errorLog, "旧错误`r`n新的错误通道：保存目录不可写`r`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::SetLastWriteTimeUtc($errorLog, [DateTime]::UtcNow.AddMinutes(1))
    $originalError = $state.CaptureErr; $state.CaptureErr = $errorLog
    Assert-Panel ((Get-LineupCaptureLog $state) -ceq '新的错误通道：保存目录不可写') '较新的错误通道也应显示最后一行'
    $state.CaptureErr = $originalError
    $actual = [IO.File]::ReadAllLines($marker)
    Assert-Panel ($actual.Count -eq 2 -and $actual[1] -ceq $Library) '含中文空格和符号的路径应完整传给 Host'
    $before = [IO.File]::ReadAllText($state.ProfilePath)
    $rejected = $false
    try { Save-LineupCaptureProfile $state $selection } catch { $rejected = $true }
    Assert-Panel $rejected '采集中不得修改配置'
    $rejected = $false
    try { Start-LineupCapture $state $selection } catch { $rejected = $true }
    Assert-Panel $rejected '不得重复启动自己的 Host'
    Assert-Panel ([IO.File]::ReadAllText($state.ProfilePath) -ceq $before) '锁定期间完整配置保持不变'
    $ownedId = $owned.Id
    Stop-LineupCapture $state
    Assert-Panel ($null -eq $state.CaptureProcess) '停止后释放本面板的进程引用'
    Assert-Panel ($null -eq (Get-Process -Id $ownedId -ErrorAction SilentlyContinue)) '本面板 Host 已退出'
    Assert-Panel (Test-LineupProcess $other) '不得结束同名其它 Host'
    Stop-LineupCapture $state
} finally {
    [Console]::OutputEncoding = $previousEncoding
    Stop-LineupCapture $state
    if (-not $other.HasExited) { $other.Kill(); $null = $other.WaitForExit(3000) }
    $other.Dispose()
}
$syncScript = Join-Path (Split-Path $Library) '仅本地同步替身.ps1'
$syncBody = 'param($LibraryDirectory,$InboxDirectory); [IO.File]::WriteAllText((Join-Path $LibraryDirectory ''fake-sync.txt''),$InboxDirectory); Write-Output ''fake sync complete'''
[IO.File]::WriteAllText($syncScript, $syncBody, [Text.UTF8Encoding]::new($true))
$state.Config.sync_script = $syncScript
Assert-Panel (-not [IO.File]::Exists((Join-Path $Library 'fake-sync.txt'))) '此前不得自动触发发送'
Start-LineupSync $state
Assert-Panel ($state.SyncProcess.WaitForExit(5000)) '同步替身应正常结束'
Assert-Panel ($state.SyncProcess.ExitCode -eq 0) ('同步替身应成功，实际：' + $state.SyncProcess.ExitCode + '; ' + (Read-LineupLogTail $state.SyncErr))
Assert-Panel ([IO.File]::ReadAllText((Join-Path $Library 'fake-sync.txt')) -ceq $state.Config.sync_inbox) '显式发送才传递辅机字符串'
$state.SyncProcess.Dispose(); $state.SyncProcess = $null
Write-Output 'PANEL_FUNCTIONS_OK'
""", encoding='utf-8-sig')
        result = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(harness), '-Panel', str(panel), '-Library', str(library),
                                 '-Executable', str(exe), '-Options', str(options_file),
                                 '-SelectionFile', str(selection_file)], capture_output=True, timeout=45)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert b'PANEL_FUNCTIONS_OK' in result.stdout
        saved = json.loads((library / 'profile.json').read_text(encoding='utf-8'))
        assert saved['source_id'] == config['source_id'] and saved['source_height'] == 480
        assert all(phase['duration_ms'] is None for phase in saved['throw_action']['phases'])
    print('PS5 只读预检/固定盘与重解析拒绝/原子保存恢复/W 同步跳投四阶段与固定选项/假 Host 所有权/运行中文日志/显式发送全部通过；未显示 GUI、采集或启动 Runtime')


if __name__ == '__main__':
    main()
