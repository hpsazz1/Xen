param(
    [string]$ObsRoot = 'D:\SteamLibrary\steamapps\common\OBS Studio Beta',
    [string]$SdkRoot = 'E:\Xen-deps\obs-studio-32.1.0-rc4-sdk',
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BuildDirectory = '',
    [string]$Generator = 'Visual Studio 18 2026',
    [switch]$Install,
    [switch]$DesktopShortcut
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$ObsRoot = (Resolve-Path -LiteralPath $ObsRoot).Path
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $RepositoryRoot 'build/nvidia/obs-recorder' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$obsBinary = Join-Path $ObsRoot 'bin/64bit/obs64.exe'
if ((Get-Item -LiteralPath $obsBinary).VersionInfo.FileVersion -notmatch '^32\.1\.') {
    throw '当前工具固定OBS32.1 ABI，其他版本需重新核对官方头文件。'
}
$expectedCommit = 'b7bc979a659a40775904a489eeb0ed9e871e066b'
$commit = (& git -C (Join-Path $SdkRoot 'source') rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -cne $expectedCommit) { throw 'SDK来源与固定OBS版本不符。' }
foreach ($helper in @('obs-ffmpeg-mux.exe','obs-nvenc-test.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ObsRoot "bin/64bit/$helper"))) { throw "缺少现有OBS辅助程序：$helper" }
}
if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot 'lib/obs.lib'))) {
    throw '缺少现有OBS导入库；请先按build_obs_grenade_overlay.ps1的固定SDK流程生成。'
}
$jsonInclude = Join-Path $RepositoryRoot 'build/nvidia/_deps/nlohmann_json-src/include'
& cmake -S (Join-Path $RepositoryRoot 'tools/recorder') -B $BuildDirectory -G $Generator -A x64 `
    "-DOBS_ROOT=$ObsRoot" "-DOBS_SDK_ROOT=$SdkRoot" "-DXEN_JSON_INCLUDE=$jsonInclude"
if ($LASTEXITCODE -ne 0) { throw '独立录制工具配置失败。' }
& cmake --build $BuildDirectory --config Release --target XenRecorder recorder_marker_tests recorder_sync_tests
if ($LASTEXITCODE -ne 0) { throw '独立录制工具构建失败。' }
& ctest --test-dir $BuildDirectory -C Release -R '^recorder_(marker|sync)_tests$' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw '独立录制专项失败。' }
$package = Join-Path $BuildDirectory 'package'
& cmake --install $BuildDirectory --config Release --prefix $package
if ($LASTEXITCODE -ne 0) { throw '独立录制工具打包失败。' }
$identity = [ordered]@{
    sdk_commit=$commit; obs_path=$obsBinary; obs_version=(Get-Item -LiteralPath $obsBinary).VersionInfo.FileVersion
    xen_commit=(& git -C $RepositoryRoot rev-parse HEAD).Trim()
    xen_dirty=[bool](& git -C $RepositoryRoot status --porcelain --untracked-files=normal)
    built_at=[datetime]::UtcNow.ToString('o'); package=$package
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $package 'build-identity.json') -Encoding utf8
if ($Install -or $DesktopShortcut) {
    $destination = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'cache/tools/recorder'))
    $installedExe = Join-Path $destination 'XenRecorder.exe'
    $running = @(Get-CimInstance Win32_Process -Filter "Name='XenRecorder.exe'" |
        Where-Object { $_.ExecutablePath -ieq $installedExe })
    if ($running.Count) { throw '独立录制程序仍在运行，请先停止保存并退出后再更新；未停止任何进程。' }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    foreach ($name in @('XenRecorder.exe','obs-ffmpeg-mux.exe','obs-nvenc-test.exe','README.md','LICENSE.GPL-2.0','build-identity.json')) {
        $sourceFile = Join-Path $package $name
        $destinationFile = Join-Path $destination $name
        if (Test-Path -LiteralPath $destinationFile) {
            if ((Get-FileHash -LiteralPath $sourceFile).Hash -eq (Get-FileHash -LiteralPath $destinationFile).Hash) { continue }
            Copy-Item -LiteralPath $destinationFile -Destination ($destinationFile + '.previous') -Force
        }
        Copy-Item -LiteralPath $sourceFile -Destination $destinationFile -Force
        if ((Get-FileHash -LiteralPath $sourceFile).Hash -ne (Get-FileHash -LiteralPath $destinationFile).Hash) {
            throw "独立工具安装回读失败：$name"
        }
    }
    Write-Host "独立录制工具已安装并回读：$installedExe"
    if ($DesktopShortcut) {
        $desktop = [Environment]::GetFolderPath('DesktopDirectory')
        $shortcutPath = Join-Path $desktop 'Xen 独立录制.lnk'
        $shortcutShell = New-Object -ComObject WScript.Shell
        $shortcut = $shortcutShell.CreateShortcut($shortcutPath)
        if ((Test-Path -LiteralPath $shortcutPath) -and $shortcut.TargetPath -ine $installedExe) {
            throw '桌面已有同名其他程序快捷方式，未覆盖。'
        }
        $shortcut.TargetPath = $installedExe
        $shortcut.WorkingDirectory = $destination
        $shortcut.Arguments = '--fps 240 --no-audio'
        $shortcut.Description = '全画面240FPS，不采集系统声音，首尾自动SYNC'
        $shortcut.IconLocation = "$installedExe,0"
        $shortcut.Save()
        $readback = $shortcutShell.CreateShortcut($shortcutPath)
        if ($readback.TargetPath -ine $installedExe -or $readback.Arguments -cne '--fps 240 --no-audio') {
            throw '桌面快捷方式回读不一致。'
        }
        Write-Host "桌面快捷方式已创建并回读：$shortcutPath"
    }
}
Write-Host "独立录制工具：$(Join-Path $package 'XenRecorder.exe')"
Write-Host '仅使用现有OBS运行库，未修改OBS设置或Xen/辅机程序。'
