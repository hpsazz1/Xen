param(
    [string]$UserName = 'XenDeploy',
    [Parameter(Mandatory = $true)][string]$PublicKeyPath,
    [Parameter(Mandatory = $true)][string]$ResultPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# 仅供用户在辅机前台完成一次管理员引导；不启动应用或物理输出。
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '请从管理员 PowerShell 或附带的 UAC 入口执行。'
}
$result = [ordered]@{ success = $false; user = $UserName; error = $null }
try {
    $user = Get-LocalUser -Name $UserName
    if (-not $user.Enabled) { throw '目标账户未启用，停止。' }
    $key = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $PublicKeyPath).Path).Trim()
    if ($key -notmatch '^ssh-ed25519 [A-Za-z0-9+/]+={0,2}( [^\r\n]*)?$') {
        throw '仅接受单行 Ed25519 公钥。'
    }
    $sshRoot = Join-Path $env:ProgramData 'ssh'
    $sshd = Join-Path $env:WINDIR 'System32/OpenSSH/sshd.exe'
    & $sshd -t
    if ($LASTEXITCODE -ne 0) { throw '现有 SSH 服务配置验证失败。' }
    $keys = Join-Path $sshRoot 'administrators_authorized_keys'
    $existing = if (Test-Path -LiteralPath $keys) { [IO.File]::ReadAllText($keys) } else { '' }
    if ($key -notin @($existing -split '\r?\n')) {
        [IO.File]::WriteAllText($keys, $existing.TrimEnd() + [Environment]::NewLine + $key + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    $admins = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $acl = [Security.AccessControl.FileSecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner($admins)
    foreach ($sid in @($admins, $system)) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
    }
    Set-Acl -LiteralPath $keys -AclObject $acl
    $members = @(Get-LocalGroupMember -SID $admins)
    if ($user.SID.Value -notin @($members | ForEach-Object { $_.SID.Value })) {
        Add-LocalGroupMember -SID $admins -Member $user
    }
    $result.success = $true
} catch {
    $result.error = $_.Exception.Message
    throw
} finally {
    $result | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding UTF8
}
Write-Host '管理员 SSH 已配置。后续命令和返回内容由主机直接读取。'
