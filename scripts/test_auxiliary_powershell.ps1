param([string]$ScriptPath = (Join-Path $PSScriptRoot 'invoke_auxiliary_powershell.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-XenAuxiliaryTask'
}, $false)
. ([scriptblock]::Create($definition.Extent.Text))
function Assert-True($Value, [string]$Message) { if (-not $Value) { throw $Message } }
$script:registered = $null; $script:starts = 0; $script:removals = 0
function Get-CimInstance { param($ClassName) [pscustomobject]@{ UserName='TEST\DesktopUser' } }
function New-ScheduledTaskAction { param($Execute, $Argument, $WorkingDirectory) [pscustomobject]@{ Execute=$Execute; Arguments=$Argument; WorkingDirectory=$WorkingDirectory } }
function New-ScheduledTaskPrincipal { param($UserId, $LogonType, $RunLevel) [pscustomobject]@{ UserId=$UserId; LogonType=$LogonType; RunLevel=$RunLevel } }
function New-ScheduledTaskSettingsSet { param($ExecutionTimeLimit, $MultipleInstances, [switch]$AllowStartIfOnBatteries, [switch]$DontStopIfGoingOnBatteries)
    [pscustomobject]@{ ExecutionTimeLimit=$ExecutionTimeLimit; MultipleInstances=$MultipleInstances }
}
function Register-ScheduledTask { param($TaskName, $Action, $Principal, $Settings)
    Assert-True ($Principal.LogonType -eq 'Interactive' -and $Principal.RunLevel -eq 'Limited') '应使用已有桌面身份且不提权'
    Assert-True ($Settings.ExecutionTimeLimit.TotalSeconds -eq 25 -and $Settings.MultipleInstances -eq 'IgnoreNew') '应使用固定有界任务与清理宽限且不并发重复执行'
    $script:registered = [pscustomobject]@{ TaskName=$TaskName; Actions=$Action; State='Ready' }
}
function Start-ScheduledTask { param($TaskName) $script:starts++; $script:registered.State='Running' }
function Get-ScheduledTask { param($TaskName, $ErrorAction) $script:registered }
function Get-ScheduledTaskInfo { param($TaskName) [pscustomobject]@{ LastTaskResult=0 } }
function Unregister-ScheduledTask { param($TaskName, $Confirm) $script:removals++; $script:registered=$null }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../temp/XEN-REFINE-REMOTE-001'))
$run = Join-Path $root ('contract-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($run)
try {
    $request = [pscustomobject]@{ mode='Start'; run_directory=$run; interactive_user=''; timeout_seconds=10;
        source='[Console]::WriteLine("无设备回传"); [IO.File]::WriteAllText((Join-Path $env:XEN_AUXILIARY_RUN_DIRECTORY "once.txt"), "once")' }
    $first = Invoke-XenAuxiliaryTask $request | ConvertFrom-Json
    Assert-True ($first.status -eq 'running' -and $script:starts -eq 1) '初次启动应返回已登记任务'
    $again = Invoke-XenAuxiliaryTask $request | ConvertFrom-Json
    Assert-True ($again.task_name -eq $first.task_name -and $script:starts -eq 1) '响应丢失后再次 Start 不得重复执行'
    $runner = Join-Path $run '.auxiliary/run.ps1'
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner
    Assert-True ($LASTEXITCODE -eq 0) '无设备脚本应成功'
    $script:registered.State='Ready'
    $request.mode='Status'
    $status = Invoke-XenAuxiliaryTask $request | ConvertFrom-Json
    Assert-True ($status.status -eq 'completed' -and $status.result.exit_code -eq 0) '状态应返回实际结束码'
    $request.mode='Stop'
    $stopped = Invoke-XenAuxiliaryTask $request | ConvertFrom-Json
    Assert-True ($stopped.stop_requested -and (Test-Path -LiteralPath (Join-Path $run '.auxiliary/STOP'))) 'Stop 只写本 Run 停止请求'
    $request.mode='Recover'
    $recovered = Invoke-XenAuxiliaryTask $request
    Assert-True (($recovered -join "`n") -match '无设备回传' -and $script:removals -eq 1) '回收应返回日志并删除已结束计划任务'
    $request.mode='Start'
    $null = Invoke-XenAuxiliaryTask $request
    Assert-True ($script:starts -eq 1) '已回收 Run 仍不得重发动作'
    # 外层PowerShell不能在payload抛错后拿默认LASTEXITCODE=0冒充成功。
    Remove-Item -LiteralPath (Join-Path $run '.auxiliary/STOP')
    [IO.File]::WriteAllText((Join-Path $run '.auxiliary/payload.ps1'), 'throw "fixture failure"')
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner
    Assert-True ($LASTEXITCODE -ne 0) '子脚本抛错必须回传失败'
    [IO.File]::WriteAllText((Join-Path $run '.auxiliary/payload.ps1'), 'exit 7')
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner
    Assert-True ($LASTEXITCODE -eq 7) '子脚本退出码不得被默认成功覆盖'
    # 在启动前收到停止请求时，生成的生产 runner 不能执行 payload。
    [IO.File]::WriteAllText((Join-Path $run '.auxiliary/STOP'), 'STOP_REQUESTED')
    Remove-Item -LiteralPath (Join-Path $run 'once.txt')
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner
    Assert-True ($LASTEXITCODE -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $run 'once.txt'))) '启动前取消必须阻止 payload'
    $failed = Get-Content -LiteralPath (Join-Path $run '.auxiliary/result.json') -Raw | ConvertFrom-Json
    Assert-True ($failed.status -eq 'failed' -and $failed.ended_utc) '异常也必须写终态'
    Write-Output 'PASS: 一次性交互调度、固定超时、重连不重发、子脚本回传/抛错/退出码、停止、回收和启动前取消。'
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '临时目录越界。' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
