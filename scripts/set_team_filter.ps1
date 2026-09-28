[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$ConfigPath,
    [Parameter(Mandatory = $true)][ValidateSet('true','false')][string]$Enabled,
    [Parameter(Mandatory = $true)][ValidateCount(1,1024)][int[]]$CtClassIds,
    [Parameter(Mandatory = $true)][ValidateCount(1,1024)][int[]]$TClassIds
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
foreach ($ids in @($CtClassIds, $TClassIds)) {
    if (@($ids | Where-Object { $_ -lt 0 }).Count -or @($ids | Select-Object -Unique).Count -ne $ids.Count) {
        throw '阵营类别必须非负且无重复。'
    }
}
if (@($CtClassIds | Where-Object { $TClassIds -contains $_ }).Count) { throw 'CT 与 T 类别不能重叠。' }
$path = (Resolve-Path -LiteralPath $ConfigPath).ProviderPath
Assert-XenNoReparsePathChain $path '阵营配置' -RequireExistingLeaf
if ((Split-Path -Leaf $path) -ine 'config.ini' -or (Get-Item -LiteralPath $path).PSIsContainer) { throw '必须明确指定 config.ini 普通文件。' }
function Assert-Stopped {
    if (@(Get-Process -Name Xen,XenLauncher -ErrorAction SilentlyContinue).Count) {
        throw '请先退出 Xen 及 Launcher；不会强制结束进程。'
    }
}
Assert-Stopped
$original = [IO.File]::ReadAllBytes($path)
if ($original.Length -gt 1048576) { throw '配置大小异常。' }
$encoding = [Text.UTF8Encoding]::new($false, $true)
$text = $encoding.GetString($original)
$sections = [regex]::Matches($text, '(?m)^[\uFEFF\t ]*\[(?<name>[^\]\r\n]+)\][^\r\n]*(?:\r?\n|$)')
function Get-Section([string]$Name) {
    $matches = @($sections | Where-Object { $_.Groups['name'].Value.Trim() -ieq $Name })
    if ($matches.Count -gt 1) { throw "配置节 [$Name] 重复，拒绝迁移。" }
    if (!$matches.Count) { return $null }
    $match = $matches[0]
    $start = $match.Index + $match.Length
    $next = @($sections | Where-Object { $_.Index -ge $start } | Select-Object -First 1)
    $end = if ($next.Count) { $next[0].Index } else { $text.Length }
    [pscustomobject]@{ Start = $start; Body = $text.Substring($start, $end - $start) }
}
$gsi = Get-Section 'gsi'
if (!$gsi) { throw '需要现有唯一 [gsi] 节且 enabled=true；不会修改 GSI。' }
$gsiKeys = [regex]::Matches($gsi.Body, '(?im)^[\t ]*enabled[\t ]*=[^\r\n]*')
if ($gsiKeys.Count -ne 1 -or $gsiKeys[0].Value -notmatch '(?i)=[\t ]*true[\t ]*$') {
    throw '需要现有唯一 GSI enabled=true；不会修改 GSI。'
}
$team = Get-Section 'team_filter'
$newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$values = [ordered]@{ enabled = $Enabled.ToLowerInvariant(); ct_class_ids = ($CtClassIds -join ','); t_class_ids = ($TClassIds -join ',') }
$edits = [Collections.Generic.List[object]]::new()
$missing = ''
foreach ($name in $values.Keys) {
    $keys = @(if ($team) { [regex]::Matches($team.Body, '(?im)^[\t ]*' + $name + '[\t ]*=[^\r\n]*') })
    if ($keys.Count -gt 1) { throw "阵营键 $name 重复，拒绝迁移。" }
    if ($keys.Count) {
        $valueMatch = [regex]::Match($keys[0].Value, '=(?<leading>[\t ]*)(?<value>.*?)(?<trailing>[\t ]*)$')
        $value = $valueMatch.Groups['value']
        if ($value.Value -cne $values[$name]) {
            $edits.Add([pscustomobject]@{ Index = $team.Start + $keys[0].Index + $value.Index; Length = $value.Length; Text = $values[$name] })
        }
    } else { $missing += $name + ' = ' + $values[$name] + $newline }
}
if ($missing.Length) {
    $insertAt = if ($team) { $team.Start } else { $text.Length }
    $prefix = if ($insertAt -gt 0 -and $text[$insertAt - 1] -ne "`n") { $newline } else { '' }
    if (!$team) { $prefix += '[team_filter]' + $newline }
    $edits.Add([pscustomobject]@{ Index = $insertAt; Length = 0; Text = $prefix + $missing })
}
$updated = $text
foreach ($edit in @($edits | Sort-Object Index -Descending)) {
    $updated = $updated.Remove($edit.Index, $edit.Length).Insert($edit.Index, $edit.Text)
}
if ($updated -ceq $text) { [pscustomobject]@{ Changed = $false; Enabled = $values.enabled }; return }
if (!$PSCmdlet.ShouldProcess($path, '仅更新 team_filter 三个键并备份原配置')) { return }
$backup = $path + '.team-filter-backup-' + [guid]::NewGuid().ToString('N')
$temporary = $path + '.team-filter-tmp-' + [guid]::NewGuid().ToString('N')
try {
    [IO.File]::WriteAllBytes($temporary, $encoding.GetBytes($updated))
    Assert-Stopped
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToBase64String($original)) {
        throw '配置已被其他进程修改，未替换。'
    }
    [IO.File]::Replace($temporary, $path, $backup)
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
[pscustomobject]@{ Changed = $true; Enabled = $values.enabled; Backup = $backup }
