param([string]$Script = (Join-Path $PSScriptRoot '../scripts/set_xen_gsi_realtime.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-gsi-config-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$path = Join-Path $root 'gamestate_integration_xen.cfg'
$encoding = [Text.UTF8Encoding]::new($false)
$fixture = @'
"Xen Recoil Integration"
{
    "uri" "http://127.0.0.1:5013/" // 保留注释和地址
    "buffer" "0.1"
    "throttle" "0.1"
    "auth" { "token" "fixture-only-not-a-secret" }
    "data" { "player_weapons" "1" }
}
'@
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
try {
    foreach ($bom in @($false, $true)) {
        $inputText = $(if ($bom) { [string][char]0xFEFF } else { '' }) + $fixture.Replace("`n", "`r`n")
        [IO.File]::WriteAllText($path, $inputText, $encoding)
        $before = [IO.File]::ReadAllBytes($path)
        $backupCount = @(Get-ChildItem -LiteralPath $root -Filter '*.backup-*').Count
        & $Script -CfgPath $path -WhatIf | Out-Null
        Require ([Convert]::ToBase64String($before) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($path))) 'WhatIf 不得写入'
        $result = & $Script -CfgPath $path
        Require $result.Changed '应报告修改'
        Require ([IO.File]::ReadAllText($path, $encoding) -ceq $inputText.Replace('"buffer" "0.1"', '"buffer" "0.0"').Replace('"throttle" "0.1"', '"throttle" "0.0"')) '仅可改变两个字段，须保留 BOM/换行/其他内容'
        $backups = @(Get-ChildItem -LiteralPath $root -Filter '*.backup-*' | Sort-Object LastWriteTimeUtc -Descending)
        Require ($backups.Count -eq $backupCount + 1) '首次修改须保留备份'
        Require ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backups[0].FullName)) -ceq [Convert]::ToBase64String($before)) '备份须保持原始字节'
        [IO.File]::SetLastWriteTimeUtc($path, [datetime]'2001-01-01T00:00:00Z')
        $unchangedTime = [IO.File]::GetLastWriteTimeUtc($path)
        $result = & $Script -CfgPath $path
        Require (!$result.Changed -and [IO.File]::GetLastWriteTimeUtc($path) -eq $unchangedTime) '重复运行不得写文件'
        Require (@(Get-ChildItem -LiteralPath $root -Filter '*.backup-*').Count -eq $backups.Count) '重复运行不得另存备份'
    }
    $invalid = @(
        $fixture.Replace('"buffer" "0.1"', '"buffer" "0.1" "buffer" "0.2"'),
        $fixture.Replace('"throttle" "0.1"', '"throttle" "0.1" "THROTTLE" "0.2"'),
        $fixture.Replace('Xen Recoil Integration', 'Another Integration'),
        $fixture.Replace('"throttle" "0.1"', ''),
        $fixture.Replace('"token" "fixture-only-not-a-secret"', '"buffer" "0.1"'),
        ($fixture + ' "extra" "value"')
    )
    $caseIndex = 0
    foreach ($bad in $invalid) {
        [IO.File]::WriteAllText($path, $bad, $encoding)
        $rejected = $false
        try { & $Script -CfgPath $path | Out-Null } catch { $rejected = $true }
        Require $rejected "无效配置必须拒绝：用例 $caseIndex"
        Require ([IO.File]::ReadAllText($path, $encoding) -ceq $bad) '拒绝不得改文件'
        $caseIndex++
    }
    'PASS gsi_realtime_config_tests'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (!$resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'xen-gsi-config-test-*') { throw '测试目录清理范围不正确' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
