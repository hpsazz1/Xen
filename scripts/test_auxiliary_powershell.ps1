param(
    [string]$ScriptPath = (Join-Path $PSScriptRoot 'invoke_auxiliary_powershell.ps1'),
    [string]$TemporaryRoot = (Join-Path $PSScriptRoot '../temp/XEN-DEEP-FIX-20261010-001')
)
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
    Assert-True ($Settings.ExecutionTimeLimit.TotalSeconds -eq 25 -and $Settings.MultipleInstances -eq 'IgnoreNew') '应登记配置期限并防止重复实例'
    $script:registered = [pscustomobject]@{ TaskName=$TaskName; Actions=$Action; State='Ready' }
}
function Start-ScheduledTask { param($TaskName) $script:starts++; $script:registered.State='Running' }
function Get-ScheduledTask { param($TaskName, $ErrorAction) $script:registered }
function Get-ScheduledTaskInfo { param($TaskName) [pscustomobject]@{ LastTaskResult=0 } }
function Unregister-ScheduledTask { param($TaskName, $Confirm) $script:removals++; $script:registered=$null }
$root = [IO.Path]::GetFullPath($TemporaryRoot)
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
    Assert-True ($status.result.child_exit_confirmed -and $status.result.child_process_id -gt 0 -and $status.result.child_started_utc) '正常结束也须记录实际子进程身份与退出确认'
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
    Assert-True (-not $failed.child_launch_attempted -and -not $failed.child_exit_confirmed) '启动前取消不得伪造子进程退出'
    # 直接执行生成的生产 runner，只替换进程边界，不复制监督逻辑或启动真实子任务。
    $fixturePath = Join-Path $run '.auxiliary/process-fixture.ps1'
    $fixture = @'
param([string]$Scenario)
$global:fixtureScenario = $Scenario
$global:fixtureWaits = [Collections.Generic.List[int]]::new()
# 必须用真实 .NET getter 抛错；固定值或只在调用方主动 throw 覆盖不到属性读取边界。
Add-Type -TypeDefinition @"
public sealed class XenAuxiliaryProcessFixture {
    public bool FailHandle;
    public int HandleReads;
    public int Id { get { return 24680; } }
    public System.DateTime StartTime { get { return new System.DateTime(2026, 10, 10, 1, 2, 3, System.DateTimeKind.Utc); } }
    public System.IntPtr Handle {
        get {
            ++HandleReads;
            if (FailHandle) { throw new System.InvalidOperationException("模拟句柄查询失败"); }
            return new System.IntPtr(1);
        }
    }
    public int ExitCode { get { return 0; } }
}
"@
function Start-Process {
    param($FilePath, $ArgumentList, $WindowStyle, [switch]$PassThru, $RedirectStandardOutput, $RedirectStandardError)
    $child = [XenAuxiliaryProcessFixture]::new()
    $child.FailHandle = $global:fixtureScenario -like 'handle_*'
    $child | Add-Member ScriptMethod WaitForExit {
        param([int]$Milliseconds)
        $global:fixtureWaits.Add($Milliseconds)
        if ($global:fixtureScenario -eq 'wait_failure') { throw [InvalidOperationException]::new('模拟等待失败') }
        if ($Milliseconds -eq 100 -and $global:fixtureWaits.Count -ge 3) { throw [InvalidOperationException]::new('监督没有进入有界清理等待') }
        return $Milliseconds -gt 100 -and $global:fixtureScenario -in @('binding_exits', 'timeout_exits', 'stop_exits', 'handle_exits')
    }
    $child | Add-Member ScriptMethod Dispose {
        $observed = @{ waits=@($global:fixtureWaits); handle_reads=$this.HandleReads }
        [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'fixture-process.json'), ($observed | ConvertTo-Json -Compress))
    }
    if ($global:fixtureScenario -like 'stop_*') { [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'STOP'), 'STOP_REQUESTED') }
    return $child
}
& (Join-Path $PSScriptRoot 'run.ps1')
exit $LASTEXITCODE
'@
    [IO.File]::WriteAllText($fixturePath, $fixture, [Text.UTF8Encoding]::new($true))
    $bindingPath = Join-Path $run '.auxiliary/task.json'
    $bindingText = [IO.File]::ReadAllText($bindingPath)
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($scenario in @('binding_pending', 'timeout_error', 'binding_exits', 'timeout_exits', 'stop_exits', 'wait_failure', 'timeout_pending', 'handle_pending', 'handle_exits')) {
        try {
            Remove-Item -LiteralPath (Join-Path $run '.auxiliary/STOP') -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath (Join-Path $run '.auxiliary/fixture-process.json') -Force -ErrorAction SilentlyContinue
            if ($scenario -like 'binding_*') { [IO.File]::WriteAllText($bindingPath, '{invalid json') }
            else {
                $binding = $bindingText | ConvertFrom-Json
                $binding.timeout_seconds = if ($scenario -like 'timeout_*') { 0 } else { 10 }
                [IO.File]::WriteAllText($bindingPath, ($binding | ConvertTo-Json))
            }
            [IO.File]::WriteAllText((Join-Path $run 'task.json'), $(if ($scenario -eq 'timeout_error') { '{invalid json' } else { '{}' }))
            & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $fixturePath -Scenario $scenario
            $result = Get-Content -LiteralPath (Join-Path $run '.auxiliary/result.json') -Raw | ConvertFrom-Json
            $expectedExit = $scenario -in @('binding_exits', 'timeout_exits', 'stop_exits', 'handle_exits')
            Assert-True (([bool]$result.ended_utc) -eq $expectedExit) "$scenario 仅确认子进程退出后才写任务结束时间"
            Assert-True ($result.child_launch_attempted -and $result.PSObject.Properties['child_process_id'] -and $result.child_process_id -eq 24680 -and
                ([datetime]$result.child_started_utc).ToUniversalTime() -eq ([datetime]'2026-10-10T01:02:03Z').ToUniversalTime()) "$scenario 必须持久化启动尝试、PID 与准确开始时间"
            $observed = Get-Content -LiteralPath (Join-Path $run '.auxiliary/fixture-process.json') -Raw | ConvertFrom-Json
            $waits = @($observed.waits)
            Assert-True ($waits.Count -le 3 -and @($waits | Where-Object { $_ -gt 100 -and $_ -le 10000 }).Count -eq 1) "$scenario 必须仅作一次有界清理等待"
            Assert-True ($observed.handle_reads -eq 1) "$scenario 必须实际读取 Process 句柄"
            if ($scenario -like 'handle_*') {
                Assert-True ($waits.Count -eq 1 -and $waits[0] -eq 5000) "$scenario getter 异常后必须立即进入有界清理，不能先继续常规监督"
                Assert-True ($result.PSObject.Properties['error_type'] -and $result.error_type) "$scenario 必须保留句柄查询失败证据"
            }
            Assert-True ($result.child_exit_confirmed -eq $expectedExit) "$scenario 不得猜测子进程退出"
            Assert-True ([bool]$result.supervision_ended_utc) "$scenario 必须区分监督器结束与子任务结束"
            Assert-True (Test-Path -LiteralPath (Join-Path $run '.auxiliary/STOP')) "$scenario 必须请求本 Run 取消"
            if (-not $expectedExit) { Assert-True ($result.status -like '*cleanup_unconfirmed') "$scenario 必须保留清理未知状态" }
            elseif ($scenario -eq 'timeout_exits') { Assert-True ($result.status -eq 'timed_out' -and $result.exit_code -eq 124) '超时后正常退出仍报告超时' }
            elseif ($scenario -eq 'stop_exits') { Assert-True ($result.status -eq 'stopped') '取消后正常退出应报告 stopped' }
            else { Assert-True ($result.status -eq 'failed' -and $result.exit_code -ne 0) '监督异常后退出不得变成成功' }
            [IO.File]::WriteAllText($bindingPath, $bindingText)
            $script:registered = [pscustomobject]@{ TaskName=$first.task_name;
                Actions=[pscustomobject]@{ Execute=(Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'); Arguments=('-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $runner + '"') }; State='Ready' }
            $beforeRemovals = $script:removals
            foreach ($schedulerState in @('Running', 'Queued')) {
                $script:registered.State = $schedulerState
                $request.mode = 'Status'
                $pending = Invoke-XenAuxiliaryTask $request | ConvertFrom-Json
                Assert-True ($pending.status -eq $result.status) "$scenario 调度器 $schedulerState 不得覆盖子进程退出证据"
                $request.mode = 'Recover'
                $null = Invoke-XenAuxiliaryTask $request
                Assert-True ($script:removals -eq $beforeRemovals) "$scenario 调度器 $schedulerState 时不得注销任务"
            }
            $script:registered.State = 'Ready'
            $request.mode = 'Recover'
            $recovery = Invoke-XenAuxiliaryTask $request
            Assert-True ($script:removals -eq ($beforeRemovals + [int]$expectedExit)) "$scenario 清理未确认时不得注销计划任务"
            if (-not $expectedExit) { Assert-True (($recovery -join "`n") -match 'cleanup_unconfirmed') "$scenario 回收必须继续呈现清理未知" }
            Write-Output "PASS: $scenario"
        } catch { $failures.Add($_.Exception.Message); Write-Output "FAIL: $scenario $($_.Exception.Message)" }
    }
    if ($failures.Count) { throw ($failures -join "`n") }
    # 旧报告只有 ended_utc，不能作为新监督契约的子进程退出证据。
    [IO.File]::WriteAllText((Join-Path $run '.auxiliary/result.json'), (@{ status='failed'; ended_utc=[datetime]::UtcNow.ToString('o') } | ConvertTo-Json))
    $beforeRemovals = $script:removals
    $request.mode = 'Recover'
    $legacy = Invoke-XenAuxiliaryTask $request
    Assert-True ($script:removals -eq $beforeRemovals -and ($legacy -join "`n") -match 'interrupted_cleanup_unconfirmed') '旧 ended_utc 不足以自动注销任务'
    Write-Output 'PASS: 一次性交互调度、固定超时、重连不重发、子脚本回传/抛错/退出码、停止、回收、启动前取消及监督异常退出确认。'
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '临时目录越界。' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
