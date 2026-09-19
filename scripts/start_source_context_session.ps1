param(
    [Parameter(Mandatory = $true)][ValidateSet('Source', 'Launcher', 'Probe')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [string]$HostAddress,
    [ValidateRange(1, 65535)][int]$Port = 5012,
    [ValidatePattern('^[A-Za-z0-9_.-]+\.exe$')][string]$ProcessName = 'cs2.exe',
    [ValidateRange(20, 2000)][int]$TtlMs = 200,
    [ValidateRange(100, 10000)][int]$TimeoutMs = 3000,
    [ValidatePattern('^udp://[0-9.]+:[0-9]+$')][string]$ClockBindUrl = ''
)

$ErrorActionPreference = 'Stop'

function Get-SourceListenerOwner {
    param([string]$Address, [int]$ListenPort)
    # 枚举失败必须向上传递，不能把无法检查误认为端口空闲。
    $listeners = @(Get-NetUDPEndpoint -ErrorAction Stop | Where-Object {
        $_.LocalPort -eq $ListenPort -and
        ($_.LocalAddress -eq $Address -or $_.LocalAddress -eq '0.0.0.0' -or $Address -eq '0.0.0.0')
    })
    foreach ($listener in $listeners) {
        if (-not $listener.OwningProcess) { throw "源服务端点 ${Address}:$ListenPort 已占用，但无法确认所属 PID。" }
    }
    @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
}

function Find-ReusableSource {
    param([string]$Binary, [string]$Arguments, [int]$Session, [string]$Address, [int]$ListenPort)
    $owners = @(Get-SourceListenerOwner -Address $Address -ListenPort $ListenPort)
    if ($owners.Count -eq 0) { return $null }
    if ($owners.Count -eq 1) {
        $owner = Get-CimInstance Win32_Process -Filter "ProcessId=$($owners[0])" -ErrorAction Stop
        # 只接受本入口生成的完整非秘密参数；未知命令行或其他会话不可复用。
        $expected = '"' + $Binary + '" ' + $Arguments
        if ($owner -and $owner.ExecutablePath -ieq $Binary -and $owner.SessionId -eq $Session -and
            $owner.CommandLine -ieq $expected) { return [int]$owners[0] }
    }
    throw "源服务端点 ${Address}:$ListenPort 已被占用，PID=$($owners -join ',')；现有进程路径、会话或参数不匹配，未停止任何进程。"
}

function Get-SourceExitDiagnostic {
    param([int]$Code, [string]$Address, [int]$ListenPort)
    $reason = switch ($Code) {
        2 { '参数或鉴权环境无效；请检查当前凭据及参数。' }
        1 { '源服务绑定或运行失败；请检查本机地址、UDP 端口占用及系统网络权限。' }
        default { '源程序提前终止；请检查该源程序的运行库和系统事件记录。' }
    }
    "源服务启动后退出，退出码=$Code，端点=${Address}:$ListenPort；$reason"
}

function Start-SourceClock {
    if (-not $ClockBindUrl) { return }
    $clockUri = [Uri]$ClockBindUrl
    $clockAddress = $null
    if (-not [Net.IPAddress]::TryParse($clockUri.Host, [ref]$clockAddress) -or
        $clockAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $clockUri.Port -lt 1 -or $clockUri.Port -gt 65535) { throw '时钟旁路必须使用有效 IPv4 UDP 端点。' }
    $clockBinary = Join-Path (Split-Path -Parent $binaryPath) 'XenClockSource.exe'
    if (-not (Test-Path -LiteralPath $clockBinary -PathType Leaf)) { throw "缺少只读时钟程序：$clockBinary" }
    $clockSession = (Get-Process -Id $PID).SessionId
    $owners = @(Get-SourceListenerOwner -Address $clockUri.Host -ListenPort $clockUri.Port)
    if ($owners.Count -gt 0) {
        if ($owners.Count -eq 1) {
            $owner = Get-CimInstance Win32_Process -Filter "ProcessId=$($owners[0])" -ErrorAction Stop
            $expected = '"' + $clockBinary + '" --bind ' + $ClockBindUrl
            $unquoted = $clockBinary + ' --bind ' + $ClockBindUrl
            if ($owner -and $owner.ExecutablePath -ieq $clockBinary -and $owner.SessionId -eq $clockSession -and
                ($owner.CommandLine -ieq $expected -or $owner.CommandLine -ieq $unquoted)) {
                Write-Output "只读 NDI 时钟已运行，PID=$($owners[0])；复用现有服务。"
                return
            }
        }
        throw "时钟端点 $ClockBindUrl 已占用且进程不匹配；未停止任何进程。"
    }
    $clockChild = Start-Process -FilePath $clockBinary -ArgumentList @('--bind', $ClockBindUrl) -WorkingDirectory (Split-Path -Parent $clockBinary) -WindowStyle Hidden -PassThru
    if ($clockChild.WaitForExit(300)) { throw "只读时钟启动失败，退出码=$($clockChild.ExitCode)。" }
    $startedOwners = @(Get-SourceListenerOwner -Address $clockUri.Host -ListenPort $clockUri.Port)
    if ($clockChild.Id -notin $startedOwners) { throw '时钟进程尚未监听指定端点，请检查启动状态。' }
    Write-Output "只读 NDI 时钟已启动，PID=$($clockChild.Id)，端点=$ClockBindUrl；不发送设备输入。"
}

Add-Type -AssemblyName System.Security
$binaryPath = (Resolve-Path -LiteralPath $Executable).Path
if ($ClockBindUrl -and $Mode -ne 'Source') { throw 'ClockBindUrl 仅用于 Source 模式。' }
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
$sourceArguments = "--enable --host $HostAddress --port $Port --process $ProcessName --ttl-ms $TtlMs --timeout-ms $TimeoutMs"
$reusePid = $null
if ($Mode -eq 'Source') {
    $localAddresses = @([Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
        ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
        ForEach-Object { $_.Address.ToString() })
    if ($HostAddress -ne '0.0.0.0' -and $HostAddress -notin $localAddresses) {
        throw "源服务地址 $HostAddress 不属于本机，请选择本机 IPv4 地址。"
    }
    $reusePid = Find-ReusableSource -Binary $binaryPath -Arguments $sourceArguments -Session $currentSession -Address $HostAddress -ListenPort $Port
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
        $action = if ($Mode -eq 'Source' -and $null -eq $reusePid) { '--enable' } else { '--probe' }
        $childAddress = if ($null -ne $reusePid -and $HostAddress -eq '0.0.0.0') { '127.0.0.1' } else { $HostAddress }
        $childInfo.Arguments = "$action --host $childAddress --port $Port --process $ProcessName --ttl-ms $TtlMs --timeout-ms $TimeoutMs"
        $childInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $childInfo.CreateNoWindow = $true
    }
    if ($Mode -eq 'Probe' -or $null -ne $reusePid) {
        $childInfo.RedirectStandardOutput = $true
        $childInfo.RedirectStandardError = $true
        $childInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
        $childInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    }
    $child = [Diagnostics.Process]::Start($childInfo)
    if ($Mode -eq 'Probe' -or $null -ne $reusePid) {
        $stdoutTask = $child.StandardOutput.ReadToEndAsync()
        $stderrTask = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit($TimeoutMs + 2000)) {
            $child.Kill()
            throw '源状态探测进程超过有界等待期限，已停止。'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($null -ne $reusePid) {
            if ($child.ExitCode -ne 0) {
                throw "源服务已运行，PID=$reusePid，但当前凭据的只读探测失败，退出码=$($child.ExitCode)；请核对凭据或源服务状态，未启动第二实例。"
            }
            Write-Output "只读源桥接已运行并通过当前凭据探测，PID=$reusePid；复用现有服务，不发送设备输入。"
            Start-SourceClock
            return
        }
        if ($stdout) { [Console]::Out.Write($stdout) }
        if ($stderr) { [Console]::Error.Write($stderr) }
        exit $child.ExitCode
    }
    if ($Mode -eq 'Source') {
        if ($child.WaitForExit(300)) { throw (Get-SourceExitDiagnostic -Code $child.ExitCode -Address $HostAddress -ListenPort $Port) }
        Write-Output "只读源桥接已启动，PID=$($child.Id)；不发送设备输入。"
        Start-SourceClock
    } else {
        Write-Output "用户启动的 Launcher 已创建，PID=$($child.Id)。"
    }
} finally {
    $childInfo.EnvironmentVariables.Remove('XEN_SOURCE_CONTEXT_TOKEN')
    if ($null -ne $plainBytes) { [Array]::Clear($plainBytes, 0, $plainBytes.Length) }
}
