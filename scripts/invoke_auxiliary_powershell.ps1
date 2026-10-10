param(
    [string]$SshHost = 'xen-aux',
    [string]$ScriptPath,
    [switch]$RequireAdministrator,
    [ValidateSet('Execute', 'Start', 'Status', 'Stop', 'Recover')][string]$Mode = 'Execute',
    [string]$RunDirectory,
    [string]$InteractiveUser,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 一次性任务只负责把已有脚本放进桌面会话，不接管设备协议。
# 有输出的脚本须自行有界，并处理 XEN_AUXILIARY_STOP_FILE 后正常关闭设备。
function Invoke-XenAuxiliaryTask($Request) {
    $mode = [string]$Request.mode
    $run = [string]$Request.run_directory
    if ($run -notmatch '^[A-Za-z]:[\\/]') { throw 'RunDirectory 必须是辅机本地绝对路径。' }
    $run = [IO.Path]::GetFullPath($run).TrimEnd('\', '/')
    if ($run -eq [IO.Path]::GetPathRoot($run).TrimEnd('\')) { throw 'RunDirectory 不能是磁盘根目录。' }
    for ($part = $run; $part; $part = [IO.Path]::GetDirectoryName($part)) {
        if ((Test-Path -LiteralPath $part) -and
            ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw '远程任务路径不能包含重解析点。'
        }
    }
    $control = Join-Path $run '.auxiliary'
    $bindingPath = Join-Path $control 'task.json'
    $resultPath = Join-Path $control 'result.json'
    $stopPath = Join-Path $control 'STOP'
    if ($mode -eq 'Start' -and -not (Test-Path -LiteralPath $bindingPath)) {
        $user = [string]$Request.interactive_user
        if (-not $user) { $user = [string](Get-CimInstance Win32_ComputerSystem).UserName }
        if (-not $user) { throw '辅机没有已登录桌面用户，无法启动交互任务。' }
        $null = [IO.Directory]::CreateDirectory($control)
        # CreateNew 使重试与并发请求只留下一个启动决定；失败也不自动重发动作。
        $claim = [IO.File]::Open($bindingPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $binding = [ordered]@{ task_name = 'Xen-' + [guid]::NewGuid().ToString('N');
                user = $user; timeout_seconds = [int]$Request.timeout_seconds;
                created_utc = [datetime]::UtcNow.ToString('o') }
            $bytes = [Text.Encoding]::UTF8.GetBytes(($binding | ConvertTo-Json))
            $claim.Write($bytes, 0, $bytes.Length)
        } finally { $claim.Dispose() }
        [IO.File]::WriteAllText((Join-Path $control 'payload.ps1'), [string]$Request.source, [Text.UTF8Encoding]::new($true))
        $runner = @'
$ErrorActionPreference = 'Stop'
$global:ProgressPreference = 'SilentlyContinue'
$global:LASTEXITCODE = 0
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$env:XEN_AUXILIARY_RUN_DIRECTORY = Split-Path -Parent $PSScriptRoot
$env:XEN_AUXILIARY_STOP_FILE = Join-Path $PSScriptRoot 'STOP'
$result = [ordered]@{ status='running'; process_id=$PID; session_id=(Get-Process -Id $PID).SessionId;
    started_utc=[datetime]::UtcNow.ToString('o'); exit_code=$null; payload_exit_code=$null; ended_utc=$null;
    child_launch_attempted=$false; child_process_id=$null; child_started_utc=$null;
    child_exit_confirmed=$false; supervision_ended_utc=$null }
$resultPath = Join-Path $PSScriptRoot 'result.json'
function Save-Result {
    $temporary = $resultPath + '.partial'
    [IO.File]::WriteAllText($temporary, ($result | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $resultPath -Force
}
function Request-Cancellation([string]$Reason) {
    [IO.File]::WriteAllText($env:XEN_AUXILIARY_STOP_FILE, $Reason)
    $nativePath = Join-Path $env:XEN_AUXILIARY_RUN_DIRECTORY 'task.json'
    if (Test-Path -LiteralPath $nativePath) {
        $native = Get-Content -LiteralPath $nativePath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($native.PSObject.Properties['owner'] -and $native.owner -ceq 'XEN_AUTO_STOP_COUNTERPULSE') {
            $output = Join-Path $env:XEN_AUXILIARY_RUN_DIRECTORY 'result'
            $null = [IO.Directory]::CreateDirectory($output)
            [IO.File]::WriteAllText((Join-Path $output 'STOP'), $Reason)
        }
    }
}
$child = $null
$childExited = $false
$cleanupWaitAttempted = $false
$timedOut = $false
Save-Result
try {
    if (Test-Path -LiteralPath $env:XEN_AUXILIARY_STOP_FILE) { throw '启动前已取消。' }
    $payload = (Join-Path $PSScriptRoot 'payload.ps1').Replace("'", "''")
    $command = '$ErrorActionPreference=''Stop''; $global:ProgressPreference=''SilentlyContinue''; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $global:LASTEXITCODE=0; try { & ''' + $payload + '''; $ok=$?; if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}; if(-not $ok){exit 1} } catch { [Console]::Error.WriteLine($_.Exception.GetType().FullName); exit 1 }'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    # 在启动前记录不确定窗口；进程句柄或启动时间查询失败也不能冒充未启动。
    $result.child_launch_attempted = $true
    Save-Result
    $child = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot 'output.log') -RedirectStandardError (Join-Path $PSScriptRoot 'error.log')
    $result.child_process_id = $child.Id
    Save-Result
    $result.child_started_utc = $child.StartTime.ToUniversalTime().ToString('o')
    $null = $child.Handle
    Save-Result
    $binding = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'task.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($child.WaitForExit(100)) { $childExited = $true; break }
        if ($clock.Elapsed.TotalSeconds -ge $binding.timeout_seconds) {
            $timedOut = $true
            Request-Cancellation 'TIMEOUT_CANCEL_REQUESTED'
        }
        if ($timedOut -or (Test-Path -LiteralPath $env:XEN_AUXILIARY_STOP_FILE)) {
            $result.status = if ($timedOut) { 'timeout_cleanup_pending' } else { 'stop_cleanup_pending' }
            Save-Result
            $cleanupWaitAttempted = $true
            $childExited = $child.WaitForExit(5000)
            break
        }
    }
    if ($childExited) {
        $result.payload_exit_code = $child.ExitCode
        $result.exit_code = if ($timedOut) { 124 } else { $child.ExitCode }
        $result.status = if ($timedOut) { 'timed_out' } elseif ($result.exit_code -ne 0) { 'failed' }
            elseif (Test-Path -LiteralPath $env:XEN_AUXILIARY_STOP_FILE) { 'stopped' } else { 'completed' }
    } else {
        $result.exit_code = if ($timedOut) { 124 } else { 1 }
        $result.status = if ($timedOut) { 'timeout_cleanup_unconfirmed' } else { 'stop_cleanup_unconfirmed' }
    }
} catch {
    # 不把任意异常对象写入报告，避免下游错误意外带出配置秘密。
    $result.exit_code = if ($timedOut) { 124 } else { 1 }
    $result.error_type = $_.Exception.GetType().FullName
    $result.error_line = $_.InvocationInfo.ScriptLineNumber
    if ($result.child_launch_attempted -and -not $childExited) {
        $result.status = 'supervision_failed_cleanup_pending'
        try { Save-Result } catch { }
        try { Request-Cancellation 'SUPERVISION_FAILED_CANCEL_REQUESTED' }
        catch { $result.cancel_error_type = $_.Exception.GetType().FullName }
        if ($child -and -not $cleanupWaitAttempted) {
            $cleanupWaitAttempted = $true
            try { $childExited = $child.WaitForExit(5000) }
            catch { $result.cleanup_error_type = $_.Exception.GetType().FullName }
        }
    }
    $result.status = if (-not $result.child_launch_attempted -or $childExited) { 'failed' }
        else { 'supervision_failed_cleanup_unconfirmed' }
} finally {
    # Dispose 只释放句柄；退出确认与物理输入释放是两层证据。
    if ($child) { try { $child.Dispose() } catch { $result.dispose_error_type = $_.Exception.GetType().FullName } }
    $result.child_exit_confirmed = $childExited
    if (-not $result.child_launch_attempted -or $childExited) { $result.ended_utc = [datetime]::UtcNow.ToString('o') }
    $result.supervision_ended_utc = [datetime]::UtcNow.ToString('o')
    Save-Result
}
exit $result.exit_code
'@
        $runnerPath = Join-Path $control 'run.ps1'
        [IO.File]::WriteAllText($runnerPath, $runner, [Text.UTF8Encoding]::new($true))
        $engine = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $action = New-ScheduledTaskAction -Execute $engine -Argument (
            '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $runnerPath + '"') -WorkingDirectory $run
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([timespan]::FromSeconds($binding.timeout_seconds + 15)) `
            -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        $null = Register-ScheduledTask -TaskName $binding.task_name -Action $action -Principal $principal -Settings $settings
        Start-ScheduledTask -TaskName $binding.task_name
    }
    if (-not (Test-Path -LiteralPath $bindingPath -PathType Leaf)) { throw '找不到本 Run 的远程任务。' }
    $binding = Get-Content -LiteralPath $bindingPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($binding.task_name -notmatch '^Xen-[0-9a-f]{32}$') { throw '远程任务绑定无效。' }
    $task = Get-ScheduledTask -TaskName $binding.task_name -ErrorAction SilentlyContinue
    $expectedArguments = '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + (Join-Path $control 'run.ps1') + '"'
    $expectedEngine = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if ($task -and (@($task.Actions).Count -ne 1 -or $task.Actions.Arguments -cne $expectedArguments -or
        $task.Actions.Execute -ine $expectedEngine)) {
        throw '计划任务已不属于该 Run，未修改任务。'
    }
    if ($mode -eq 'Stop') {
        [IO.File]::WriteAllText($stopPath, 'STOP_REQUESTED', [Text.UTF8Encoding]::new($false))
        # 复用反向轻点原生取消入口；不强杀进程，不向其他设备实例补发输入。
        $nativeTask = Join-Path $run 'task.json'
        if (Test-Path -LiteralPath $nativeTask -PathType Leaf) {
            $native = Get-Content -LiteralPath $nativeTask -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($native.PSObject.Properties['owner'] -and $native.owner -ceq 'XEN_AUTO_STOP_COUNTERPULSE') {
                $null = [IO.Directory]::CreateDirectory((Join-Path $run 'result'))
                [IO.File]::WriteAllText((Join-Path $run 'result\STOP'), 'REMOTE_CANCEL_REQUESTED')
            }
        }
    }
    $result = if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        Get-Content -LiteralPath $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else { $null }
    $state = if ($task) { [string]$task.State } else { 'Unregistered' }
    $schedulerResult = if ($task) { (Get-ScheduledTaskInfo -TaskName $binding.task_name).LastTaskResult } else { $null }
    $completionConfirmed = $result -and $result.ended_utc -and $result.PSObject.Properties['child_launch_attempted'] -and
        (-not $result.child_launch_attempted -or ($result.PSObject.Properties['child_exit_confirmed'] -and $result.child_exit_confirmed))
    $status = if ($completionConfirmed) { [string]$result.status }
        elseif ($result -and $result.status -like '*cleanup_*') { [string]$result.status }
        elseif ($state -eq 'Running') { 'running' }
        elseif ($result) { 'interrupted_cleanup_unconfirmed' }
        else { 'not_started_or_failed' }
    # 返回不等于设备验收；输入释放须以原生工具报告为准。
    [ordered]@{ run_directory=$run; task_name=$binding.task_name; user=$binding.user;
        status=$status; scheduler_state=$state; scheduler_result=$schedulerResult;
        stop_requested=(Test-Path -LiteralPath $stopPath); result=$result } | ConvertTo-Json -Depth 6
    if ($mode -eq 'Recover') {
        foreach ($name in @('output.log', 'error.log')) {
            $log = Join-Path $control $name
            if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Encoding UTF8 }
        }
        if ($task -and $state -notin @('Running', 'Queued') -and $completionConfirmed) {
            Unregister-ScheduledTask -TaskName $binding.task_name -Confirm:$false
        }
    }
}

if ($SshHost -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw '请使用 SSH 配置中的主机别名。' }
$source = ''
if ($Mode -in @('Execute', 'Start')) {
    if (-not $ScriptPath) { throw 'Execute/Start 需要 ScriptPath。' }
    $source = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $ScriptPath).Path)
} elseif ($ScriptPath) { throw 'Status/Stop/Recover 不接受新的脚本，沿用原 Run。' }
if ($Mode -ne 'Execute') {
    if (-not $RunDirectory) { throw '交互任务需要 RunDirectory。' }
    $request = @{ mode=$Mode; run_directory=$RunDirectory; source=$source;
        interactive_user=$InteractiveUser; timeout_seconds=$TimeoutSeconds } | ConvertTo-Json -Compress
    $requestEncoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($request))
    $source = 'function Invoke-XenAuxiliaryTask {' + ${function:Invoke-XenAuxiliaryTask}.ToString() + "`n}`n" +
        '$request = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String(''' + $requestEncoded + ''')) | ConvertFrom-Json' +
        "`nInvoke-XenAuxiliaryTask `$request"
}
# 只编码脚本以避免跨 shell 引号损坏；编码不是加密，不得在脚本中放凭据。
$guard = if ($RequireAdministrator -or $Mode -eq 'Start') {
    '$p=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()); if(!$p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw "远端会话不是管理员"}'
} else { '' }
$remote = @'
$ErrorActionPreference='Stop'
$global:ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$global:LASTEXITCODE=0
try {
'@ + "`n" + $guard + "`n" + @'
    # Windows 旧命令按系统 ANSI 输出；先正确解码，再统一用 UTF-8 回传。
    [Console]::OutputEncoding=[Text.Encoding]::Default
    $OutputEncoding=[Console]::OutputEncoding
'@ + "`n" + '$result = & {' + "`n" + $source + "`n} *>&1`n" + @'
    [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
    $OutputEncoding=[Console]::OutputEncoding
    $result | Write-Output
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} catch {
    [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
'@
# 正文经 stdin 传输，避免 Windows 默认 shell 的命令长度限制；不输出编码正文。
$bootstrap = '$global:ProgressPreference=''SilentlyContinue''; $source=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([Console]::In.ReadToEnd().Trim())); & ([scriptblock]::Create($source))'
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remote)) |
    & ssh.exe -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 `
        $SshHost powershell.exe -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $encoded
if ($LASTEXITCODE -ne 0) { throw "辅机命令失败，退出码 $LASTEXITCODE" }
