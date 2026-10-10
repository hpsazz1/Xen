param(
    [string]$SshHost = 'xen-aux',
    [Parameter(Mandatory = $true)][string]$ScriptPath,
    [switch]$RequireAdministrator
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($SshHost -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw '请使用 SSH 配置中的主机别名。' }
$source = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $ScriptPath).Path)
# 只编码脚本以避免跨 shell 引号损坏；编码不是加密，不得在脚本中放凭据。
$guard = if ($RequireAdministrator) {
    '$p=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()); if(!$p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw "远端会话不是管理员"}'
} else { '' }
$remote = @'
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
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
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remote))
& ssh.exe -o BatchMode=yes -o StrictHostKeyChecking=yes $SshHost powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded
if ($LASTEXITCODE -ne 0) { throw "辅机命令失败，退出码 $LASTEXITCODE" }
