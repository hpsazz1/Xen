param(
    [Parameter(Mandatory = $true)][ValidateSet('Source', 'Launcher', 'Probe')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [string]$HostAddress,
    [ValidateRange(1, 65535)][int]$Port = 5012,
    [ValidatePattern('^[A-Za-z0-9_.-]+\.exe$')][string]$ProcessName = 'cs2.exe',
    [ValidateRange(20, 2000)][int]$TtlMs = 200,
    [ValidateRange(100, 10000)][int]$TimeoutMs = 3000
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$binaryPath = (Resolve-Path -LiteralPath $Executable).Path
$credentialPath = Join-Path ([IO.Path]::GetFullPath($CredentialDirectory)) 'token.dpapi'
for ($ancestor = $credentialPath; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
    if (Test-Path -LiteralPath $ancestor) {
        if (([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '凭据文件及父路径不能包含重解析点。'
        }
    }
}
if ($Mode -ne 'Launcher') {
    if ([IO.Path]::GetFileName($binaryPath) -ine 'xen_source_context.exe') {
        throw 'Source/Probe 只允许 xen_source_context.exe，不能启动 Runtime 或 Launcher。'
    }
    $address = $null
    if (-not [Net.IPAddress]::TryParse($HostAddress, [ref]$address) -or
        $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw '源桥接地址必须是 IPv4。' }
}
if ($Mode -eq 'Launcher') {
    if ([IO.Path]::GetFileName($binaryPath) -ine 'XenLauncher.exe') { throw 'Launcher 模式只允许 XenLauncher.exe。' }
    if ((Get-Process -Id $PID).SessionId -eq 0) { throw 'Launcher 只能由用户在辅机交互会话启动，拒绝 SSH/服务会话。' }
}
if ($Mode -eq 'Source') {
    $currentSession = (Get-Process -Id $PID).SessionId
    $game = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ProcessName)) -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $currentSession })
    if ($currentSession -eq 0 -or $game.Count -eq 0) {
        throw '源服务须从游戏所在主机的同一交互会话启动；拒绝 SSH/服务会话。'
    }
}
$plainBytes = $null
$childInfo = New-Object Diagnostics.ProcessStartInfo
try {
    $scopeValue = [Security.Cryptography.DataProtectionScope]::$Scope
    $plainBytes = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($credentialPath), $null, $scopeValue)
    $childInfo.FileName = $binaryPath
    $childInfo.WorkingDirectory = Split-Path -Parent $binaryPath
    $childInfo.UseShellExecute = $false
    $childInfo.EnvironmentVariables['XEN_SOURCE_CONTEXT_TOKEN'] = [Text.Encoding]::UTF8.GetString($plainBytes)
    if ($Mode -ne 'Launcher') {
        $action = if ($Mode -eq 'Source') { '--enable' } else { '--probe' }
        $childInfo.Arguments = "$action --host $HostAddress --port $Port --process $ProcessName --ttl-ms $TtlMs --timeout-ms $TimeoutMs"
        $childInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $childInfo.CreateNoWindow = $true
    }
    if ($Mode -eq 'Probe') {
        $childInfo.RedirectStandardOutput = $true
        $childInfo.RedirectStandardError = $true
        $childInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
        $childInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    }
    $child = [Diagnostics.Process]::Start($childInfo)
    if ($Mode -eq 'Probe') {
        $stdoutTask = $child.StandardOutput.ReadToEndAsync()
        $stderrTask = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit($TimeoutMs + 2000)) {
            $child.Kill()
            throw '源状态探测进程超过有界等待期限，已停止。'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($stdout) { [Console]::Out.Write($stdout) }
        if ($stderr) { [Console]::Error.Write($stderr) }
        exit $child.ExitCode
    }
    if ($Mode -eq 'Source') {
        if ($child.WaitForExit(300)) { throw '源服务启动后退出；请检查地址、端口及配置。' }
        Write-Output "只读源桥接已启动，PID=$($child.Id)；不发送设备输入。"
    } else {
        Write-Output "用户启动的 Launcher 已创建，PID=$($child.Id)。"
    }
} finally {
    $childInfo.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
    if ($null -ne $plainBytes) { [Array]::Clear($plainBytes, 0, $plainBytes.Length) }
}
