#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LibraryDirectory,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OptionsPath,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# 从 PowerShell 7 调用 Windows PowerShell 5 时，优先使用当前宿主自带模块。
$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath

function Get-LineupField($Object, [string]$Name, $Default = $null) {
    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}

function Resolve-LineupLocalPath([string]$Path, [string]$Label) {
    # 在访问文件前拒绝 UNC、映射网络盘及重解析点，采集和预检不触及辅机。
    if ($Path -notmatch '^[A-Za-z]:[\\/]' -or $Path.Substring(2).Contains(':')) {
        throw "$Label 必须是本机固定磁盘的绝对路径。"
    }
    $full = [IO.Path]::GetFullPath($Path)
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($full))
    if ($drive.DriveType -ne [IO.DriveType]::Fixed) { throw "$Label 必须位于本机固定磁盘。" }
    $ancestors = [Collections.Generic.List[string]]::new()
    $cursor = $full
    while ($cursor) {
        $ancestors.Add($cursor)
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    $ancestors.Reverse()
    foreach ($cursor in $ancestors) {
        try {
            $attributes = [IO.File]::GetAttributes($cursor)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Label 不能通过链接或重解析点访问。"
            }
        } catch [IO.FileNotFoundException] { }
        catch [IO.DirectoryNotFoundException] { }
    }
    return $full
}

function Read-LineupJson([string]$Path, [int]$MaximumBytes = 65536) {
    $local = Resolve-LineupLocalPath $Path '配置文件'
    if (-not (Test-Path -LiteralPath $local -PathType Leaf)) { throw "缺少配置文件：$local" }
    if ((Get-Item -LiteralPath $local).Length -gt $MaximumBytes) { throw "配置文件过大：$local" }
    $value = [IO.File]::ReadAllText($local, [Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($null -eq $value -or $value -isnot [pscustomobject]) { throw "配置必须是 JSON 对象：$local" }
    if ((Get-LineupField $value 'schema') -isnot [int] -or $value.schema -ne 1) { throw "配置版本必须为 1：$local" }
    return $value
}

function Assert-LineupText($Value, [string]$Label, [int]$MaximumBytes = 512, [switch]$AllowEmpty) {
    if ($Value -isnot [string] -or (-not $AllowEmpty -and [string]::IsNullOrWhiteSpace($Value)) -or
        [Text.Encoding]::UTF8.GetByteCount($Value) -gt $MaximumBytes) { throw "$Label 未填写或过长。" }
}

function Test-LineupChoice($Options, [string]$Category, [string]$Id) {
    return @($Options.$Category | Where-Object { $_.id -ceq $Id }).Count -eq 1
}

function New-LineupPanelState([string]$Library, [string]$Program, [string]$OptionsFile) {
    $local = Resolve-LineupLocalPath $Library '采集目录'
    $exe = Resolve-LineupLocalPath $Program '采集程序'
    $optionsFileLocal = Resolve-LineupLocalPath $OptionsFile '选项文件'
    if (-not (Test-Path -LiteralPath $local -PathType Container)) { throw '采集目录不存在。' }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw '采集程序不存在。' }
    $config = Read-LineupJson (Join-Path $local 'config.json')
    Assert-LineupText (Get-LineupField $config 'source_id') '来源名称'
    foreach ($dimension in @(@('source_width', 7680), @('source_height', 4320))) {
        $value = Get-LineupField $config $dimension[0]
        if ($value -isnot [int] -or $value -lt 320 -or $value -gt $dimension[1]) { throw '全屏尺寸超出范围。' }
    }
    $options = Read-LineupJson $optionsFileLocal 1048576
    foreach ($category in @('maps','teams','grenades','methods','buttons','directions')) {
        $choices = @(Get-LineupField $options $category)
        if ($choices.Count -eq 0) { throw "缺少选项：$category" }
        $seen = @{}
        foreach ($choice in $choices) {
            Assert-LineupText (Get-LineupField $choice 'id') "$category 标识"
            Assert-LineupText (Get-LineupField $choice 'text') "$category 名称"
            if ($seen.ContainsKey($choice.id)) { throw "选项重复：$category" }
            $seen[$choice.id] = $true
        }
    }
    if (@($options.teams | Where-Object { $_.id -cnotin @('CT','T') }).Count -ne 0) { throw '阵营只能为 CT 或 T。' }
    $presets = @(Get-LineupField $options 'presets')
    if ($presets.Count -eq 0) { throw '缺少投掷预设。' }
    $presetKeys = @{}
    foreach ($preset in $presets) {
        foreach ($mapping in @(@('method','methods'), @('buttons','buttons'), @('direction','directions'))) {
            if (-not (Test-LineupChoice $options $mapping[1] (Get-LineupField $preset $mapping[0]))) { throw '投掷预设引用了未知选项。' }
        }
        $key = "$($preset.method)|$($preset.buttons)|$($preset.direction)"
        if ($presetKeys.ContainsKey($key)) { throw '投掷预设重复。' }
        $presetKeys[$key] = $true
        Assert-LineupText (Get-LineupField $preset 'throw_instructions') '投掷说明' 8192
        $action = Get-LineupField $preset 'throw_action'
        if ((Get-LineupField $action 'schema') -ne 1 -or (Get-LineupField $action 'type') -cne 'phases') { throw '投掷动作格式无效。' }
        $phases = @(Get-LineupField $action 'phases')
        if ($phases.Count -lt 2 -or $phases.Count -gt 16) { throw '投掷动作阶段数量无效。' }
        foreach ($phase in $phases) {
            if ($null -eq $phase.PSObject.Properties['duration_ms'] -or $null -ne $phase.duration_ms) {
                throw '采集预设的时长必须留空，不能猜测执行时序。'
            }
            if ((Get-LineupField $phase 'jump') -isnot [bool] -or
                $null -eq $phase.PSObject.Properties['buttons'] -or $null -eq $phase.PSObject.Properties['movement']) {
                throw '投掷动作阶段字段不完整。'
            }
        }
    }
    # 同步目标只保留为字符串，不在启动或 CheckOnly 中探测可达性。
    $profilePath = Resolve-LineupLocalPath (Join-Path $local 'profile.json') '采集设置'
    $profile = Get-LineupField $config 'default_profile'
    if (Test-Path -LiteralPath $profilePath -PathType Leaf) { $profile = Read-LineupJson $profilePath }
    foreach ($name in @('captures','logs')) { $null = Resolve-LineupLocalPath (Join-Path $local $name) '本地采集子目录' }
    return [pscustomobject]@{
        LibraryDirectory=$local; Executable=$exe; OptionsPath=$optionsFileLocal
        Config=$config; Options=$options; ProfilePath=$profilePath; Profile=$profile
        CaptureProcess=$null; SyncProcess=$null; CaptureOut=''; CaptureErr=''; SyncOut=''; SyncErr=''
    }
}

function ConvertTo-LineupCanonicalJson($Value) {
    if ($null -eq $Value) { return 'null' }
    if ($Value -is [pscustomobject]) {
        $fields = foreach ($property in ($Value.PSObject.Properties | Sort-Object Name)) {
            ($property.Name | ConvertTo-Json -Compress) + ':' + (ConvertTo-LineupCanonicalJson $property.Value)
        }
        return '{' + ($fields -join ',') + '}'
    }
    if ($Value -is [array]) {
        $items = foreach ($item in $Value) { ConvertTo-LineupCanonicalJson $item }
        return '[' + ($items -join ',') + ']'
    }
    return ($Value | ConvertTo-Json -Compress)
}

function Get-LineupSelection($State) {
    $profile = $State.Profile
    $selection = Get-LineupField $profile 'panel_selection'
    if ($null -eq $selection -and $null -ne (Get-LineupField $profile 'throw_action')) {
        $actionJson = ConvertTo-LineupCanonicalJson $profile.throw_action
        $selection = @($State.Options.presets | Where-Object {
            (ConvertTo-LineupCanonicalJson $_.throw_action) -ceq $actionJson
        } | Select-Object -First 1)
        if ($selection.Count -eq 0) { $selection = $null } else { $selection = $selection[0] }
    }
    return [pscustomobject]@{
        map=(Get-LineupField $profile 'map' ''); team=(Get-LineupField $profile 'team' '')
        grenade=(Get-LineupField $profile 'grenade' ''); name=(Get-LineupField $profile 'name' '')
        target=(Get-LineupField $profile 'target' '当前瞄点'); instructions=(Get-LineupField $profile 'instructions' '')
        method=(Get-LineupField $selection 'method' '')
        buttons=(Get-LineupField $selection 'buttons' ''); direction=(Get-LineupField $selection 'direction' 'forward')
    }
}

function New-LineupCaptureProfile($State, $Selection) {
    foreach ($mapping in @(@('map','maps'), @('team','teams'), @('grenade','grenades'), @('method','methods'), @('buttons','buttons'))) {
        if (-not (Test-LineupChoice $State.Options $mapping[1] (Get-LineupField $Selection $mapping[0] ''))) {
            throw '请完整选择地图、阵营、道具、投掷方式和按键。'
        }
    }
    $direction = if ($Selection.method -cin @('stationary','jump')) { 'forward' } else { Get-LineupField $Selection 'direction' '' }
    $presets = @($State.Options.presets | Where-Object {
        $_.method -ceq $Selection.method -and $_.buttons -ceq $Selection.buttons -and $_.direction -ceq $direction
    })
    if ($presets.Count -ne 1) { throw '请选择完整的投掷方向。' }
    $name = Get-LineupField $Selection 'name' ''
    $target = Get-LineupField $Selection 'target' '当前瞄点'
    if ([string]::IsNullOrWhiteSpace($target)) { $target = '当前瞄点' }
    $instructions = Get-LineupField $Selection 'instructions' ''
    Assert-LineupText $name '名称' 8192 -AllowEmpty
    Assert-LineupText $target '目标' 8192
    Assert-LineupText $instructions '靠位说明' 8192 -AllowEmpty
    return [pscustomobject][ordered]@{
        schema=1; source_id=$State.Config.source_id
        source_width=$State.Config.source_width; source_height=$State.Config.source_height
        map=$Selection.map; team=$Selection.team; grenade=$Selection.grenade; name=$name; target=$target; instructions=$instructions
        throw_instructions=$presets[0].throw_instructions; throw_action=$presets[0].throw_action
        panel_selection=[ordered]@{method=$Selection.method; buttons=$Selection.buttons; direction=$direction}
    }
}

function Test-LineupProcess($Process) {
    return $null -ne $Process -and -not $Process.HasExited
}

function Save-LineupCaptureProfile($State, $Selection) {
    if (Test-LineupProcess $State.CaptureProcess) { throw '请先停止采集，再修改设置。' }
    $profile = New-LineupCaptureProfile $State $Selection
    $json = $profile | ConvertTo-Json -Depth 20
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + "`n")
    if ($bytes.Length -gt 65536) { throw '采集设置过大。' }
    $destination = Resolve-LineupLocalPath $State.ProfilePath '采集设置'
    $temporary = $destination + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        if ([IO.File]::Exists($destination)) { [IO.File]::Replace($temporary, $destination, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $destination) }
        $State.Profile = $profile
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function ConvertTo-LineupArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function New-LineupLogPaths($State, [string]$Prefix) {
    $directory = Resolve-LineupLocalPath (Join-Path $State.LibraryDirectory 'logs') '日志目录'
    $null = [IO.Directory]::CreateDirectory($directory)
    $id = [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N')
    return @((Join-Path $directory "$Prefix-$id-out.log"), (Join-Path $directory "$Prefix-$id-err.log"))
}

function Start-LineupCapture($State, $Selection) {
    if (Test-LineupProcess $State.CaptureProcess) { throw '采集已经开始。' }
    Save-LineupCaptureProfile $State $Selection
    $exe = Resolve-LineupLocalPath $State.Executable '采集程序'
    $logs = New-LineupLogPaths $State 'capture'
    $arguments = '--local-library ' + (ConvertTo-LineupArgument $State.LibraryDirectory)
    $State.CaptureProcess = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $State.LibraryDirectory `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $logs[0] -RedirectStandardError $logs[1]
    $null = $State.CaptureProcess.Handle
    $State.CaptureOut = $logs[0]; $State.CaptureErr = $logs[1]
}

function Stop-LineupCapture($State) {
    # 只持有并结束本面板创建的进程对象，不按名称或共享 PID 搜索其它 Host。
    if ($null -ne $State.CaptureProcess) {
        if (-not $State.CaptureProcess.HasExited) {
            $State.CaptureProcess.Kill()
            if (-not $State.CaptureProcess.WaitForExit(3000)) { throw '采集进程尚未退出，请稍后重试。' }
        }
        $State.CaptureProcess.Dispose()
        $State.CaptureProcess = $null
    }
}

function Get-LineupCaptureSummary($State) {
    $directory = Resolve-LineupLocalPath (Join-Path $State.LibraryDirectory 'captures') '截图目录'
    $captures = @()
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $captures = @(Get-ChildItem -LiteralPath $directory -Directory | Where-Object {
            $_.Name -match '^[A-Za-z0-9_-]{1,96}$' -and
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0
        } | Where-Object {
            $path = $_.FullName
            @('capture.json','full.png','roi.png','overview.png' | Where-Object {
                $file = Join-Path $path $_
                try {
                    $attributes = [IO.File]::GetAttributes($file)
                    ($attributes -band ([IO.FileAttributes]::ReparsePoint -bor [IO.FileAttributes]::Directory)) -eq 0
                } catch [IO.FileNotFoundException] { $false }
                catch [IO.DirectoryNotFoundException] { $false }
            }).Count -eq 4
        } | Sort-Object LastWriteTime -Descending)
    }
    return [pscustomobject]@{Count=$captures.Count; Latest=$(if ($captures.Count) { $captures[0].Name } else { '' })}
}

function Start-LineupSync($State) {
    if (Test-LineupProcess $State.SyncProcess) { throw '发送正在进行。' }
    $scriptPath = Resolve-LineupLocalPath (Get-LineupField $State.Config 'sync_script' '') '发送脚本'
    if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf) -or [IO.Path]::GetExtension($scriptPath) -ine '.ps1') { throw '本地发送脚本不存在。' }
    $inbox = Get-LineupField $State.Config 'sync_inbox' ''
    Assert-LineupText $inbox '辅机收件箱路径' 4096
    $logs = New-LineupLogPaths $State 'sync'
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File ' + (ConvertTo-LineupArgument $scriptPath) +
        ' -LibraryDirectory ' + (ConvertTo-LineupArgument $State.LibraryDirectory) + ' -InboxDirectory ' + (ConvertTo-LineupArgument $inbox)
    $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $State.SyncProcess = Start-Process -FilePath $shell -ArgumentList $arguments -WorkingDirectory $State.LibraryDirectory `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $logs[0] -RedirectStandardError $logs[1]
    $null = $State.SyncProcess.Handle
    $State.SyncOut = $logs[0]; $State.SyncErr = $logs[1]
}

function Read-LineupLogTail([string]$Path) {
    if ($Path) {
        $local = Resolve-LineupLocalPath $Path '日志文件'
        if (Test-Path -LiteralPath $local -PathType Leaf) {
            return ('' + (Get-Content -LiteralPath $local -Encoding UTF8 -Tail 1)).Trim()
        }
    }
    return ''
}

function Get-LineupCaptureLog($State) {
    $logs = foreach ($path in @($State.CaptureOut, $State.CaptureErr)) {
        if ($path) {
            $local = Resolve-LineupLocalPath $path '采集日志'
            if (Test-Path -LiteralPath $local -PathType Leaf) {
                [pscustomobject]@{Path=$local; Updated=[IO.File]::GetLastWriteTimeUtc($local)}
            }
        }
    }
    foreach ($log in ($logs | Sort-Object Updated -Descending)) {
        $line = Read-LineupLogTail $log.Path
        if ($line) { return $line }
    }
    return ''
}

function Show-LineupCapturePanel($State) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    $form = [Windows.Forms.Form]::new()
    $form.Text = 'Xen 道具采集'; $form.ClientSize = [Drawing.Size]::new(700, 680)
    $form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
    $form.Font = [Drawing.Font]::new('Microsoft YaHei UI', 10)
    $hint = [Windows.Forms.Label]::new()
    $hint.SetBounds(20, 15, 660, 65)
    $hint.Text = "先选择设置，再开始采集。切回游戏瞄准后按 F7。`n全屏与中心 320 参考保存在本机，完成后再发送到辅机。`n来源：$($State.Config.source_id)  $($State.Config.source_width) × $($State.Config.source_height)"
    $form.Controls.Add($hint)
    $controls = @{}; $restored = Get-LineupSelection $State
    $rows = @(@('map','maps','地图'), @('team','teams','阵营'), @('grenade','grenades','道具'),
        @('method','methods','投掷方式'), @('buttons','buttons','投掷按键'), @('direction','directions','移动方向'))
    for ($index = 0; $index -lt $rows.Count; $index++) {
        $row = $rows[$index]; $top = 92 + 42 * $index
        $label = [Windows.Forms.Label]::new(); $label.Text = $row[2]; $label.SetBounds(20, $top + 4, 110, 28)
        $combo = [Windows.Forms.ComboBox]::new(); $combo.SetBounds(140, $top, 530, 30)
        $combo.DropDownStyle = 'DropDownList'; $combo.DisplayMember = 'text'
        foreach ($choice in $State.Options.($row[1])) { $null = $combo.Items.Add($choice) }
        $combo.SelectedIndex = -1
        for ($item = 0; $item -lt $combo.Items.Count; $item++) {
            if ($combo.Items[$item].id -ceq $restored.($row[0])) { $combo.SelectedIndex = $item; break }
        }
        $controls[$row[0]] = $combo; $form.Controls.Add($label); $form.Controls.Add($combo)
    }
    foreach ($row in @(@('name','名称（可选）', 350), @('instructions','靠位说明', 392))) {
        $label = [Windows.Forms.Label]::new(); $label.Text = $row[1]; $label.SetBounds(20, $row[2] + 4, 110, 28)
        $input = [Windows.Forms.TextBox]::new(); $input.SetBounds(140, $row[2], 530, 30)
        $input.MaxLength = 2000; $input.Text = $restored.($row[0]); $controls[$row[0]] = $input
        $form.Controls.Add($label); $form.Controls.Add($input)
    }
    $start = [Windows.Forms.Button]::new(); $start.Text = '开始 F7 采集'; $start.SetBounds(20, 441, 154, 36)
    $stop = [Windows.Forms.Button]::new(); $stop.Text = '停止采集'; $stop.SetBounds(184, 441, 130, 36)
    $open = [Windows.Forms.Button]::new(); $open.Text = '打开采集目录'; $open.SetBounds(324, 441, 166, 36)
    $send = [Windows.Forms.Button]::new(); $send.Text = '发送到辅机'; $send.SetBounds(500, 441, 170, 36)
    foreach ($button in @($start,$stop,$open,$send)) { $form.Controls.Add($button) }
    $status = [Windows.Forms.TextBox]::new(); $status.SetBounds(20, 490, 650, 66)
    $status.Multiline = $true; $status.ReadOnly = $true; $status.ScrollBars = 'Vertical'
    $status.Text = '尚未开始。开始时保存设置，停止后可以修改。'
    $saved = [Windows.Forms.Label]::new(); $saved.SetBounds(20, 567, 650, 42)
    $syncStatus = [Windows.Forms.Label]::new(); $syncStatus.SetBounds(20, 616, 650, 56); $syncStatus.Text = '发送由你手动触发，采集无需辅机在线。'
    foreach ($label in @($status,$saved,$syncStatus)) { $form.Controls.Add($label) }
    $readSelection = {
        $value = @{}
        foreach ($field in @('map','team','grenade','method','buttons','direction')) {
            $value[$field] = if ($controls[$field].SelectedIndex -ge 0) { $controls[$field].SelectedItem.id } else { '' }
        }
        $value['name'] = $controls.name.Text; $value['target'] = $restored.target; $value['instructions'] = $controls.instructions.Text
        return [pscustomobject]$value
    }
    $refresh = {
        $running = Test-LineupProcess $State.CaptureProcess
        foreach ($control in $controls.Values) { $control.Enabled = -not $running }
        $method = if ($controls.method.SelectedIndex -ge 0) { $controls.method.SelectedItem.id } else { '' }
        if ($method -cin @('stationary','jump')) {
            for ($item = 0; $item -lt $controls.direction.Items.Count; $item++) {
                if ($controls.direction.Items[$item].id -ceq 'forward') { $controls.direction.SelectedIndex = $item; break }
            }
        }
        $controls.direction.Enabled = -not $running -and $method -cnotin @('','stationary','jump')
        $start.Enabled = -not $running; $stop.Enabled = $running
        $send.Enabled = -not (Test-LineupProcess $State.SyncProcess)
        $summary = Get-LineupCaptureSummary $State
        $saved.Text = "本地已保存：$($summary.Count) 张`n最近一次：$(if ($summary.Latest) { $summary.Latest } else { '暂无' })"
        if ($running) {
            $line = Get-LineupCaptureLog $State
            if ($line) { $status.Text = "采集中（F7）。最新日志：`r`n$line" }
        }
        if ($null -ne $State.CaptureProcess -and $State.CaptureProcess.HasExited) {
            $status.Text = "采集已退出（$($State.CaptureProcess.ExitCode)）。`r`n" + (Get-LineupCaptureLog $State)
            Stop-LineupCapture $State
        }
        if ($null -ne $State.SyncProcess -and $State.SyncProcess.HasExited) {
            $result = Read-LineupLogTail $State.SyncOut
            if ($State.SyncProcess.ExitCode -ne 0) { $result += ' ' + (Read-LineupLogTail $State.SyncErr) }
            $syncStatus.Text = "发送结束（$($State.SyncProcess.ExitCode)）：$result"
            $State.SyncProcess.Dispose(); $State.SyncProcess = $null
        }
    }
    $controls.method.Add_SelectedIndexChanged({ & $refresh })
    $start.Add_Click({
        try { Start-LineupCapture $State (& $readSelection); $status.Text = '采集已开始。切回游戏，瞄准后按 F7；修改设置前先停止采集。'; & $refresh }
        catch { $status.Text = $_.Exception.Message }
    })
    $stop.Add_Click({ try { Stop-LineupCapture $State; $status.Text = '采集已停止，设置可以修改。'; & $refresh } catch { $status.Text = $_.Exception.Message } })
    $open.Add_Click({
        try {
            $directory = Resolve-LineupLocalPath (Join-Path $State.LibraryDirectory 'captures') '截图目录'
            $null = [IO.Directory]::CreateDirectory($directory)
            Start-Process -FilePath (Join-Path $env:SystemRoot 'explorer.exe') -ArgumentList (ConvertTo-LineupArgument $directory)
        } catch { $status.Text = $_.Exception.Message }
    })
    $send.Add_Click({ try { Start-LineupSync $State; $syncStatus.Text = '正在发送已有截图，请稍候……'; & $refresh } catch { $syncStatus.Text = $_.Exception.Message } })
    $timer = [Windows.Forms.Timer]::new(); $timer.Interval = 1000
    $timer.Add_Tick({ try { & $refresh } catch { $status.Text = $_.Exception.Message } })
    $form.Add_FormClosing({ param($sender, $event)
        try { Stop-LineupCapture $State; $timer.Stop() }
        catch { $event.Cancel = $true; $status.Text = '无法结束本面板的采集进程：' + $_.Exception.Message }
    })
    try { & $refresh; $timer.Start(); [Windows.Forms.Application]::Run($form) }
    finally { $timer.Dispose(); $form.Dispose(); Stop-LineupCapture $State }
}

try {
    $script:LineupPanelState = New-LineupPanelState $LibraryDirectory $Executable $OptionsPath
    if ($CheckOnly) {
        [pscustomobject]@{valid=$true; library=$script:LineupPanelState.LibraryDirectory; has_profile=($null -ne $script:LineupPanelState.Profile)} | ConvertTo-Json -Compress
        return
    }
    Show-LineupCapturePanel $script:LineupPanelState
} catch {
    if ($CheckOnly) { throw }
    Add-Type -AssemblyName System.Windows.Forms
    $null = [Windows.Forms.MessageBox]::Show($_.Exception.Message, '道具采集无法启动', 'OK', 'Error')
    exit 1
}
