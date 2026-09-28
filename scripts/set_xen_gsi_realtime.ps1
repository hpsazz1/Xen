[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$CfgPath,
    [switch]$IncludeTeamContext
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$item = Get-Item -LiteralPath $CfgPath
if ($item.PSIsContainer -or $item.Name -ine 'gamestate_integration_xen.cfg' -or
    ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw '必须明确指定普通文件 gamestate_integration_xen.cfg。'
}
$path = $item.FullName
$original = [IO.File]::ReadAllBytes($path)
if ($original.Length -gt 65536) { throw 'GSI 配置超过允许大小。' }
$encoding = [Text.UTF8Encoding]::new($false, $true)
try { $content = $encoding.GetString($original) } catch { throw 'GSI 配置必须是有效 UTF-8。' }
# 只接受带引号的 KeyValues、花括号和行注释；保留原始字符位置，不重写整个配置格式。
$pattern = '\G(?:[\s\uFEFF]+|//[^\r\n]*|(?<quoted>"(?:\\.|[^"\\])*")|(?<brace>[{}]))'
$tokens = [Collections.Generic.List[object]]::new()
$lexer = [regex]::new($pattern, [Text.RegularExpressions.RegexOptions]::None, [timespan]::FromSeconds(1))
$position = 0
while ($position -lt $content.Length) {
    # \G 需要从当前偏移开始匹配。
    $match = $lexer.Match($content, $position)
    if (!$match.Success -or $match.Index -ne $position) { throw 'GSI 配置语法不受支持；未修改。' }
    if ($match.Groups['quoted'].Success -or $match.Groups['brace'].Success) {
        $tokens.Add([pscustomobject]@{ Text = $match.Value; Index = $match.Index; Length = $match.Length })
    }
    $position += $match.Length
}
if ($tokens.Count -lt 4 -or $tokens[0].Text -cne '"Xen Recoil Integration"' -or $tokens[1].Text -ne '{') {
    throw '不是受支持的 Xen Recoil Integration 配置；未修改。'
}
$depth = 1
$index = 2
$fields = @{}
$teamFields = @{}
$dataCount = 0
$dataDepth = 0
$dataClose = -1
$rootClose = -1
while ($index -lt $tokens.Count) {
    $key = $tokens[$index].Text
    if ($key -eq '}') {
        if ($IncludeTeamContext -and $depth -eq $dataDepth) { $dataClose = $tokens[$index].Index; $dataDepth = 0 }
        if ($depth -eq 1) { $rootClose = $tokens[$index].Index }
        $depth--; $index++
        if ($depth -eq 0) { break }
        continue
    }
    if (!$key.StartsWith('"') -or $index + 1 -ge $tokens.Count) { throw 'GSI 配置键值结构无效；未修改。' }
    $value = $tokens[$index + 1]
    $name = $key.Substring(1, $key.Length - 2)
    if ($IncludeTeamContext -and $depth -eq 1 -and $name -ieq 'data') {
        $dataCount++
        if ($dataCount -ne 1 -or $value.Text -ne '{') { throw 'data 必须是唯一顶层对象；未修改。' }
        $dataDepth = 2
    }
    if ($IncludeTeamContext -and $depth -eq $dataDepth -and ($name -ieq 'map' -or $name -ieq 'round')) {
        if ($teamFields.ContainsKey($name) -or $value.Text -notmatch '^"[01]"$') {
            throw 'data.map/round 必须是唯一的 0 或 1 字段；未修改。'
        }
        $teamFields[$name] = $value
    }
    if ($name -ieq 'buffer' -or $name -ieq 'throttle') {
        if ($depth -ne 1 -or $fields.ContainsKey($name) -or $value.Text -notmatch '^"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?"$') {
            throw 'buffer/throttle 必须各有一个顶层数值字段；未修改。'
        }
        $fields[$name] = $value
    }
    if ($value.Text -eq '{') { $depth++ }
    elseif (!$value.Text.StartsWith('"')) { throw 'GSI 配置值结构无效；未修改。' }
    $index += 2
}
if ($depth -ne 0 -or $index -ne $tokens.Count -or $fields.Count -ne 2) {
    throw 'GSI 配置不完整，或缺少唯一 buffer/throttle；未修改。'
}
$updated = $content
$edits = [Collections.Generic.List[object]]::new()
foreach ($field in $fields.Values) {
    $edits.Add([pscustomobject]@{ Index = $field.Index; Length = $field.Length; Text = '"0.0"' })
}
if ($IncludeTeamContext) {
    $missing = ''
    foreach ($name in @('map', 'round')) {
        if ($teamFields.ContainsKey($name)) {
            $field = $teamFields[$name]
            $edits.Add([pscustomobject]@{ Index = $field.Index; Length = $field.Length; Text = '"1"' })
        } else { $missing += ' "' + $name + '" "1"' }
    }
    if ($missing.Length -gt 0) {
        $insertAt = $dataClose
        $insertText = $missing + ' '
        if ($dataCount -eq 0) { $insertAt = $rootClose; $insertText = ' "data" {' + $missing + ' } ' }
        if ($insertAt -lt 0) { throw 'data 插入位置无效；未修改。' }
        $edits.Add([pscustomobject]@{ Index = $insertAt; Length = 0; Text = $insertText })
    }
}
foreach ($edit in @($edits | Sort-Object Index -Descending)) {
    $updated = $updated.Remove($edit.Index, $edit.Length).Insert($edit.Index, $edit.Text)
}
if ($updated -ceq $content) {
    [pscustomobject]@{ Changed = $false; Buffer = '0.0'; Throttle = '0.0'; BackupCreated = $false }
    return
}
$changeDescription = '仅将 GSI buffer/throttle 改为 0.0，并保留原文件备份'
if ($IncludeTeamContext) { $changeDescription = '将 GSI buffer/throttle 改为 0.0，并启用 data.map/round，保留原文件备份' }
if (!$PSCmdlet.ShouldProcess($path, $changeDescription)) { return }
$backup = $path + '.backup-' + [guid]::NewGuid().ToString('N')
$temporary = $path + '.tmp-' + [guid]::NewGuid().ToString('N')
try {
    [IO.File]::WriteAllBytes($temporary, $encoding.GetBytes($updated))
    # 替换前检查，避免覆盖编辑器或另一实例刚写入的配置。
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToBase64String($original)) {
        throw 'GSI 配置已被其他进程修改；未替换。'
    }
    [IO.File]::Replace($temporary, $path, $backup)
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
[pscustomobject]@{ Changed = $true; Buffer = '0.0'; Throttle = '0.0'; BackupCreated = $true }
