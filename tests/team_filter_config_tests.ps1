param([string]$Script = (Join-Path $PSScriptRoot '../scripts/set_team_filter.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-team-config-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$path = Join-Path $root 'config.ini'
$encoding = [Text.UTF8Encoding]::new($false)
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Write-Fixture([string]$Text) { [IO.File]::WriteAllText($path, $Text, $encoding) }
try {
    $base = "; 保留注释`r`n[gsi]`r`nenabled = true`r`nport = 5013`r`n[aim]`r`nperson_class_ids = 0,2`r`n"
    foreach ($bom in @('', [string][char]0xFEFF)) {
        $inputText = $bom + $base
        Write-Fixture $inputText
        $before = [IO.File]::ReadAllBytes($path)
        & $Script -ConfigPath $path -Enabled true -CtClassIds 0,1 -TClassIds 2,3 -WhatIf | Out-Null
        Require ([IO.File]::ReadAllText($path, $encoding) -ceq $inputText) 'WhatIf 不得修改配置'
        $result = & $Script -ConfigPath $path -Enabled true -CtClassIds 0,1 -TClassIds 2,3
        $expected = $inputText + "[team_filter]`r`nenabled = true`r`nct_class_ids = 0,1`r`nt_class_ids = 2,3`r`n"
        Require ($result.Changed -and [IO.File]::ReadAllText($path, $encoding) -ceq $expected) '只追加阵营节并保留其余字节'
        Require ([Convert]::ToBase64String([IO.File]::ReadAllBytes($result.Backup)) -ceq [Convert]::ToBase64String($before)) '备份保留原始字节'
        $time = [IO.File]::GetLastWriteTimeUtc($path)
        $result = & $Script -ConfigPath $path -Enabled true -CtClassIds 0,1 -TClassIds 2,3
        Require (!$result.Changed -and [IO.File]::GetLastWriteTimeUtc($path) -eq $time) '重复迁移不得重写'
    }
    $existing = $base + "[team_filter]`n  enabled`t= false  `nct_class_ids=4,5`t`nt_class_ids = 6,7`nother=keep`n"
    Write-Fixture $existing
    & $Script -ConfigPath $path -Enabled true -CtClassIds 0,1 -TClassIds 2,3 | Out-Null
    $expected = $existing.Replace('false', 'true').Replace('4,5','0,1').Replace('6,7','2,3')
    Require ([IO.File]::ReadAllText($path, $encoding) -ceq $expected) '已有节仅替换三个值，保留空白及无关键'
    & $Script -ConfigPath $path -Enabled false -CtClassIds 0,1 -TClassIds 2,3 | Out-Null
    Require ([IO.File]::ReadAllText($path, $encoding) -ceq $expected.Replace("= true  ","= false  ")) '支持显式关闭'
    Write-Fixture ($base + '[team_filter]')
    & $Script -ConfigPath $path -Enabled true -CtClassIds 0,1 -TClassIds 2,3 | Out-Null
    Require ([IO.File]::ReadAllText($path, $encoding) -ceq ($base + "[team_filter]`r`nenabled = true`r`nct_class_ids = 0,1`r`nt_class_ids = 2,3`r`n")) '末尾无换行的空节可补齐三键'
    foreach ($case in @(
        @{ Text = $base; Ct = @(-1); T = @(2,3) },
        @{ Text = $base; Ct = @(0,0); T = @(2,3) },
        @{ Text = $base; Ct = @(0,1); T = @(1,3) },
        @{ Text = $base.Replace('true','false'); Ct = @(0,1); T = @(2,3) },
        @{ Text = $base.Replace('[gsi]','[other]'); Ct = @(0,1); T = @(2,3) },
        @{ Text = $base + "[gsi]`nenabled=true"; Ct = @(0,1); T = @(2,3) },
        @{ Text = $base.Replace('port = 5013', 'enabled=true'); Ct = @(0,1); T = @(2,3) },
        @{ Text = $base + "[team_filter]`n[team_filter]"; Ct = @(0,1); T = @(2,3) },
        @{ Text = $base + "[team_filter]`nenabled=false`nENABLED=true"; Ct = @(0,1); T = @(2,3) }
    )) {
        Write-Fixture $case.Text
        $rejected = $false
        try { & $Script -ConfigPath $path -Enabled true -CtClassIds $case.Ct -TClassIds $case.T | Out-Null } catch { $rejected = $true }
        Require $rejected '非法映射或重复配置须拒绝'
        Require ([IO.File]::ReadAllText($path, $encoding) -ceq $case.Text) '拒绝后原配置不变'
    }
    'PASS team_filter_config_tests'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (!$resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'xen-team-config-test-*') { throw '测试清理目录范围不正确' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
