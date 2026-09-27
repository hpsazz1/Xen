[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$ConfigPath,
    [Parameter(Mandatory = $true)][ValidateSet('HUD','H40')][string]$Strategy
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$path = (Resolve-Path -LiteralPath $ConfigPath).ProviderPath
Assert-XenNoReparsePathChain $path '策略配置' -RequireExistingLeaf
if ((Split-Path -Leaf $path) -ine 'config.ini') { throw '必须明确指定config.ini。' }
if (@(Get-Process -Name Xen,XenLauncher -ErrorAction SilentlyContinue).Count) {
    throw '请先退出Xen及Launcher；不会强制结束进程。'
}
$original = [IO.File]::ReadAllBytes($path)
if ($original.Length -gt 1048576) { throw '配置大小异常。' }
$encoding = [Text.UTF8Encoding]::new($false, $true)
$text = $encoding.GetString($original)
$sections = [regex]::Matches($text, '(?im)^[\uFEFF\s]*\[auto_stop\][^\S\r\n]*(?:\r?\n|$)')
if ($sections.Count -ne 1) { throw '要求唯一的[auto_stop]配置节。' }
$start = $sections[0].Index + $sections[0].Length
$tail = $text.Substring($start)
$next = [regex]::Match($tail, '(?m)^\s*\[')
$length = if ($next.Success) { $next.Index } else { $tail.Length }
$body = $tail.Substring(0, $length)
$keys = [regex]::Matches($body, '(?im)^[^\S\r\n]*experimental_hud_model[^\S\r\n]*=[^\r\n]*')
if ($keys.Count -gt 1) { throw '策略键重复，拒绝迁移。' }
$value = if ($Strategy -eq 'HUD') { 'true' } else { 'false' }
$line = 'experimental_hud_model = ' + $value
if ($keys.Count) {
    if ($keys[0].Value -notmatch '=\s*(true|false)\s*$') { throw '原策略值无效，拒绝覆盖。' }
    if ($Matches[1] -ieq $value) { [pscustomobject]@{ Changed=$false; Strategy=$Strategy }; return }
    $body = $body.Remove($keys[0].Index, $keys[0].Length).Insert($keys[0].Index, $line)
} else {
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $body = $line + $newline + $body
    if ($start -gt 0 -and $text[$start - 1] -ne "`n") { $body = $newline + $body }
}
$updated = $text.Substring(0, $start) + $body + $tail.Substring($length)
if (-not $PSCmdlet.ShouldProcess($path, "切换急停策略为$Strategy，仅修改策略键并备份")) { return }
$backup = $path + '.strategy-backup-' + [guid]::NewGuid().ToString('N')
$temporary = $path + '.strategy-tmp-' + [guid]::NewGuid().ToString('N')
try {
    [IO.File]::WriteAllBytes($temporary, $encoding.GetBytes($updated))
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToBase64String($original)) {
        throw '配置已被其他进程修改，未替换。'
    }
    [IO.File]::Replace($temporary, $path, $backup)
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
[pscustomobject]@{ Changed=$true; Strategy=$Strategy; Backup=$backup }
