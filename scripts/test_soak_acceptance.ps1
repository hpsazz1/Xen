#requires -Version 5.1
[CmdletBinding()]
param([string]$EvidenceParent = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'soak_acceptance_support.ps1')
if (-not $EvidenceParent) { $EvidenceParent = Join-Path $PSScriptRoot '../docs/evidence/SOAK-RC-001/20260928/entry-fixture' }
$null = New-Item -ItemType Directory -Path $EvidenceParent -Force
$root = Join-Path ([IO.Path]::GetFullPath($EvidenceParent)) ('fixture-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$package = Join-Path $root 'fake package'
$run = Join-Path $root 'run'
$engine = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell/v1.0/powershell.exe'
$entry = Join-Path $PSScriptRoot 'invoke_hud_stop_acceptance.ps1'
$checks = New-Object 'Collections.Generic.List[string]'
$children = New-Object 'Collections.Generic.List[Diagnostics.Process]'
function Assert([bool]$Ok, [string]$Message) {
    if (-not $Ok) { throw $Message }
    $checks.Add($Message)
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $caught = $false
    try { & $Action | Out-Null } catch { $caught = $true }
    Assert $caught $Message
}
function Write-Fixture([string]$Path, [string]$Text) {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}
function Invoke-Entry([string]$Mode, [bool]$Success = $true, [string[]]$Extra = @()) {
    $log = Join-Path $root ('entry-' + $Mode + '-' + [guid]::NewGuid().ToString('N') + '.log')
    # 不提供物理确认令牌；Launch只能验证拒绝分支。
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $engine -NoProfile -ExecutionPolicy Bypass -File $entry -Mode $Mode -Profile Soak `
            -PackageRoot $package -RunDirectory $run @Extra > $log 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    Assert (($code -eq 0) -eq $Success) ("入口${Mode}预期成功=${Success}；$log")
}
try {
    foreach ($name in @('XenLauncher.exe','runtimes/nvidia/Xen.exe','Start-Xen.cmd','tools/source/start_source_context_session.ps1')) {
        Write-Fixture (Join-Path $package $name) 'NOT EXECUTABLE: NEVER LAUNCH THIS FIXTURE'
    }
    Write-Fixture (Join-Path $package 'manifest.json') '{"schema":1,"product":"Xen","runtimes":[{"id":"nvidia","executable":"runtimes/nvidia/Xen.exe","backends":["tensorrt"]}]}'
    Write-Fixture (Join-Path $package 'config.ini') "[detector]`nbackend=tensorrt`n[auto_stop]`nexperimental_hud_model=true`n"
    Invoke-Entry Prepare $false
    Assert (-not (Test-Path -LiteralPath $run)) '旧Source-session无身份回执能力不能Prepare出可Launch假计划'
    Write-Fixture (Join-Path $package 'tools/source/start_source_context_session.ps1') "# XEN_ACCEPTANCE_LAUNCH_RECEIPT_SCHEMA=1`n# NONEXECUTABLE FIXTURE"
    Invoke-Entry Launch $false
    Assert (-not (Test-Path -LiteralPath $run)) '无物理授权时不创建Run或启动假包'
    Invoke-Entry Prepare
    Invoke-Entry Validate
    $task = Get-Content -LiteralPath (Join-Path $run 'task.json') -Raw | ConvertFrom-Json
    Assert ($task.task_id -ceq 'SOAK-RC-001' -and $task.profile -ceq 'Soak') 'Soak独立任务身份'
    Assert ($task.runtime_duration_seconds -eq 3600 -and $task.resource_max_duration_seconds -eq 4200 -and
        $task.duration_basis -ceq 'plan_default_60_minutes_not_user_confirmed') '60分钟及启动退出预算明示为未确认默认'
    Assert ($task.launch_command.Contains('-Profile Soak') -and
        $task.launch_command.Contains('-AllowPhysicalOutput -Confirm XEN_SOAK_SENDS_REAL_INPUT') -and
        (Get-Content -LiteralPath (Join-Path $run 'TASK.md') -Raw).Contains($task.launch_command)) '唯一Launch保留SOAK物理门且TASK一致'
    Assert (-not (Test-Path -LiteralPath (Join-Path $run 'launch.json'))) 'Prepare和Validate不启动监督'
    Invoke-Entry Validate $false @('-RuntimeDurationSeconds','2')
    $taskPath = Join-Path $run 'task.json'
    $originalTask = [IO.File]::ReadAllText($taskPath)
    $task.runtime_duration_seconds = 2
    Write-Fixture $taskPath ($task | ConvertTo-Json -Depth 16)
    Invoke-Entry Validate $false
    Write-Fixture $taskPath $originalTask
    Invoke-Entry Prepare $false
    Invoke-Entry Recover
    $notLaunched = Get-Content -LiteralPath (Join-Path $run 'automatic-summary.json') -Raw | ConvertFrom-Json
    Assert ($notLaunched.execution_status -ceq 'NOT_LAUNCHED' -and -not $notLaunched.physical_effect_verified) '未Launch回收不能认成执行'

    # 使用生产纯选择函数验证父关系/路径/时间/session，未模拟另一套算法。
    $now = [datetime]::UtcNow
    $parent = [pscustomobject]@{process_id=123; executable_path='C:\fixture\launcher.exe';
        start_time_utc=$now.ToString('o'); start_time_utc_ticks=$now.Ticks.ToString(); session_id=1}
    $candidate = [pscustomobject]@{ParentProcessId=123; ProcessId=456; ExecutablePath='C:\fixture\Xen.exe';
        CreationDate=$now.AddSeconds(1); SessionId=1}
    Assert ((Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate)).ProcessId -eq 456) '唯一精确子Worker可选择'
    $candidate.ParentProcessId=124
    Assert ($null -eq (Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate))) '同名不同父不得附加'
    $candidate.ParentProcessId=123; $candidate.ExecutablePath='C:\other\Xen.exe'
    Assert ($null -eq (Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate))) '同名异完整路径不得附加'
    $candidate.ExecutablePath='C:\fixture\Xen.exe'; $candidate.SessionId=2
    Assert-Throws { Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate) } '异交互会话拒绝'
    $candidate.SessionId=1; $candidate.CreationDate=$now.AddSeconds(-1)
    Assert-Throws { Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate) } '父PID复用创建顺序拒绝'
    $candidate.CreationDate=$now.AddSeconds(1)
    Assert-Throws { Select-XenSoakWorker $parent 'C:\fixture\Xen.exe' @($candidate,$candidate) } '多个候选拒绝而非取首个'

    # 自有无设备父子链；父与子均只休眠，不加载Xen二进制。
    $pidPath = Join-Path $root 'own-child.txt'
    $escapedPath = $pidPath.Replace("'", "''")
    $escapedEngine = $engine.Replace("'", "''")
    $code = "`$p=Start-Process -FilePath '$escapedEngine' -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 40' -WindowStyle Hidden -PassThru; [IO.File]::WriteAllText('$escapedPath',[string]`$p.Id); `$p.WaitForExit()"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $ownParent = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-EncodedCommand',$encoded) -WindowStyle Hidden -PassThru
    $children.Add($ownParent)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while (-not [IO.File]::Exists($pidPath) -and $wait.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 50 }
    Assert ([IO.File]::Exists($pidPath)) '自有父子链在有界等待内建立'
    $ownChild = [Diagnostics.Process]::GetProcessById([int]([IO.File]::ReadAllText($pidPath)))
    $children.Add($ownChild)
    $parentIdentity = Get-XenProcessIdentity $ownParent
    $childIdentity = Find-XenSoakWorker $parentIdentity $engine
    Assert ($childIdentity.process_id -eq $ownChild.Id) '实际父子链按生产函数完成路径启动时间与session复核'
    $changedIdentity = $childIdentity | Select-Object *
    $changedIdentity.start_time_utc_ticks = ([long]$changedIdentity.start_time_utc_ticks + 1).ToString()
    Assert ((Get-XenBoundProcessState $changedIdentity) -ceq 'identity_mismatch') '同PID不同启动Ticks不能冒充原进程'

    # 仅加载Source-session函数，既不解密凭据也不走Launcher入口。
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'start_source_context_session.ps1'),[ref]$tokens,[ref]$errors)
    Assert ($errors.Count -eq 0) 'Source-session脚本可解析'
    foreach ($function in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
        . ([scriptblock]::Create($function.Extent.Text))
    }
    $receiptPath = Join-Path $root 'launcher-identity.json'
    Write-LauncherReceipt $ownParent $receiptPath
    $receipt=Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    Assert (Test-XenProcessIdentityEqual $parentIdentity $receipt) '公开回执来自创建得到的真实Process对象'
    Assert-Throws { Write-LauncherReceipt $ownParent $receiptPath } '身份回执不覆盖已有证据'
    $startInfo=New-Object Diagnostics.ProcessStartInfo
    $startInfo.EnvironmentVariables['XEN_ACCEPTANCE_LAUNCH_RECEIPT']='public-fixture-path'
    Remove-LauncherReceiptEnvironment $startInfo
    Assert (-not $startInfo.EnvironmentVariables.ContainsKey('XEN_ACCEPTANCE_LAUNCH_RECEIPT')) 'Launcher子环境移除验收回执变量'

    $resources=Join-Path $root 'resource-ready'
    $null=New-Item -ItemType Directory -Path $resources
    Write-Fixture (Join-Path $resources 'summary.partial.json') ([ordered]@{
        target_process_id=$childIdentity.process_id; expected_executable_path=$childIdentity.executable_path;
        expected_start_time_utc=$childIdentity.start_time_utc } | ConvertTo-Json)
    Write-Fixture (Join-Path $resources 'samples.partial.csv') '"elapsed_seconds","private_bytes","threads"'
    Assert (-not (Test-XenResourceReady $resources $childIdentity)) '只有CSV表头不算资源首样本就绪'
    Write-Fixture (Join-Path $resources 'samples.partial.csv') "`"elapsed_seconds`",`"private_bytes`",`"threads`"`n`"0.2`",`"1024`",`"2`"`n"
    Assert (Test-XenResourceReady $resources $childIdentity) '有效首数据行才算资源就绪'

    # 监督函数只观察自有无设备链并启动只读采集器，不调用物理Launch入口。
    $supervisionRoot = Join-Path $root 'supervision'
    $null = New-Item -ItemType Directory -Path $supervisionRoot
    Write-LauncherReceipt $ownParent (Join-Path $supervisionRoot 'launcher-identity.json')
    $state = [ordered]@{supervision_status='interrupted'; launcher_identity=$null; worker_identity=$null;
        collector_identity=$null; resource_ready_utc=$null; resource_exit_code=$null; ended_utc=$null; failure_type=$null}
    $plan = [pscustomobject]@{startup_wait_budget_seconds=10;close_wait_budget_seconds=2;
        runtime_duration_seconds=1;resource_max_duration_seconds=1;resource_interval_milliseconds=100}
    Invoke-XenSoakSupervision -Task $plan -Launch $state -LauncherPath $engine -WorkerPath $engine `
        -RunDirectory $supervisionRoot -SamplerPath (Join-Path $PSScriptRoot 'measure_process_resources.ps1') `
        -SaveState { param($Record) Write-Fixture (Join-Path $supervisionRoot 'launch.json') ($Record|ConvertTo-Json -Depth 8) }
    Assert ($state.supervision_status -ceq 'resource_collector_ended_before_worker' -and
        $state.resource_exit_code -eq 0 -and $null -ne $state.resource_ready_utc) '监督采集窗口结束只标覆盖不足，不标长稳完成'
    Assert (-not $ownChild.HasExited -and -not $ownParent.HasExited) '监督结束不停止自有被观察父子进程'
    foreach ($restart in @($false, $true)) {
        $name = if ($restart) { 'supervision-restart' } else { 'supervision-exit' }
        $supervisionRoot = Join-Path $root $name
        $null = New-Item -ItemType Directory -Path $supervisionRoot
        $pidFile = Join-Path $supervisionRoot 'first-child.txt'
        $safeFile = $pidFile.Replace("'", "''")
        $restartFile = Join-Path $supervisionRoot 'next-child.txt'
        $safeNext = $restartFile.Replace("'", "''")
        $code = "`$p=Start-Process -FilePath '$escapedEngine' -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 2' -WindowStyle Hidden -PassThru; [IO.File]::WriteAllText('$safeFile',[string]`$p.Id); `$p.WaitForExit();"
        if ($restart) {
            $code += "`$p=Start-Process -FilePath '$escapedEngine' -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 30' -WindowStyle Hidden -PassThru; [IO.File]::WriteAllText('$safeNext',[string]`$p.Id); `$p.WaitForExit();"
        }
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
        $fakeParent = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-EncodedCommand',$encoded) -WindowStyle Hidden -PassThru
        $children.Add($fakeParent)
        $wait.Restart()
        while (-not [IO.File]::Exists($pidFile) -and $wait.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 30 }
        Assert ([IO.File]::Exists($pidFile)) '退出/重启假链有界建立'
        $firstChild = [Diagnostics.Process]::GetProcessById([int]([IO.File]::ReadAllText($pidFile)))
        $children.Add($firstChild)
        Write-LauncherReceipt $fakeParent (Join-Path $supervisionRoot 'launcher-identity.json')
        $state = [ordered]@{supervision_status='interrupted'; launcher_identity=$null; worker_identity=$null;
            collector_identity=$null; resource_ready_utc=$null; resource_exit_code=$null; ended_utc=$null; failure_type=$null}
        $plan.resource_max_duration_seconds = 10
        Invoke-XenSoakSupervision -Task $plan -Launch $state -LauncherPath $engine -WorkerPath $engine `
            -RunDirectory $supervisionRoot -SamplerPath (Join-Path $PSScriptRoot 'measure_process_resources.ps1') `
            -SaveState { param($Record) Write-Fixture (Join-Path $supervisionRoot 'launch.json') ($Record|ConvertTo-Json -Depth 8) }
        if ($restart) {
            $nextChild = [Diagnostics.Process]::GetProcessById([int]([IO.File]::ReadAllText($restartFile)))
            $children.Add($nextChild)
            Assert ($state.supervision_status -ceq 'worker_restarted' -and
                $state.worker_identity.process_id -eq $firstChild.Id -and -not $nextChild.HasExited) 'Worker重启不转附加、不代停、不拼连续时长'
        } else {
            Assert ($state.supervision_status -ceq 'worker_exited' -and $state.resource_exit_code -eq 2) '正常退出保留资源process_exited事实，不改写采集时长完成'
        }
    }

    $fakeLaunch=[pscustomobject]@{supervision_status='worker_exited'}
    foreach($state in @('duration_completed','process_exited','collection_failed')) {
        $result=Get-XenSoakRecoveryState $fakeLaunch ([pscustomobject]@{status=$state}) $true
        Assert (-not $result.soak_completed -and $result.runtime_evaluation -ceq 'pending_full_session_report_validation') "资源${state}不能代替Runtime连续时长证明"
    }
    $boundLaunch = [pscustomobject]@{supervision_status='worker_exited';worker_identity=$childIdentity}
    $boundResource = [pscustomobject]@{status='process_exited';target_process_id=$childIdentity.process_id;
        expected_executable_path=$childIdentity.executable_path;expected_start_time_utc=$childIdentity.start_time_utc}
    Assert ((Get-XenSoakRecoveryState $boundLaunch $boundResource $true).resource_identity_matches) '回收资源摘要仍须绑定同一Worker三元身份'
    $boundResource.target_process_id++
    Assert (-not (Get-XenSoakRecoveryState $boundLaunch $boundResource $true).resource_identity_matches) '别的Worker资源摘要不能冒充本Run附件'
    # 中断监督无ended_utc仍可收集已停止假Run的证据，不补造成功退出。
    $launch=[ordered]@{schema=1; started_utc=[datetime]::UtcNow.ToString('o'); before_files=@(); before_file_metadata=@();
        entrypoint_pid=$null; ended_utc=$null; exit_code=$null; supervision_status='interrupted'; worker_identity=$null}
    Write-Fixture (Join-Path $run 'launch.json') ($launch|ConvertTo-Json -Depth 8)
    Invoke-Entry Recover
    $recovered=Get-Content -LiteralPath (Join-Path $run 'automatic-summary.json') -Raw | ConvertFrom-Json
    Assert ($recovered.execution_status -ceq 'interrupted' -and -not $recovered.soak.soak_completed) '中断Run可回收但不伪完成'
    [ordered]@{status='passed'; checks=$checks.Count; powershell=$PSVersionTable.PSVersion.ToString(); evidence=$root} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'test-summary.json') -Encoding utf8
    Write-Output ("SOAK入口假包及无设备身份fixture通过：{0}项；{1}" -f $checks.Count,$root)
} finally {
    # 仅清理本fixture自己创建并持有的无设备Process对象。
    for($index=$children.Count-1;$index -ge 0;--$index) {
        $child=$children[$index]
        try { if(-not $child.HasExited){$child.Kill();$null=$child.WaitForExit(5000)} } finally {$child.Dispose()}
    }
}
