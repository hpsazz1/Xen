param(
    [Parameter(Mandatory = $true)][string]$CredentialDirectory,
    [ValidateSet('CurrentUser', 'LocalMachine')][string]$Scope = 'CurrentUser',
    [string[]]$ReaderSid = @()
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$credentialRoot = [IO.Path]::GetFullPath($CredentialDirectory)
$credentialPath = Join-Path $credentialRoot 'token.dpapi'
for ($ancestor = $credentialRoot; $ancestor; $ancestor = [IO.Path]::GetDirectoryName($ancestor)) {
    if (Test-Path -LiteralPath $ancestor) {
        if (([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '凭据目录及父路径不能包含重解析点。'
        }
    }
}
# 标准输入由调用进程内存传入；命令参数、脚本和输出不得包含鉴权值。
if (-not [Console]::IsInputRedirected) { throw '凭据安装只接受重定向标准输入。' }
if (Test-Path -LiteralPath $credentialRoot) { throw '凭据目录已存在；保留原凭据，拒绝覆盖。' }
$credentialText = [Console]::ReadLine()
if ($null -eq $credentialText -or $credentialText -notmatch '^[A-Za-z0-9+/=_-]{32,1024}$') {
    throw '标准输入凭据缺失或格式无效。'
}
$credentialBytes = [Text.Encoding]::UTF8.GetBytes($credentialText)
$credentialText = $null
$verifiedBytes = $null
try {
    $scopeValue = [Security.Cryptography.DataProtectionScope]::$Scope
    $cipherBytes = [Security.Cryptography.ProtectedData]::Protect($credentialBytes, $null, $scopeValue)
    $ownerSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($ownerSid.Value, 'S-1-5-18', 'S-1-5-32-544') | Select-Object -Unique) {
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    foreach ($sid in $ReaderSid) {
        $identity = New-Object Security.Principal.SecurityIdentifier($sid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($identity, 'ReadAndExecute', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    [IO.Directory]::CreateDirectory($credentialRoot) | Out-Null
    # PS7 父进程可能把自己的 PSModulePath 传给 Windows PowerShell；直接用
    # .NET ACL API，避免 PS5.1 误加载不同版本的 Microsoft.PowerShell.Security。
    if ($PSVersionTable.PSEdition -eq 'Desktop') {
        [IO.Directory]::SetAccessControl($credentialRoot, $acl)
    } else {
        [IO.FileSystemAclExtensions]::SetAccessControl((New-Object IO.DirectoryInfo($credentialRoot)), $acl)
    }
    # 先收紧目录 ACL，再写密文；DPAPI LocalMachine 的跨用户边界由此 ACL 提供。
    [IO.File]::WriteAllBytes($credentialPath, $cipherBytes)
    $verifiedBytes = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($credentialPath), $null, $scopeValue)
    if ($verifiedBytes.Length -ne $credentialBytes.Length) { throw '凭据回读验证失败。' }
    for ($index = 0; $index -lt $credentialBytes.Length; ++$index) {
        if ($verifiedBytes[$index] -ne $credentialBytes[$index]) { throw '凭据回读验证失败。' }
    }
    Write-Output "源桥接凭据密文已安装并回读验证；Scope=$Scope。"
} finally {
    [Array]::Clear($credentialBytes, 0, $credentialBytes.Length)
    if ($null -ne $verifiedBytes) { [Array]::Clear($verifiedBytes, 0, $verifiedBytes.Length) }
}
