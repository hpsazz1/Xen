param(
    [ValidateSet('Prepare', 'Validate', 'Launch', 'Recover')][string]$Mode = 'Prepare',
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [switch]$AllowPhysicalOutput,
    [string]$Confirm = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskId = 'AUTO-STOP-HUD-EXPERIMENT-001'
if ($Mode -eq 'Launch') {
    if (-not $AllowPhysicalOutput -or $Confirm -cne 'HUD_STOP_EXPERIMENT') {
        throw 'Launch必须由用户前台提供-AllowPhysicalOutput -Confirm HUD_STOP_EXPERIMENT。'
    }
} elseif ($AllowPhysicalOutput -or $Confirm) { throw '仅Launch接受物理输出授权。' }

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
    if ((Get-IniValue $config 'auto_stop' 'experimental_hud_model') -ine 'true') {
        throw '本入口仅允许experimental_hud_model=true的独立HUD实验包。'
    }
}
$identityPaths = @($launcher, (Join-Path $package 'runtimes/nvidia/Xen.exe'), $manifestPath, $configPath, $PSCommandPath,
    $entrypoint, (Join-Path $package 'tools/source/start_source_context_session.ps1'))
$identities = @($identityPaths | ForEach-Object { Get-Identity $_ })
if ($Mode -eq 'Prepare') {
    if (Test-Path -LiteralPath $run) { throw 'Run已存在，禁止覆盖。' }
    New-Item -ItemType Directory -Path $run | Out-Null
    Copy-Item -LiteralPath $configPath -Destination (Join-Path $run 'config.ini')
    $quote = { param([string]$Value) "'" + $Value.Replace("'", "''") + "'" }
    $command = 'powershell -NoProfile -ExecutionPolicy Bypass -File ' + (& $quote $PSCommandPath) +
        ' -Mode Launch -PackageRoot ' + (& $quote $package) + ' -RunDirectory ' + (& $quote $run) +
        ' -AllowPhysicalOutput -Confirm HUD_STOP_EXPERIMENT'
    Write-Json (Join-Path $run 'task.json') ([ordered]@{
        schema = 1; task_id = $taskId; prepared_utc = [DateTime]::UtcNow.ToString('o')
        package_root = $package; run_directory = $run; identities = $identities
        snapshot_sha256 = (Get-Identity (Join-Path $run 'config.ini')).sha256
        launch_command = $command; automatic_arm = $false; automatic_fire = $false
    })
    Write-Text (Join-Path $run 'TASK.md') @"
# HUD急停独立实验人工比较

任务 $taskId；当前仅Prepare，未执行。config.ini是证据副本；启动仍使用独立包原配置。
HUD模型是输入评估模型经Xen控制适配，软件状态/ACK不证明游戏已停稳，不预设优于原版。

1. 源游戏前台确认焦点、GSI与源时钟有效，保持同一武器、场景、灵敏度、目标和参数。
2. 先用原H40包记录同条件表现，停止Runtime并退出原包。再运行下方唯一命令打开独立实验包。
3. 用户在UI启动Runtime并人工武装；持续方向+功能键、AD来回、目标消失后恢复逐项比较。
4. 切刀再返回枪，分别保持功能键和松开重按；观察是否需人为松方向才开火、目标消失是否释放。
5. 验证End总急停。出现异常立即松键、End、停止Runtime并退出UI；不要同时运行两包。
6. 每组建议不超过120秒，由用户自行停止。不要改变其他参数。结束后把观察直接回复当前任务。

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
    $launch = [ordered]@{ schema = 1; started_utc = [DateTime]::UtcNow.ToString('o')
        before_files = @(Get-ReportFiles); entrypoint_pid = $null; ended_utc = $null; exit_code = $null }
    Write-Json (Join-Path $run 'launch.json') $launch
    Write-Host '即将打开实验包UI；用户自行启动Runtime与武装，End急停，完成后退出。'
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
Assert-Stopped
$launchPath = Join-Path $run 'launch.json'
if (-not (Test-Path -LiteralPath $launchPath)) {
    Write-Json (Join-Path $run 'automatic-summary.json') ([ordered]@{
        schema = 1; task_id = $taskId; execution_status = 'NOT_LAUNCHED'; collected_files = @()
        physical_effect_verified = $false; human_observation_required = $true
    })
    return
}
$launch = Read-Json $launchPath
if (-not $launch.entrypoint_pid -or -not $launch.ended_utc) { throw '启动入口记录尚未结束，无法回收；入口退出不代表UI或实机测试完成。' }
$collected = @()
foreach ($file in @(Get-ReportFiles)) {
    if ($file -in $launch.before_files) { continue }
    if ((Get-Item -LiteralPath $file).LastWriteTimeUtc -lt [DateTime]::Parse($launch.started_utc).ToUniversalTime()) { continue }
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
Write-Json (Join-Path $run 'automatic-summary.json') ([ordered]@{
    schema = 1; task_id = $taskId; execution_status = 'ENTRYPOINT_EXITED'; entrypoint_exit_code = $launch.exit_code
    collected_files = $collected; physical_effect_verified = $false; human_observation_required = $true
    config_changed_during_ui = ((Get-Identity $configPath).sha256 -cne $task.snapshot_sha256)
})
Write-Output "回收$($collected.Count)份本轮新增文件；人工效果未判定。"
