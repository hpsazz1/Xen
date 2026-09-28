param(
    [ValidateSet('Prepare', 'Validate', 'Launch', 'Recover')][string]$Mode = 'Prepare',
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [ValidateSet('HudStop', 'Soak')][string]$Profile = 'HudStop',
    [ValidateRange(1,82800)][int]$RuntimeDurationSeconds = 3600,
    [ValidateRange(100,60000)][int]$ResourceIntervalMilliseconds = 5000,
    [ValidateRange(1,1800)][int]$StartupWaitBudgetSeconds = 300,
    [ValidateRange(1,1800)][int]$CloseWaitBudgetSeconds = 300,
    [switch]$AllowPhysicalOutput,
    [string]$Confirm = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskId = if ($Profile -eq 'Soak') { 'SOAK-RC-001' } else { 'AUTO-STOP-HUD-EXPERIMENT-001' }
$confirmation = if ($Profile -eq 'Soak') { 'XEN_SOAK_SENDS_REAL_INPUT' } else { 'HUD_STOP_EXPERIMENT' }
if ($Mode -eq 'Launch') {
    if (-not $AllowPhysicalOutput -or $Confirm -cne $confirmation) {
        throw "Launch必须由用户前台提供-AllowPhysicalOutput -Confirm $confirmation。"
    }
} elseif ($AllowPhysicalOutput -or $Confirm) { throw '仅Launch接受物理输出授权。' }
if ($Profile -eq 'Soak') { . (Join-Path $PSScriptRoot 'soak_acceptance_support.ps1') }

function Assert-PlainPath([string]$Path, [bool]$MustExist = $true) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($MustExist -and -not (Test-Path -LiteralPath $full)) { throw "路径不存在：$full" }
    $part = $full
    while ($part) {
        if ((Test-Path -LiteralPath $part) -and
            ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw '任务路径不允许重解析点。'
        }
        $next = Split-Path -Parent $part
        if ($next -eq $part) { break }
        $part = $next
    }
    return $full.TrimEnd('\', '/')
}
function Get-Identity([string]$Path) {
    $path = Assert-PlainPath $Path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw '身份对象必须是文件。' }
    $stream = [IO.File]::OpenRead($path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
    return [ordered]@{ path = $path; sha256 = $hash }
}
function Write-Text([string]$Path, [string]$Value) {
    $null = Assert-PlainPath $Path $false
    [IO.File]::WriteAllText($Path, $Value, [Text.UTF8Encoding]::new($false))
}
function Write-Json([string]$Path, $Value) { Write-Text $Path ($Value | ConvertTo-Json -Depth 16) }
function Read-Json([string]$Path) { Get-Content -LiteralPath (Assert-PlainPath $Path) -Raw -Encoding UTF8 | ConvertFrom-Json }
function Get-IniValue([string]$Text, [string]$Section, [string]$Key) {
    $inside = $false
    $values = @()
    foreach ($line in ($Text -split "`n")) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') { $inside = $Matches[1] -ieq $Section; continue }
        if ($inside -and $line -match '^\s*([^;#=]+?)\s*=\s*(.*?)\s*$' -and $Matches[1] -ieq $Key) {
            $values += $Matches[2]
        }
    }
    if ($values.Count -ne 1) { throw "配置项必须唯一存在：[$Section] $Key" }
    return $values[0]
}
function Assert-Stopped {
    if (@(Get-Process -Name Xen, XenLauncher -ErrorAction SilentlyContinue).Count -gt 0) {
        throw '仍有Xen/XenLauncher运行；请用户退出全部版本，脚本不会强制结束进程。'
    }
}
function Get-ReportFiles {
    $files = @()
    foreach ($scope in @('cache/runtime', 'logs')) {
        $root = Join-Path $package $scope
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue((Assert-PlainPath $root))
        while ($pending.Count -gt 0) {
            foreach ($item in @(Get-ChildItem -LiteralPath $pending.Dequeue() -Force)) {
                $path = Assert-PlainPath $item.FullName
                if ($item.PSIsContainer) { $pending.Enqueue($path) }
                else { $files += $path }
            }
        }
    }
    return $files
}
$package = Assert-PlainPath $PackageRoot
$run = Assert-PlainPath $RunDirectory ($Mode -ne 'Prepare')
if ($package -ieq $run -or $run.StartsWith($package + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $package.StartsWith($run + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Run与包路径不得重叠。' }
if (-not (Test-Path -LiteralPath $package -PathType Container)) { throw '包根必须是目录。' }
$launcher = Join-Path $package 'XenLauncher.exe'
$entrypoint = Join-Path $package 'Start-Xen.cmd'
$configPath = Join-Path $package 'config.ini'
$manifestPath = Join-Path $package 'manifest.json'
if ($Mode -ne 'Recover') {
    $manifest = Read-Json $manifestPath
    $config = Get-Content -LiteralPath (Assert-PlainPath $configPath) -Raw -Encoding UTF8
    $backend = Get-IniValue $config 'detector' 'backend'
    $routes = @($manifest.runtimes | Where-Object { $backend -in $_.backends })
    if ($manifest.schema -ne 1 -or $manifest.product -cne 'Xen' -or $routes.Count -ne 1 -or
        $routes[0].id -cne 'nvidia' -or $routes[0].executable -cne 'runtimes/nvidia/Xen.exe') {
        throw '配置必须唯一选择正式NVIDIA Worker路由。'
    }
    if ($Profile -eq 'HudStop' -and (Get-IniValue $config 'auto_stop' 'experimental_hud_model') -ine 'true') {
        throw '本入口仅允许experimental_hud_model=true的HUD急停配置。'
    }
    if ($Profile -eq 'Soak') {
        $sourceSession = Get-Content -LiteralPath (Assert-PlainPath (
            Join-Path $package 'tools/source/start_source_context_session.ps1')) -Raw -Encoding UTF8
        if ($sourceSession -notmatch '(?m)^# XEN_ACCEPTANCE_LAUNCH_RECEIPT_SCHEMA=1\r?$') {
            throw '包内Source-session缺少公开Launcher身份回执合同，请先差量更新工具。'
        }
    }
}
$identityPaths = @($launcher, (Join-Path $package 'runtimes/nvidia/Xen.exe'), $manifestPath, $configPath, $PSCommandPath,
    $entrypoint, (Join-Path $package 'tools/source/start_source_context_session.ps1'))
if ($Profile -eq 'Soak') {
    $identityPaths += @((Join-Path $PSScriptRoot 'soak_acceptance_support.ps1'),
        (Join-Path $PSScriptRoot 'measure_process_resources.ps1'))
}
$identities = @($identityPaths | ForEach-Object {
    if ($Mode -eq 'Recover' -and $Profile -eq 'Soak' -and -not (Test-Path -LiteralPath $_)) {
        [ordered]@{ path = [IO.Path]::GetFullPath($_); sha256 = $null }
    } else { Get-Identity $_ }
})
if ($Mode -eq 'Prepare') {
    if (Test-Path -LiteralPath $run) { throw 'Run已存在，禁止覆盖。' }
    New-Item -ItemType Directory -Path $run | Out-Null
    Copy-Item -LiteralPath $configPath -Destination (Join-Path $run 'config.ini')
    $quote = { param([string]$Value) "'" + $Value.Replace("'", "''") + "'" }
    $command = 'powershell -NoProfile -ExecutionPolicy Bypass -File ' + (& $quote $PSCommandPath) +
        ' -Mode Launch -PackageRoot ' + (& $quote $package) + ' -RunDirectory ' + (& $quote $run) +
        $(if ($Profile -eq 'Soak') { ' -Profile Soak' } else { '' }) +
        ' -AllowPhysicalOutput -Confirm ' + $confirmation
    $preparedTask = [ordered]@{
        schema = 1; task_id = $taskId; prepared_utc = [DateTime]::UtcNow.ToString('o')
        package_root = $package; run_directory = $run; identities = $identities
        snapshot_sha256 = (Get-Identity (Join-Path $run 'config.ini')).sha256
        launch_command = $command; automatic_arm = $false; automatic_fire = $false
    }
    if ($Profile -eq 'Soak') {
        $preparedTask.profile = 'Soak'
        $preparedTask.runtime_duration_seconds = $RuntimeDurationSeconds
        $preparedTask.resource_interval_milliseconds = $ResourceIntervalMilliseconds
        $preparedTask.startup_wait_budget_seconds = $StartupWaitBudgetSeconds
        $preparedTask.close_wait_budget_seconds = $CloseWaitBudgetSeconds
        $preparedTask.resource_max_duration_seconds = $RuntimeDurationSeconds + $StartupWaitBudgetSeconds + $CloseWaitBudgetSeconds
        $preparedTask.duration_basis = if ($PSBoundParameters.ContainsKey('RuntimeDurationSeconds')) {
            'explicit_prepare_parameter'
        } else { 'plan_default_60_minutes_not_user_confirmed' }
        $preparedTask.plan_sha256 = Get-XenSoakPlanHash $preparedTask
    }
    Write-Json (Join-Path $run 'task.json') $preparedTask
    if ($Profile -eq 'Soak') {
        Write-Text (Join-Path $run 'TASK.md') @"
# 发布候选长稳

任务SOAK-RC-001；仅Prepare，尚未执行。Runtime目标$RuntimeDurationSeconds 秒；时长来源$($preparedTask.duration_basis)。默认60分钟不是用户确认，修改须重新Prepare。
资源周期$ResourceIntervalMilliseconds 毫秒；最大窗口$($preparedTask.resource_max_duration_seconds)秒，包含启动等待$StartupWaitBudgetSeconds 秒与退出保存$CloseWaitBudgetSeconds 秒。
config.ini是证据副本；入口使用包原目录配置，不修改控制参数。

1. 退出其他版本。用户在目标机本地前台执行下方唯一命令。
2. 等待控制台确认Worker身份及首条资源样本就绪，再在UI启动Runtime并自行武装；需要录像则先开始录像。
3. 连续运行计划时长；普通使用中记录异常及恢复，不重复End/AD专项。出现异常由用户自行停止。
4. 到时仅提示，不自动停止Runtime、退出、按键或开火；用户停止Runtime保存报告并退出UI。
5. UI存活/资源采样够时长不等于Runtime长稳；回收时按一个Runtime的全摄入跨度及覆盖独立核对。重启Runtime或Worker不得拼接时长。

## 唯一Launch命令

``````powershell
$command
``````

仅用户前台执行。资源采集器只读，不代表Runtime验收；结束后回复观察，由代理Recover。
"@
        Write-Text (Join-Path $run 'OBSERVATION.md') "# 长稳人工观察`n`n尚未执行；记录实际Runtime开始/停止、场景、异常、恢复和结论。不重复已确认End/AD专项。`n"
        Write-Output $command
        return
    }
    Write-Text (Join-Path $run 'TASK.md') @"
# HUD急停人工复测

任务 $taskId（保留历史任务标识兼容）；当前仅Prepare，未执行。config.ini是证据副本；启动仍使用包原配置。
HUD模型是输入评估模型经Xen控制适配，软件状态/ACK不证明游戏已停稳。

1. 源游戏前台确认焦点、GSI与源时钟有效，保持同一武器、场景、灵敏度、目标和参数。
2. 退出其他Xen版本，再运行下方唯一命令打开当前HUD包；仅专项对照时另行安排H40比较。
3. 先开始录像，再由用户在UI启动Runtime并人工武装；持续方向+功能键、AD来回、连续换目标与移动恢复逐项观察。
4. 切刀再返回枪，分别保持功能键和松开重按；观察是否需人为松方向才开火、目标消失是否释放。
5. 验证End总急停。出现异常立即松键、End、停止Runtime并退出UI；不要同时运行两包。
6. 每段建议不超过30秒，由用户停止Runtime并保存录像；补录前也先停止Runtime，再开录像并启动新的Runtime会话，避免多段混入同一报告。
   当前报告最多保留10000个样本，240帧/秒约覆盖最后42秒；更高帧率或更长时段仍可能覆盖，30秒不保证完整，回收后须核对报告覆盖计数。
   不要改变其他参数。结束后把观察直接回复当前任务。

## 唯一Launch命令

``````powershell
$command
``````

命令打开真实KMBOX输出入口，仅用户本轮前台执行；脚本不自动武装、按键、开火或强制结束进程。
用户退出后代理Recover；进程启动/退出均不代表实际停稳或人工通过。
"@
    Write-Text (Join-Path $run 'OBSERVATION.md') "# 人工观察`n`n尚未执行；执行人、时间、场景、各组比较、End急停和结论均未填写。`n"
    Write-Output $command
    return
}
$task = Read-Json (Join-Path $run 'task.json')
if ($task.schema -ne 1 -or $task.task_id -cne $taskId -or $task.package_root -ine $package -or
    $task.run_directory -ine $run -or $task.automatic_arm -or $task.automatic_fire) { throw '任务身份不符。' }
if ($Profile -eq 'Soak') {
    Assert-XenSoakTask $task
    foreach ($binding in @(
        @('RuntimeDurationSeconds','runtime_duration_seconds'),
        @('ResourceIntervalMilliseconds','resource_interval_milliseconds'),
        @('StartupWaitBudgetSeconds','startup_wait_budget_seconds'),
        @('CloseWaitBudgetSeconds','close_wait_budget_seconds'))) {
        if ($PSBoundParameters.ContainsKey($binding[0]) -and
            [int]$PSBoundParameters[$binding[0]] -ne [int]$task.($binding[1])) {
            throw 'Launch/Validate/Recover不得修改Prepare冻结的时长或周期。'
        }
    }
}
if ($Mode -in @('Validate', 'Launch')) {
    if (@($task.identities).Count -ne $identities.Count) { throw '绑定身份数量不符。' }
    for ($i = 0; $i -lt $identities.Count; $i++) {
        if ($task.identities[$i].path -ine $identities[$i].path -or
            $task.identities[$i].sha256 -cne $identities[$i].sha256) { throw 'Prepare绑定文件已变化，请重新准备。' }
    }
    if ((Get-Identity (Join-Path $run 'config.ini')).sha256 -cne $task.snapshot_sha256) { throw '配置证据副本已变化。' }
    if ($Mode -eq 'Validate') { Write-Output '绑定身份验证通过；未启动程序、未发送物理输入。'; return }
    if ($package.StartsWith('\\') -or $run.StartsWith('\\')) { throw 'Launch必须由用户在目标机本地前台运行。' }
    if (Test-Path -LiteralPath (Join-Path $run 'launch.json')) { throw '禁止重复Launch，请重新Prepare。' }
    Assert-Stopped
    $beforeFiles = @(Get-ReportFiles)
    $beforeMetadata = @($beforeFiles | ForEach-Object {
        $item = Get-Item -LiteralPath $_
        [ordered]@{ path = $_; length = $item.Length; last_write_utc_ticks = $item.LastWriteTimeUtc.Ticks }
    })
    $launch = [ordered]@{ schema = 1; started_utc = [DateTime]::UtcNow.ToString('o')
        before_files = $beforeFiles; before_file_metadata = $beforeMetadata
        entrypoint_pid = $null; ended_utc = $null; exit_code = $null }
    if ($Profile -eq 'Soak') {
        $launch.supervision_status = 'interrupted'
        $launch.worker_identity = $null
        $launch.launcher_identity = $null
        $launch.collector_identity = $null
        $launch.resource_ready_utc = $null
        $launch.resource_exit_code = $null
        $launch.failure_type = $null
        $launch.runtime_duration_notice_basis = 'worker_resource_clock_not_runtime_clock'
    }
    Write-Json (Join-Path $run 'launch.json') $launch
    if ($Profile -eq 'Soak') {
        $savedReceipt = [Environment]::GetEnvironmentVariable('XEN_ACCEPTANCE_LAUNCH_RECEIPT')
        $process = $null
        try {
            [Environment]::SetEnvironmentVariable('XEN_ACCEPTANCE_LAUNCH_RECEIPT', (Join-Path $run 'launcher-identity.json'))
            # 正式Start-Xen原入口保持SourceContext注入；公开回执由Source-session消费。
            $process = Start-Process -FilePath $entrypoint -WorkingDirectory $package -WindowStyle Normal -PassThru
        } finally { [Environment]::SetEnvironmentVariable('XEN_ACCEPTANCE_LAUNCH_RECEIPT', $savedReceipt) }
        try {
            $launch.entrypoint_pid = $process.Id
            Write-Json (Join-Path $run 'launch.json') $launch
            Invoke-XenSoakSupervision -Task $task -Launch $launch -LauncherPath $launcher `
                -WorkerPath (Join-Path $package 'runtimes/nvidia/Xen.exe') -RunDirectory $run `
                -SamplerPath (Join-Path $PSScriptRoot 'measure_process_resources.ps1') `
                -SaveState { param($Record) Write-Json (Join-Path $run 'launch.json') $Record }
            if ($launch.supervision_status -cne 'worker_exited') {
                throw '长稳监督未完整收尾，证据已保留；请按launch.json状态处理，不代表Runtime通过。'
            }
        } finally { if ($null -ne $process) { $process.Dispose() } }
        return
    }
    Write-Host '即将打开HUD包UI；用户自行启动Runtime与武装，End急停，完成后退出。'
    # 复用发布包入口注入本机SourceContext；不读取或输出任何凭据内容。
    $process = Start-Process -FilePath $entrypoint -WorkingDirectory $package -WindowStyle Normal -PassThru
    $launch.entrypoint_pid = $process.Id
    Write-Json (Join-Path $run 'launch.json') $launch
    $process.WaitForExit()
    $launch.ended_utc = [DateTime]::UtcNow.ToString('o')
    $launch.exit_code = $process.ExitCode
    Write-Json (Join-Path $run 'launch.json') $launch
    return
}
if ($Profile -eq 'HudStop') { Assert-Stopped }
$launchPath = Join-Path $run 'launch.json'
if (-not (Test-Path -LiteralPath $launchPath)) {
    Write-Json (Join-Path $run 'automatic-summary.json') ([ordered]@{
        schema = 1; task_id = $taskId; execution_status = 'NOT_LAUNCHED'; collected_files = @()
        physical_effect_verified = $false; human_observation_required = $true
    })
    return
}
$launch = Read-Json $launchPath
if ($Profile -eq 'HudStop') {
    if (-not $launch.entrypoint_pid -or -not $launch.ended_utc) { throw '启动入口记录尚未结束，无法回收；入口退出不代表UI或实机测试完成。' }
} elseif ($null -ne $launch.worker_identity) {
    if ((Get-XenBoundProcessState $launch.worker_identity) -ceq 'running') {
        throw '本Run绑定Worker仍运行，请用户停止并退出后回收；脚本不会代停。'
    }
} else { Assert-Stopped }
if ($Profile -eq 'Soak' -and $null -ne $launch.PSObject.Properties['replacement_worker_identity'] -and
    $null -ne $launch.replacement_worker_identity -and
    (Get-XenBoundProcessState $launch.replacement_worker_identity) -ceq 'running') {
    throw '本Run已记录重启Worker且仍运行，请用户退出后回收；不能拼接会话。'
}
$collected = @()
$hasMetadata = $null -ne $launch.PSObject.Properties['before_file_metadata']
$beforeByPath = @{}
if ($hasMetadata) {
    foreach ($item in @($launch.before_file_metadata)) { $beforeByPath[$item.path] = $item }
}
foreach ($file in @(Get-ReportFiles)) {
    $item = Get-Item -LiteralPath $file
    if ($hasMetadata -and $beforeByPath.ContainsKey($file)) {
        $before = $beforeByPath[$file]
        if ($item.Length -eq $before.length -and $item.LastWriteTimeUtc.Ticks -eq $before.last_write_utc_ticks) { continue }
    } elseif ($item.LastWriteTimeUtc -lt [DateTime]::Parse($launch.started_utc).ToUniversalTime()) { continue }
    # 历史Run没有长度/时间元数据，保守收集启动后修改的文件；整份日志可能含历史Run，分析需再按Run筛选。
    $relative = $file.Substring($package.Length + 1)
    $target = Assert-PlainPath (Join-Path (Join-Path $run 'reports') $relative) $false
    if (Test-Path -LiteralPath $target) {
        if ((Get-Identity $target).sha256 -cne (Get-Identity $file).sha256) { throw '已回收文件变化，禁止覆盖。' }
    } else {
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $file -Destination $target
    }
    $collected += Get-Identity $target
}
$automatic = [ordered]@{
    schema = 1; task_id = $taskId; execution_status = 'ENTRYPOINT_EXITED'; entrypoint_exit_code = $launch.exit_code
    collected_files = $collected; physical_effect_verified = $false; human_observation_required = $true
    collection_basis = $(if ($hasMetadata) { 'LENGTH_OR_WRITE_TIME_CHANGED' } else { 'LEGACY_WRITE_TIME_FALLBACK' })
    reports_may_contain_other_runs = $true
    config_changed_during_ui = (-not (Test-Path -LiteralPath $configPath) -or
        (Get-Identity $configPath).sha256 -cne $task.snapshot_sha256)
}
if ($Profile -eq 'Soak') {
    $unchanged = $identities.Count -eq @($task.identities).Count
    if ($unchanged) {
        for ($i = 0; $i -lt $identities.Count; ++$i) {
            if ($identities[$i].path -ine $task.identities[$i].path -or
                $identities[$i].sha256 -cne $task.identities[$i].sha256) { $unchanged = $false }
        }
    }
    $resourcePath = Join-Path $run 'resources/summary.json'
    $resource = if (Test-Path -LiteralPath $resourcePath) { Read-Json $resourcePath } else { $null }
    $automatic.soak = Get-XenSoakRecoveryState $launch $resource $unchanged
    $automatic.execution_status = $launch.supervision_status
    $automatic.resource_attachments = @()
    foreach ($name in @('summary.json','samples.csv','summary.partial.json','samples.partial.csv')) {
        $attachment = Join-Path (Join-Path $run 'resources') $name
        if (Test-Path -LiteralPath $attachment -PathType Leaf) {
            $automatic.resource_attachments += Get-Identity $attachment
        }
    }
}
Write-Json (Join-Path $run 'automatic-summary.json') $automatic
Write-Output "回收$($collected.Count)份新增或变化文件；完整日志可能含其他Run，人工效果未判定。"
