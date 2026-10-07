param(
    [Parameter(Mandatory = $true)][string]$LibraryDirectory,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OptionsPath,
    [Parameter(Mandatory = $true)][string]$SourceId,
    [Parameter(Mandatory = $true)][ValidateRange(320,7680)][int]$SourceWidth,
    [Parameter(Mandatory = $true)][ValidateRange(320,4320)][int]$SourceHeight,
    [string]$SyncInbox = '',
    [string]$ShortcutPath = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:PSModulePath = [IO.Path]::Combine($PSHOME,'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
if (-not $ShortcutPath) { $ShortcutPath = [IO.Path]::Combine([Environment]::GetFolderPath('Desktop'),'道具采集.lnk') }
$library = [IO.Path]::GetFullPath($LibraryDirectory).TrimEnd('\')
$marker = 'Xen 主机离线道具采集 / GRENADE-CAPTURE-OFFLINE-20261007-001'
$shell = New-Object -ComObject WScript.Shell
$shortcut = [IO.Path]::GetFullPath($ShortcutPath)
$panel = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'lineup_capture_panel.ps1'))
$transfer = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'sync_lineup_captures.ps1'))
$binary = [IO.Path]::GetFullPath($Executable)
$options = [IO.Path]::GetFullPath($OptionsPath)
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if ([IO.Path]::GetExtension($shortcut) -ine '.lnk') { throw '入口必须为 .lnk 快捷方式。' }
foreach ($path in @($library,$panel,$transfer,$binary,$options,$shortcut)) {
    $driveRoot = [IO.Path]::GetPathRoot($path)
    if ($path.StartsWith('\\') -or $path.Contains('"') -or -not $driveRoot -or
        [IO.DriveInfo]::new($driveRoot).DriveType -notin @('Fixed','Removable','Ram')) { throw '采集入口和程序必须位于主机本地。' }
    $existing = $driveRoot
    foreach ($component in $path.Substring($driveRoot.Length).Split('\',[StringSplitOptions]::RemoveEmptyEntries)) {
        $existing = Join-Path $existing $component
        if (-not (Test-Path -LiteralPath $existing)) { break }
        if (((Get-Item -LiteralPath $existing -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "本地入口不使用重解析路径：$existing" }
    }
}
if ($library.Length -lt 4) { throw '请指定独立采集目录，不使用磁盘根目录。' }
foreach ($path in @($panel,$transfer,$binary,$options,$powershell)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "本地入口依赖缺失：$path" }
}
if (Test-Path -LiteralPath $shortcut) {
    $previous = $shell.CreateShortcut($shortcut)
    if ($previous.Description -cne $marker) { throw "同名快捷方式不属于本任务，保留原文件：$shortcut" }
}
if ([string]::IsNullOrWhiteSpace($SourceId)) { throw '需要明确已有 NDI 源身份，用于后续辅机核对。' }
[void][IO.Directory]::CreateDirectory($library)
$configPath = Join-Path $library 'config.json'
$config = [ordered]@{schema=1;source_id=$SourceId;source_width=$SourceWidth;source_height=$SourceHeight;
    sync_script=$transfer;sync_inbox=$SyncInbox}
if (Test-Path -LiteralPath $configPath) {
    $old = [IO.File]::ReadAllText($configPath) | ConvertFrom-Json
    foreach ($key in @('schema','source_id','source_width','source_height','sync_script','sync_inbox')) {
        if ($old.$key -cne $config[$key]) { throw '本地采集配置已存在且不同，保留用户设置；请使用独立目录。' }
    }
} else {
    [IO.File]::WriteAllText($configPath,($config | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
}
# 只创建并回读入口；不调用快捷方式或启动面板/截图/设备。
$arguments = '-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $panel +
    '" -LibraryDirectory "' + $library + '" -Executable "' + $binary + '" -OptionsPath "' + $options + '"'
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $powershell
$link.Arguments = $arguments
$link.WorkingDirectory = $library
$link.Description = $marker
$link.IconLocation = $binary + ',0'
$link.Save()
$readBack = $shell.CreateShortcut($shortcut)
if ($readBack.TargetPath -ine $powershell -or $readBack.Arguments -cne $arguments -or $readBack.Description -cne $marker) {
    throw '快捷方式回读不一致。'
}
Write-Output "已创建主机本地入口：$shortcut；采集库：$library。未启动任何程序。"
