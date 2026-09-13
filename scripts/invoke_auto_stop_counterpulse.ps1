[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare', 'Launch')][string]$Mode,
    [Parameter(Mandatory)][string]$RunDirectory,
    [string]$Executable,
    [string]$ConfigPath,
    [ValidateSet('stationary', 'no_counter', 'counter')][string]$Baseline = 'counter',
    [ValidateSet(7, 8)][int]$Shots = 8,
    [ValidateSet('A', 'D')][string]$Direction = 'A',
    [ValidateRange(1, 250)][int]$MoveMs = 120,
    [ValidateRange(1, 200)][int]$CounterHoldMs = 30,
    [ValidateRange(1, 20)][int]$ShotHoldMs = 5,
    [ValidateRange(0, 10)][int]$LateToleranceMs = 5,
    [switch]$AllowPhysicalOutput,
    [string]$Confirm,
    [string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Get-Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Quote-PS([string]$Value) { "'" + $Value.Replace("'", "''") + "'" }
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
}
function Invoke-Probe([string]$Binary, [string[]]$Arguments, [bool]$Physical) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Binary
    $info.WorkingDirectory = Split-Path -Parent $Binary
    $info.UseShellExecute = $false
    # 参数均为本地路径或固定参数；拒绝引号，避免命令行歧义。
    $quoted = foreach ($argument in $Arguments) {
        if ($argument.Contains('"')) { throw '参数包含不支持的引号。' }
        '"' + $argument.TrimEnd('\') + '"'
    }
    $info.Arguments = $quoted -join ' '
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $plain = $null
    try {
        if ($Physical -and $CredentialDirectory) {
            Add-Type -AssemblyName System.Security
            $credentialFile = Join-Path ([IO.Path]::GetFullPath($CredentialDirectory)) 'token.dpapi'
            for ($ancestor = $credentialFile; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
                if ((Test-Path -LiteralPath $ancestor) -and
                    (([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                    throw '凭据路径不能包含重解析点。'
                }
            }
            $plain = [Security.Cryptography.ProtectedData]::Unprotect(
                [IO.File]::ReadAllBytes($credentialFile), $null, [Security.Cryptography.DataProtectionScope]::$Scope)
            $info.EnvironmentVariables['XEN_SOURCE_CONTEXT_TOKEN'] = [Text.Encoding]::UTF8.GetString($plain)
        }
        $child = [Diagnostics.Process]::Start($info)
        try {
            # 不转发子进程原始输出，避免配置或异常携带凭据。
            $stdout = $child.StandardOutput.ReadToEndAsync()
            $stderr = $child.StandardError.ReadToEndAsync()
            $timeoutMs = if ($Physical) { 60000 } else { 30000 }
            if (-not $child.WaitForExit($timeoutMs)) {
                if ($Physical) {
                    # 只向本次Run写取消请求，不另开设备发送补偿命令。
                    try {
                        $outputIndex = [Array]::IndexOf($Arguments, '--output')
                        if ($outputIndex -ge 0 -and $outputIndex + 1 -lt $Arguments.Length) {
                            $stopDirectory = $Arguments[$outputIndex + 1]
                            $null = [IO.Directory]::CreateDirectory($stopDirectory)
                            [IO.File]::WriteAllText((Join-Path $stopDirectory 'STOP'), 'TIMEOUT_CANCEL_REQUESTED')
                        }
                    } catch { }
                    [Console]::Error.WriteLine('反向轻点进程超时，已尝试请求取消；清理状态未知，请人工确认设备。')
                }
                if (-not $child.WaitForExit(5000)) {
                    $child.Kill()
                    $null = $child.WaitForExit(5000)
                }
                throw '反向轻点进程超时，不能认定完成。'
            }
            $null = $stdout.GetAwaiter().GetResult()
            $null = $stderr.GetAwaiter().GetResult()
            if ($child.ExitCode -ne 0) { throw '探针未通过；请检查计划或本次结果。' }
        } finally { $child.Dispose() }
    } finally {
        $info.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
        if ($null -ne $plain) { [Array]::Clear($plain, 0, $plain.Length) }
    }
}

try {
    $runPath = [IO.Path]::GetFullPath($RunDirectory)
    $scriptPath = [IO.Path]::GetFullPath($PSCommandPath)
    $planPath = Join-Path $runPath 'plan.json'
    $taskPath = Join-Path $runPath 'task.json'
    if ($Mode -eq 'Prepare') {
        if ($PSBoundParameters.ContainsKey('AllowPhysicalOutput') -or $PSBoundParameters.ContainsKey('Confirm')) { throw 'Prepare禁止混入物理授权参数。' }
        if (-not $Executable -or -not $ConfigPath) { throw 'Prepare需要Executable和ConfigPath。' }
        if (Test-Path -LiteralPath $runPath) { throw 'Run目录已存在，拒绝覆盖。' }
        $binary = (Resolve-Path -LiteralPath $Executable).Path
        $config = (Resolve-Path -LiteralPath $ConfigPath).Path
        if (-not [IO.File]::Exists($binary) -or -not [IO.File]::Exists($config)) { throw '需要有效文件。' }
        $plan = [ordered]@{ baseline = $Baseline; shots = $Shots; shot_interval_ms = 280;
            move_ms = $MoveMs; counter_hold_ms = $CounterHoldMs; shot_hold_ms = $ShotHoldMs;
            late_tolerance_ms = $LateToleranceMs; direction = $(if ($Direction -eq 'A') { 2 } else { 8 }) }
        $null = New-Item -ItemType Directory -Path $runPath
        Write-Json $planPath $plan
        Invoke-Probe $binary @('--plan', $planPath, '--dry-run') $false
        $task = [ordered]@{ schema_version = 1; status = 'PREPARED_NOT_LAUNCHED'; run_id = [IO.Path]::GetFileName($runPath);
            executable = $binary; config = $config; plan = $planPath; script = $scriptPath;
            executable_sha256 = (Get-Digest $binary); config_sha256 = (Get-Digest $config);
            plan_sha256 = (Get-Digest $planPath); script_sha256 = (Get-Digest $scriptPath) }
        Write-Json $taskPath $task
        $launch = '& ' + (Quote-PS $scriptPath) + ' -Mode Launch -RunDirectory ' + (Quote-PS $runPath) +
            ' -AllowPhysicalOutput -Confirm AUTO_STOP_COUNTERPULSE'
        if ($CredentialDirectory) {
            $launch += ' -CredentialDirectory ' + (Quote-PS ([IO.Path]::GetFullPath($CredentialDirectory))) + ' -Scope ' + $Scope
        }
        $markdown = "# 反向轻点人工Run`n`n状态：PREPARED_NOT_LAUNCHED。仅用户在当前前台执行一次；会发送真实移动及开火输入。`n`n请先确认测试场景、源焦点、独占设备和紫色弹着点显示；End或人工输入取消。`n`n``````powershell`n$launch`n```````n`n一组$Shots 发，间隔280ms；结果不代表已经停稳。结果目录：result。`n"
        [IO.File]::WriteAllText((Join-Path $runPath 'TASK.md'), $markdown, (New-Object Text.UTF8Encoding($false)))
        Write-Output 'PREPARED_NOT_LAUNCHED；未发送设备输入。'
    } else {
        if (-not $AllowPhysicalOutput -or $Confirm -cne 'AUTO_STOP_COUNTERPULSE') { throw '缺少本轮物理输出授权参数。' }
        if ((Get-Process -Id $PID).SessionId -eq 0 -or $env:SSH_CONNECTION -or $env:SSH_CLIENT) { throw 'Launch仅允许用户本机交互会话。' }
        $task = Get-Content -LiteralPath $taskPath -Raw | ConvertFrom-Json
        if ($task.schema_version -ne 1 -or $task.status -ne 'PREPARED_NOT_LAUNCHED' -or
            $task.plan -cne $planPath -or $task.script -cne $scriptPath) { throw 'Run绑定无效。' }
        foreach ($name in @('executable', 'config', 'plan', 'script')) {
            if ((Get-Digest $task.$name) -cne $task.($name + '_sha256')) { throw 'Run绑定文件已变化，必须新建Run。' }
        }
        $output = Join-Path $runPath 'result'
        if (Test-Path -LiteralPath $output) { throw '结果目录已存在，拒绝重复执行。' }
        # CreateNew原子抢占；失败或取消也不自动重试此Run。
        $marker = [IO.File]::Open((Join-Path $runPath 'CONSUMED'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $marker.Dispose()
        Invoke-Probe $task.executable @('--config', $task.config, '--plan', $planPath, '--output', $output,
            '--allow-physical-output', '--confirm', 'AUTO_STOP_COUNTERPULSE') $true
        Write-Output '本次有界Run结束；请回收result及人工观察，不能自动认定停稳。'
    }
} catch {
    # 不输出异常对象或路径内容，尤其不泄漏解密或配置解析异常。
    [Console]::Error.WriteLine('反向轻点入口拒绝或执行失败；未自动重试。请核对授权、绑定、路径与本次结果。')
    exit 1
}
