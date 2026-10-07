param(
    [ValidateSet('Prepare', 'Check', 'Launch')][string]$Mode = 'Prepare',
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [string]$XenRoot,
    [string]$WorkerExecutable,
    [string]$LineupExecutable,
    [string]$WebDirectory,
    [string]$BindAddress = '127.0.0.1',
    [ValidateRange(1024, 65535)][int]$Port = 8879,
    [string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalConfirm
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# 从 PowerShell 7 调用 Windows PowerShell 5 时，优先使用当前宿主自带模块。
$env:PSModulePath = (Join-Path $PSHOME 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
$utf8 = New-Object Text.UTF8Encoding($false)
$run = [IO.Path]::GetFullPath($RunDirectory)

function Read-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Fallback = '') {
    $inside = $false
    $value = $Fallback
    foreach ($line in [regex]::Split($Text, '\r?\n')) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') { $inside = $Matches[1] -ieq $Section }
        elseif ($inside -and $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$')) { $value = $Matches[1] }
    }
    return $value
}
function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    $lines = New-Object 'Collections.Generic.List[string]'
    $inside = $false; $sectionFound = $false; $written = $false
    foreach ($line in [regex]::Split($Text, '\r?\n')) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            if ($inside -and -not $written) { $lines.Add("$Key=$Value"); $written = $true }
            $inside = $Matches[1] -ieq $Section
            if ($inside) { $sectionFound = $true }
        }
        if ($inside -and $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
            if (-not $written) { $lines.Add("$Key=$Value"); $written = $true }
        } else { $lines.Add($line) }
    }
    if (-not $sectionFound) { $lines.Add("`n[$Section]") }
    if (-not $written) { $lines.Add("$Key=$Value") }
    return ($lines -join "`r`n")
}
function Require-File([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "文件不存在：$Path" }
    return [IO.Path]::GetFullPath($Path)
}
function Read-Check([string]$Binary, [string]$Config, [string]$Inbox, [string]$ListenAddress, [int]$ListenPort) {
    $result = & $Binary --config $Config --capture-inbox $Inbox --bind $ListenAddress --port $ListenPort --check-config
    if ($LASTEXITCODE -ne 0) { throw '测试配置预检失败；没有启动任何设备。' }
    $state = $result | ConvertFrom-Json
    if (-not $state.test_mode -or $state.locate_virtual_key -ne 119 -or $state.throw_virtual_key -ne 120) {
        throw '二进制或测试配置不支持独立 F8/F9 入口；请使用配对的新版本。'
    }
    return $state
}
function New-LineupWorkerStartInfo([string]$Binary, [string]$WorkingDirectory,
    [bool]$SourceContextEnabled, [string]$Credentials, [string]$ProtectionScope) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Binary
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
    $info.EnvironmentVariables['XEN_RELEASE_ROOT'] = $WorkingDirectory
    $info.EnvironmentVariables['XEN_RUNTIME_ID'] = 'nvidia'
    $info.EnvironmentVariables['XEN_RELEASE_BACKENDS'] = 'cpu,cuda,tensorrt'
    $plainBytes = $null
    try {
        if ($SourceContextEnabled) {
            if ($Credentials) {
                $credentialPath = Require-File (Join-Path $Credentials 'token.dpapi')
                for ($part = $credentialPath; $part; $part = [IO.Path]::GetDirectoryName($part)) {
                    if (([IO.File]::GetAttributes($part) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                        throw '已有凭据路径包含重解析点，请沿用正式入口的凭据目录。'
                    }
                }
                Add-Type -AssemblyName System.Security
                $scopeValue = [Security.Cryptography.DataProtectionScope]::$ProtectionScope
                try {
                    $plainBytes = [Security.Cryptography.ProtectedData]::Unprotect(
                        [IO.File]::ReadAllBytes($credentialPath), $null, $scopeValue)
                } catch { throw '无法读取已有源状态凭据，请核对正式入口的凭据目录、保护范围和登录用户。' }
                # 沿用正式入口：明文只进入指定 Worker 的环境，不写入父环境、INI 或 Run。
                $info.EnvironmentVariables['XEN_SOURCE_CONTEXT_TOKEN'] = [Text.Encoding]::UTF8.GetString($plainBytes)
            }
            $length = ([string]$info.EnvironmentVariables['XEN_SOURCE_CONTEXT_TOKEN']).Length
            if ($length -lt 32 -or $length -gt 1024) {
                throw '测试入口未取得已有源状态凭据；请沿用正式入口的 CredentialDirectory 和 Scope，无需重新配对。'
            }
        }
        return $info
    } catch {
        $info.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
        throw
    } finally {
        if ($null -ne $plainBytes) { [Array]::Clear($plainBytes, 0, $plainBytes.Length) }
    }
}

if ($Mode -eq 'Prepare') {
    if (-not $XenRoot) { throw 'Prepare 需要指定现有 XenRoot。' }
    $root = [IO.Path]::GetFullPath($XenRoot)
    $sourceConfig = Require-File (Join-Path $root 'config.ini')
    $worker = Require-File $WorkerExecutable
    $lineup = Require-File $LineupExecutable
    if (-not $WebDirectory) { $WebDirectory = Join-Path (Split-Path -Parent $lineup) 'lineup-web' }
    $web = [IO.Path]::GetFullPath($WebDirectory)
    [void](Require-File (Join-Path $web 'index.html'))
    if (Test-Path -LiteralPath $run) { throw 'RunDirectory 已存在，请使用新目录；不覆盖已有测试或正式配置。' }
    $original = [IO.File]::ReadAllText($sourceConfig)
    $sourceHash = (Get-FileHash -LiteralPath $sourceConfig -Algorithm SHA256).Hash
    $modelName = [IO.Path]::GetFileName((Read-IniValue $original 'detector' 'model_path'))
    if (-not $modelName -or [IO.Path]::GetExtension($modelName) -ine '.onnx') { throw '原配置没有明确 ONNX 模型。' }
    $model = Require-File (Join-Path (Join-Path $root 'models') $modelName)
    $backend = (Read-IniValue $original 'detector' 'backend' 'cpu').ToLowerInvariant()
    if ($backend -notin @('cpu','cuda','tensorrt')) { throw '本入口当前仅准备已验证构建的 NVIDIA 运行库。' }
    # 单独配置只改变测试所需项目；不改游戏、NDI、GSI、标定内容及正式 INI。
    $candidate = $original
    foreach ($setting in @(
        @('lineup','test_mode','true'), @('lineup','locate_virtual_key','119'), @('lineup','throw_virtual_key','120'),
        @('keyboard','runtime_toggle_virtual_keys',''), @('keyboard','anomaly_mark_virtual_keys',''),
        @('keyboard','debug_test_enabled','false'), @('keyboard','debug_test_virtual_keys',''),
        @('keyboard','aim_hold_virtual_keys','135'), @('keyboard','emergency_virtual_keys','35'),
        @('auto_stop','enabled','false'), @('auto_stop','cycle_enabled','false'),
        @('trigger','enabled','false'), @('trigger','fire_enabled','false'),
        @('recoil','enabled','false'), @('movement','enabled','false'), @('mouse','allow_send_input','true'),
        @('log','log_dir',(Join-Path $run 'logs'))
    )) { $candidate = Set-IniValue $candidate $setting[0] $setting[1] $setting[2] }
    foreach ($item in @(@('lineup','calibration_file'), @('weapon_timing','file'), @('recoil','profile_directory'))) {
        $value = Read-IniValue $candidate $item[0] $item[1]
        if ($value -and -not [IO.Path]::IsPathRooted($value)) {
            $candidate = Set-IniValue $candidate $item[0] $item[1] ([IO.Path]::GetFullPath((Join-Path $root $value)))
        }
    }
    [void][IO.Directory]::CreateDirectory($run)
    $config = Join-Path $run 'config.ini'
    $inbox = Join-Path $run 'inbox'
    [IO.File]::WriteAllText($config, $candidate, $utf8)
    $checked = Read-Check $lineup $config $inbox $BindAddress $Port
    [void][IO.Directory]::CreateDirectory((Join-Path $run 'models'))
    # 仅硬链接所选现有模型，避免复制或再次哈希未变化的大文件；跨卷明确拒绝。
    [void](New-Item -ItemType HardLink -Path (Join-Path (Join-Path $run 'models') $modelName) -Value $model)
    [void][IO.Directory]::CreateDirectory($inbox)
    [void][IO.Directory]::CreateDirectory((Join-Path $inbox 'captures'))
    $task = [ordered]@{
        schema = 1; task_id = 'GRENADE-HOST-CAPTURE-20261007-001'; state = 'PREPARED_NOT_LAUNCHED'
        source_config = $sourceConfig; source_config_sha256 = $sourceHash
        worker = $worker; worker_sha256 = (Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash
        lineup = $lineup; lineup_sha256 = (Get-FileHash -LiteralPath $lineup -Algorithm SHA256).Hash
        config = $config; config_sha256 = (Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
        web = $web; bind = $BindAddress; port = $Port; inbox = $inbox
        credential_directory = $(if ($CredentialDirectory) { [IO.Path]::GetFullPath($CredentialDirectory) } else { '' })
        credential_scope = $Scope
        calibration_configured = [bool]$checked.calibration_configured
        real_verified = $false
    }
    [IO.File]::WriteAllText((Join-Path $run 'task.json'), ($task | ConvertTo-Json -Depth 8), $utf8)
    $entry = $PSCommandPath.Replace("'", "''"); $quotedRun = $run.Replace("'", "''")
    $launch = "powershell -NoProfile -ExecutionPolicy Bypass -File '$entry' -Mode Launch -RunDirectory '$quotedRun' -AllowPhysicalOutput -PhysicalConfirm LINEUP_TEST_F8_F9"
    $taskText = @"
# 道具助手独立测试

状态：PREPARED_NOT_LAUNCHED。启动后由用户在 Xen 界面开始运行并武装；按键才产生对应意图。

唯一辅机 Launch 命令（会允许真实 KMBOX 输入）：

``````powershell
$launch
``````

网页：http://${BindAddress}:$Port 。先在“采集与整理”选择地图、CT/T、道具和投掷方式，保存 F7 采集设置。
主机 F7 保存全屏/原始320/标框图；辅机导入并选中后，站位不变、稍移鼠标，F8只回准，F9回准后单次投掷。End急停。
主机采集收件目录：$inbox（主机通过现有 SMB 对应路径访问）。
F9各阶段时长必须明确填写；标定必须为已量测并审核的数据，当前是否配置：$($checked.calibration_configured)。配置存在不等于有效。
参考移出中心320且无重叠、纹理不足、源时钟/几何/标定不满足时停止并报告，不盲搜。
正式 INI 不变。已禁用其他辅助功能；测试 Aim 按住键为 F24，仅为满足既有配置合同，请勿触发。
源状态桥接沿用正式入口的凭据目录和保护范围；已有认证只在启动时传给测试 Runtime，不需重新配对。
关闭测试 Runtime 后启动器会停止本次创建的 Lineup 服务。实际精度与落点由本轮人工结果确认。
"@
    [IO.File]::WriteAllText((Join-Path $run 'TASK.md'), $taskText, $utf8)
    [IO.File]::WriteAllText((Join-Path $run 'OBSERVATION.md'), "# 人工观察`n`n尚未执行。用户反馈后按原意记录。`n", $utf8)
    if ((Get-FileHash -LiteralPath $sourceConfig -Algorithm SHA256).Hash -ne $sourceHash) { throw '准备期间正式配置发生变化；请核对，不启动。' }
    Write-Host "已准备：$run；未启动采集、Runtime 或物理输出。"
    Write-Host "标定路径已配置：$($checked.calibration_configured)；有效性由实际 Runtime 核对。"
    return
}

$taskPath = Require-File (Join-Path $run 'task.json')
$task = [IO.File]::ReadAllText($taskPath) | ConvertFrom-Json
if ($task.schema -ne 1 -or $task.task_id -ne 'GRENADE-HOST-CAPTURE-20261007-001' -or
    [IO.Path]::GetFullPath($task.config) -ne (Join-Path $run 'config.ini')) { throw 'Run 身份或配置路径不匹配。' }
foreach ($field in @('worker','lineup','config')) {
    $path = Require-File ([string]$task.$field)
    $hashField = $field + '_sha256'
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $task.$hashField) { throw "本次 $field 已变化，请重新 Prepare。" }
}
$checked = Read-Check $task.lineup $task.config $task.inbox $task.bind $task.port
$sourceContextEnabled = (Read-IniValue ([IO.File]::ReadAllText($task.config)) 'source_context' 'enabled' 'false') -in @('true', '1', 'yes', 'on')
if (-not $PSBoundParameters.ContainsKey('CredentialDirectory') -and $task.PSObject.Properties['credential_directory']) {
    $CredentialDirectory = [string]$task.credential_directory
}
if (-not $PSBoundParameters.ContainsKey('Scope') -and $task.PSObject.Properties['credential_scope']) {
    $Scope = [string]$task.credential_scope
}
if ($Scope -notin @('CurrentUser', 'LocalMachine')) { throw '凭据保护范围无效，请沿用正式入口的 Scope。' }
if ($Mode -eq 'Check') {
    $checkInfo = New-LineupWorkerStartInfo $task.worker $run $sourceContextEnabled $CredentialDirectory $Scope
    $checkInfo.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
    Write-Host "测试入口及已有凭据预检通过；标定路径已配置：$($checked.calibration_configured)。未启动 Runtime 或设备。"
    return
}
if (-not $AllowPhysicalOutput -or $PhysicalConfirm -cne 'LINEUP_TEST_F8_F9') { throw 'Launch 必须由用户前台提供 -AllowPhysicalOutput 和 -PhysicalConfirm LINEUP_TEST_F8_F9。' }
if (-not [Environment]::UserInteractive) { throw 'Launch 需要用户交互会话。' }
$workerName = [IO.Path]::GetFileNameWithoutExtension([string]$task.worker)
$lineupName = [IO.Path]::GetFileNameWithoutExtension([string]$task.lineup)
foreach ($name in @('Xen', 'XenLauncher', 'XenLineup', $workerName, $lineupName) | Select-Object -Unique) {
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) { throw "已有 $name 正在运行，请先由用户关闭，避免同设备竞争。" }
}
$oldEnvironment = @{}
foreach ($key in @('XEN_RELEASE_ROOT','XEN_RUNTIME_ID','XEN_RELEASE_BACKENDS','XEN_SOURCE_CONTEXT_TOKEN')) { $oldEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
$helper = $null
$workerInfo = $null
try {
    $env:XEN_RELEASE_ROOT = $run; $env:XEN_RUNTIME_ID = 'nvidia'; $env:XEN_RELEASE_BACKENDS = 'cpu,cuda,tensorrt'
    $workerInfo = New-LineupWorkerStartInfo $task.worker $run $sourceContextEnabled $CredentialDirectory $Scope
    # 即使调用者已有认证环境，网页服务也不继承；Worker 使用上面独立的环境副本。
    [Environment]::SetEnvironmentVariable('XEN_SOURCE_CONTEXT_TOKEN', $null, 'Process')
    $helperArgs = @('--config', ('"' + $task.config + '"'), '--data', ('"' + (Join-Path $run 'lineup-data') + '"'),
        '--capture-inbox', ('"' + $task.inbox + '"'), '--web', ('"' + $task.web + '"'), '--bind', $task.bind, '--port', [string]$task.port)
    $helper = Start-Process -FilePath $task.lineup -ArgumentList $helperArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $run 'lineup-stdout.log') -RedirectStandardError (Join-Path $run 'lineup-stderr.log')
    Start-Sleep -Milliseconds 500
    if ($helper.HasExited) { throw 'Lineup 服务启动失败，请查看本次 Run 的 lineup-stderr.log。' }
    # 这是供用户操作开始/武装/急停的交互窗口。
    $worker = [Diagnostics.Process]::Start($workerInfo)
    $workerInfo.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
    $task.state = 'USER_LAUNCHED'
    [IO.File]::WriteAllText($taskPath, ($task | ConvertTo-Json -Depth 8), $utf8)
    Write-Host "请在测试 Xen 窗口开始并武装；设置网页 http://$($task.bind):$($task.port) 。End 急停。"
    $worker.WaitForExit()
} finally {
    if ($null -ne $workerInfo) { $workerInfo.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN') }
    if ($helper -and -not $helper.HasExited) { Stop-Process -Id $helper.Id }
    foreach ($key in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $oldEnvironment[$key], 'Process') }
}
