param(
    [ValidateSet('Prepare', 'Launch', 'Recover')][string]$Mode = 'Prepare',
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$RunDirectory,
    [ValidateRange(30, 300)][int]$RecommendedSeconds = 120,
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$confirmation = 'XEN_RECOIL_LEGACY_ACCEPT_SENDS_REAL_KMBOX_INPUT'
if ($Mode -eq 'Launch') {
    if (-not $AllowPhysicalOutput -or $PhysicalOutputConfirmation -cne $confirmation) {
        throw 'Launch会发送真实KMBOX输入，必须由用户提供-AllowPhysicalOutput和固定确认令牌。'
    }
} elseif ($AllowPhysicalOutput -or $PhysicalOutputConfirmation) {
    throw 'Prepare/Recover不接受物理输出授权。'
}
function Assert-PlainPath([string]$Path, [bool]$MustExist = $true) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($MustExist -and -not (Test-Path -LiteralPath $full)) { throw "路径不存在：$full" }
    $part = $full
    while ($part) {
        if (Test-Path -LiteralPath $part) {
            if (((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw '任务路径不允许重定向。'
            }
        }
        $next = Split-Path -Parent $part
        if ($next -eq $part) { break }
        $part = $next
    }
    return $full
}
function Get-Identity([string]$Path) {
    $path = Assert-PlainPath $Path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw '身份必须是普通文件。' }
    return [ordered]@{ path = $path; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 16), [Text.UTF8Encoding]::new($false))
}
function Get-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Fallback = '') {
    $inside = $false
    foreach ($line in ($Text -split "`n")) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') { $inside = $Matches[1] -ieq $Section; continue }
        if ($inside -and $line -match '^\s*([^;#=]+?)\s*=\s*(.*?)\s*$' -and $Matches[1] -ieq $Key) {
            return $Matches[2]
        }
    }
    return $Fallback
}
function Assert-PackageStopped([string]$Root) {
    foreach ($process in @(Get-Process -Name Xen, XenLauncher -ErrorAction SilentlyContinue)) {
        if (-not $process.Path -or $process.Path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw '该包UI/Worker仍在运行；请用户退出后再操作，不强制结束进程。'
        }
    }
}
$package = (Assert-PlainPath $PackageRoot).TrimEnd('\')
$run = (Assert-PlainPath $RunDirectory ($Mode -ne 'Prepare')).TrimEnd('\')
$launcher = Join-Path $package 'XenLauncher.exe'
$runtimeReports = Join-Path $package 'cache/runtime'
if ($Mode -eq 'Prepare') {
    if (Test-Path -LiteralPath $run) { throw 'Run目录已存在，请另建任务，禁止覆盖。' }
    if ($run -ieq $package -or $run.StartsWith($runtimeReports + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw '任务不能覆盖包根或嵌入Runtime报告目录。'
    }
    $configPath = Join-Path $package 'config.ini'
    $configText = Get-Content -LiteralPath (Assert-PlainPath $configPath) -Raw -Encoding UTF8
    $manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $backend = Get-IniValue $configText 'detector' 'backend'
    $routes = @($manifest.runtimes | Where-Object { $backend -in $_.backends })
    if ($manifest.schema -ne 1 -or $manifest.product -cne 'Xen' -or $routes.Count -ne 1 -or $routes[0].id -cne 'nvidia') {
        throw '原config必须唯一选择已验证NVIDIA运行时。'
    }
    $workerRelative = [string]$routes[0].executable
    if ($workerRelative -cne 'runtimes/nvidia/Xen.exe') { throw 'Worker路由不是正式NVIDIA入口。' }
    $directory = Get-IniValue $configText 'recoil' 'profile_directory' 'cache/recoil/profiles'
    $profileRoot = Assert-PlainPath $(if ([IO.Path]::IsPathRooted($directory)) { $directory } else { Join-Path $package $directory })
    $activePath = Join-Path $profileRoot 'active.json'
    $active = Get-Content -LiteralPath (Assert-PlainPath $activePath) -Raw -Encoding UTF8 | ConvertFrom-Json
    $entries = @($active.active.PSObject.Properties)
    if ($active.schema_version -ne 1 -or $entries.Count -ne 17) { throw '本次任务要求已迁移17条活动曲线。' }
    $identities = @((Get-Identity $launcher), (Get-Identity (Join-Path $package $workerRelative)),
        (Get-Identity (Join-Path $package 'manifest.json')), (Get-Identity $configPath), (Get-Identity $activePath), (Get-Identity $PSCommandPath))
    foreach ($entry in $entries) {
        $file = [string]$entry.Value.file
        if ($file -cnotmatch '^[A-Za-z0-9_-]+\.json$' -or $file -ceq 'active.json') { throw '活动曲线文件名无效。' }
        $path = Assert-PlainPath (Join-Path $profileRoot $file)
        $profile = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($profile.schema_version -ne 3 -or $profile.verified -isnot [bool] -or -not $profile.verified) {
            throw '必须先完成已确认离散曲线迁移，Prepare不能伪造确认。'
        }
        $identities += Get-Identity $path
    }
    New-Item -ItemType Directory -Path $run | Out-Null
    Copy-Item -LiteralPath $configPath -Destination (Join-Path $run 'config.ini')
    $quote = { param([string]$Value) "'" + $Value.Replace("'", "''") + "'" }
    $launchCommand = 'powershell -NoProfile -ExecutionPolicy Bypass -File ' + (& $quote $PSCommandPath) +
        ' -Mode Launch -PackageRoot ' + (& $quote $package) + ' -RunDirectory ' + (& $quote $run) +
        ' -AllowPhysicalOutput -PhysicalOutputConfirmation ' + $confirmation
    $task = [ordered]@{ schema = 1; task_id = 'RECOIL-LEGACY-PARITY-001'; run_id = (Split-Path -Leaf $run)
        prepared_utc = [DateTime]::UtcNow.ToString('o'); package_root = $package; run_directory = $run
        recommended_seconds = $RecommendedSeconds; automatic_timeout = $false; automatic_fire = $false
        original_recoil_enabled = (Get-IniValue $configText 'recoil' 'enabled' 'false')
        original_mixed_aim = (Get-IniValue $configText 'recoil' 'mixed_aim' 'false')
        identities = $identities; snapshot_sha256 = (Get-Identity (Join-Path $run 'config.ini')).sha256
        launch_command = $launchCommand; physical_output_started = $false }
    Write-Json (Join-Path $run 'task.json') $task
    $instructions = @"
# 普通压枪旧版兼容人工确认

任务 RECOIL-LEGACY-PARITY-001。仅Prepare；未发送真实输入。
原包配置完整保留，当前压枪enabled=$($task.original_recoil_enabled)，mixed_aim=$($task.original_mixed_aim)。副本config.ini仅为证据，不作为CLI配置。
建议人工窗口$RecommendedSeconds 秒，不是Runtime或硬件强制超时；脚本只启动UI并等待用户退出，不自动武装、不按键、不射击、不强杀。

1. 在源游戏前台确认焦点/GSI有效、灵敏度与曲线一致。End总急停和松开开火保持可用。
2. 用户在UI启用普通压枪，确认自动扳机关闭，第一组保持Aim关闭；启动Runtime、人工武装，手动开火完成AK47整段，松键后再试重按。不要保存或覆盖原包config。
3. 手动选择代表武器M4A4、MP9、Negev重复，确认首段、后段及换向；最后用户启用Aim和混合，确认Recoil独占Y、Aim继续X且退出恢复正常。其余武器逐项补验。
4. 到建议窗口或异常时，用户松开开火、End/停止Runtime，退出UI。将真实观察直接回复当前任务；不要求手填OBSERVATION.md。

## 唯一Launch命令

``````powershell
$launchCommand
``````

命令会启用真实KMBOX输出入口，只能由用户本轮前台运行。软件日志/ACK不是游戏效果通过。
完成后代理以同一脚本Mode Recover回收本次新增Debug文件；没有Launch记录不能当执行证据。
"@
    [IO.File]::WriteAllText((Join-Path $run 'TASK.md'), $instructions, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $run 'OBSERVATION.md'), "# 人工观察`n`n尚未执行，未填写人工结论。`n", [Text.UTF8Encoding]::new($false))
    Write-Output $launchCommand
    return
}
$task = Get-Content -LiteralPath (Join-Path $run 'task.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($task.schema -ne 1 -or $task.task_id -cne 'RECOIL-LEGACY-PARITY-001' -or
    $task.package_root -ine $package -or $task.run_directory -ine $run -or $task.automatic_timeout -or $task.automatic_fire) {
    throw '任务身份或运行路径不符。'
}
if ($Mode -eq 'Launch') {
    if ($package.StartsWith('\\')) { throw 'Launch只能由用户在目标机本地前台运行。' }
    if (Test-Path -LiteralPath (Join-Path $run 'launch.json')) { throw '此Run已启动过，禁止重复Launch；请新建Prepare。' }
    foreach ($identity in $task.identities) {
        if ((Get-Identity $identity.path).sha256 -cne $identity.sha256) { throw 'Prepare绑定文件已变化，请重新准备。' }
    }
    if ((Get-Identity (Join-Path $run 'config.ini')).sha256 -cne $task.snapshot_sha256) { throw '配置证据副本已变化。' }
    Assert-PackageStopped $package
    $before = @()
    if (Test-Path -LiteralPath $runtimeReports) { $before = @(Get-ChildItem -LiteralPath (Assert-PlainPath $runtimeReports) -Force | ForEach-Object { $_.Name }) }
    $launch = [ordered]@{ schema = 1; started_utc = [DateTime]::UtcNow.ToString('o'); before_names = $before
        launcher_pid = $null; ended_utc = $null; exit_code = $null; manual_window_seconds = $task.recommended_seconds }
    Write-Json (Join-Path $run 'launch.json') $launch
    Write-Host '即将打开原包UI；由用户启用压枪、启动Runtime、武装和手动射击。End急停；建议窗口结束自行停止并退出。'
    $process = Start-Process -FilePath $launcher -WorkingDirectory $package -WindowStyle Normal -PassThru
    $launch.launcher_pid = $process.Id
    Write-Json (Join-Path $run 'launch.json') $launch
    $process.WaitForExit()
    $launch.ended_utc = [DateTime]::UtcNow.ToString('o')
    $launch.exit_code = $process.ExitCode
    Write-Json (Join-Path $run 'launch.json') $launch
    Write-Host 'UI已退出。请把人工观察回复当前任务，随后Recover收集报告。'
    return
}
Assert-PackageStopped $package
$launch = Get-Content -LiteralPath (Join-Path $run 'launch.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $launch.launcher_pid -or -not $launch.ended_utc) { throw 'Launch未确认结束，不能自动认定本轮已完成。' }
$collected = Join-Path $run 'reports'
New-Item -ItemType Directory -Path $collected -Force | Out-Null
$files = @()
if (Test-Path -LiteralPath $runtimeReports) {
    foreach ($entry in @(Get-ChildItem -LiteralPath (Assert-PlainPath $runtimeReports) -Force)) {
        if ($entry.Name -in $launch.before_names) { continue }
        if ($entry.Name -notmatch '^[A-Za-z0-9_-]+\.(json|csv)$' -and $entry.Name -notmatch '^[A-Za-z0-9_-]+-recoil-batches$') { continue }
        if ($entry.PSIsContainer) {
            $pending = [Collections.Generic.Queue[string]]::new()
            $pending.Enqueue((Assert-PlainPath $entry.FullName))
            while ($pending.Count -gt 0) {
                foreach ($file in @(Get-ChildItem -LiteralPath $pending.Dequeue() -Force)) {
                    $null = Assert-PlainPath $file.FullName
                    if ($file.PSIsContainer) { $pending.Enqueue($file.FullName); continue }
                    $relative = $file.FullName.Substring($runtimeReports.Length + 1)
                    $target = Join-Path $collected $relative
                    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                    Copy-Item -LiteralPath $file.FullName -Destination $target -Force
                    $files += Get-Identity $target
                }
            }
        } else {
            $null = Assert-PlainPath $entry.FullName
            $target = Join-Path $collected $entry.Name
            Copy-Item -LiteralPath $entry.FullName -Destination $target -Force
            $files += Get-Identity $target
        }
    }
}
Write-Json (Join-Path $run 'automatic-summary.json') ([ordered]@{ schema = 1; task_id = $task.task_id
    launcher_exit_code = $launch.exit_code; collected_files = $files; has_new_reports = ($files.Count -gt 0)
    physical_effect_verified = $false; human_observation_required = $true
    config_changed_during_ui = ((Get-Identity (Join-Path $package 'config.ini')).sha256 -cne $task.snapshot_sha256) })
Write-Host "已回收本次新增报告$($files.Count)份；未将报告或ACK判为人工效果通过。"
