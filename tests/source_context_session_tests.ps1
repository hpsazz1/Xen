param([string]$SessionScript = (Join-Path $PSScriptRoot '../scripts/start_source_context_session.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# 仅加载生产决策函数，不执行凭据解密、源服务或设备进程。
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($SessionScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw '源服务脚本语法错误' }
$functions = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($function in $functions) { . ([scriptblock]::Create($function.Extent.Text)) }
if (-not (Get-Command Find-ReusableSource -ErrorAction SilentlyContinue)) { throw '重复启动缺少已运行服务检查' }
$script:endpoints = @()
$script:owner = $null
$script:enumerationFails = $false
function Get-NetUDPEndpoint {
    param($ErrorAction)
    if ($script:enumerationFails) { throw '端点枚举失败' }
    $script:endpoints
}
function Get-CimInstance { param($ClassName, $Filter, $ErrorAction) $script:owner }
function Assert-True($Condition, $Message) { if (-not $Condition) { throw $Message } }
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_.Exception.Message }
    Assert-True ($null -ne $caught -and $caught -match $Pattern) "预期失败未发生：$Pattern；实际：$caught"
}
$arguments = '--enable --host 192.168.3.10 --port 5012 --process cs2.exe --ttl-ms 200 --timeout-ms 3000'
$parameters = @{ Binary='E:\bundle\xen_source_context.exe'; Arguments=$arguments; Session=1; Address='192.168.3.10'; ListenPort=5012 }
Assert-True ($null -eq (Find-ReusableSource @parameters)) '空闲端点应允许启动'
$script:endpoints = @([pscustomobject]@{LocalAddress='192.168.3.10'; LocalPort=5012; OwningProcess=123})
$script:owner = [pscustomobject]@{ExecutablePath=$parameters.Binary; SessionId=1; CommandLine=('"' + $parameters.Binary + '" ' + $arguments)}
Assert-True ((Find-ReusableSource @parameters) -eq 123) '重复同参数启动应复用原 PID'
$script:owner.CommandLine += ' --unknown'
Assert-Throws { Find-ReusableSource @parameters } 'PID=123'
$script:owner.CommandLine = '"' + $parameters.Binary + '" ' + $arguments
$script:owner.SessionId = 2
Assert-Throws { Find-ReusableSource @parameters } '会话或参数不匹配'
$script:owner.SessionId = 1
$script:owner.ExecutablePath = 'E:\other\xen_source_context.exe'
Assert-Throws { Find-ReusableSource @parameters } '已被占用'
$script:endpoints[0].LocalAddress = '0.0.0.0'
Assert-Throws { Find-ReusableSource @parameters } '已被占用'
$script:endpoints[0].LocalAddress = '192.168.3.11'
Assert-True ($null -eq (Find-ReusableSource @parameters)) '另一地址的同端口不应冲突'
$script:enumerationFails = $true
Assert-Throws { Find-ReusableSource @parameters } '端点枚举失败'
Assert-True ((Get-SourceExitDiagnostic 1 '192.168.3.10' 5012) -match '退出码=1.*192.168.3.10:5012.*绑定') '快速退出应有绑定诊断'
Assert-True ((Get-SourceExitDiagnostic 2 '192.168.3.10' 5012) -match '鉴权') '无效鉴权应有单独诊断'
Write-Output 'PASS: 源服务空闲、重复启动、异参/异路径/异会话/通配地址占用、枚举失败、快速退出诊断。'
