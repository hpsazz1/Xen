param(
    [Parameter(Mandatory = $true)][string]$ObsRoot,
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$BuildDirectory = '',
    [string]$Generator = 'Visual Studio 18 2026',
    [switch]$Install,
    [string]$AuxiliaryUrl = 'http://192.168.3.20:5015',
    [string]$CapturePython = '',
    [string]$CaptureRoot = '',
    [string]$CaptureDestination = '\\192.168.3.20\XenLab$\releases\Xen-current\cache\grenade'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$ObsRoot = (Resolve-Path -LiteralPath $ObsRoot).Path
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $RepositoryRoot 'build/nvidia/obs-grenade-overlay' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$obsBinary = Join-Path $ObsRoot 'bin/64bit/obs64.exe'
if ((Get-Item -LiteralPath $obsBinary).VersionInfo.FileVersion -notmatch '^32\.1\.') {
    throw '当前构建固定 OBS 32.1 ABI，其他版本需先核对对应官方头文件。'
}
$source = Join-Path $SdkRoot 'source'
$expectedCommit = 'b7bc979a659a40775904a489eeb0ed9e871e066b'
if (-not (Test-Path -LiteralPath $source)) {
    & git clone --depth 1 --branch 32.1.0-rc4 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git $source
    if ($LASTEXITCODE -ne 0) { throw 'OBS 官方头文件获取失败。' }
    & git -C $source sparse-checkout set libobs frontend/api
    if ($LASTEXITCODE -ne 0) { throw 'OBS 头文件稀疏检出失败。' }
}
$commit = (& git -C $source rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -cne $expectedCommit) { throw 'OBS 头文件来源与固定版本不符。' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $visualStudio) { throw '未找到 MSVC x64 工具链。' }
$msvc = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Tools/MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$compilerTools = Join-Path $msvc.FullName 'bin/Hostx64/x64'
$dumpbin = Join-Path $compilerTools 'dumpbin.exe'
$lib = Join-Path $compilerTools 'lib.exe'
$libraries = Join-Path $SdkRoot 'lib'
New-Item -ItemType Directory -Path $libraries -Force | Out-Null
$bindings = @()
foreach ($name in @('obs', 'obs-frontend-api')) {
    $dll = Join-Path $ObsRoot "bin/64bit/$name.dll"
    $exports = @(& $dumpbin /nologo /exports $dll)
    if ($LASTEXITCODE -ne 0) { throw "无法读取 $name 导出表。" }
    $symbols = @($exports | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] }
    })
    if ($symbols.Count -lt 10) { throw "$name 导出表解析失败。" }
    $definition = Join-Path $libraries "$name.def"
    @("LIBRARY $name.dll", 'EXPORTS') + $symbols | Set-Content -LiteralPath $definition -Encoding ascii
    & $lib /nologo /machine:x64 "/def:$definition" "/out:$(Join-Path $libraries "$name.lib")"
    if ($LASTEXITCODE -ne 0) { throw "$name 导入库生成失败。" }
    $bindings += [ordered]@{ name=$name; dll_sha256=(Get-FileHash -LiteralPath $dll).Hash; exports=$symbols.Count }
}
$jsonInclude = Join-Path $RepositoryRoot 'build/nvidia/_deps/nlohmann_json-src/include'
$project = Join-Path $RepositoryRoot 'tools/obs_grenade_overlay'
& cmake -S $project -B $BuildDirectory -G $Generator -A x64 "-DOBS_SDK_ROOT=$SdkRoot" "-DXEN_JSON_INCLUDE=$jsonInclude"
if ($LASTEXITCODE -ne 0) { throw 'OBS 插件配置失败。' }
& cmake --build $BuildDirectory --config Release --target xen-grenade-overlay obs_grenade_overlay_tests
if ($LASTEXITCODE -ne 0) { throw 'OBS 插件构建失败。' }
& ctest --test-dir $BuildDirectory -C Release -R '^obs_grenade_overlay_tests$' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'OBS 插件状态专项失败。' }
$package = Join-Path $BuildDirectory 'package'
& cmake --install $BuildDirectory --config Release --prefix $package
if ($LASTEXITCODE -ne 0) { throw 'OBS 插件打包失败。' }
$identity = [ordered]@{
    sdk_commit=$commit; obs_path=$obsBinary; obs_version=(Get-Item -LiteralPath $obsBinary).VersionInfo.FileVersion
    bindings=$bindings; xen_commit=(& git -C $RepositoryRoot rev-parse HEAD).Trim()
    xen_dirty=[bool](& git -C $RepositoryRoot status --porcelain --untracked-files=normal)
    built_at=[datetime]::UtcNow.ToString('o'); package=$package
}
$identity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'build-identity.json') -Encoding utf8
if ($Install) {
    $running = @(Get-CimInstance Win32_Process -Filter "Name='obs64.exe'" |
        Where-Object { $_.ExecutablePath -ieq $obsBinary })
    if ($running.Count) { throw '插件已构建，但目标 OBS 仍在运行；关闭后再安装，未停止任何进程。' }
    $relativeFiles = @('obs-plugins/64bit/xen-grenade-overlay.dll',
        'data/obs-plugins/xen-grenade-overlay/reference.html',
        'data/obs-plugins/xen-grenade-overlay/README.md',
        'data/obs-plugins/xen-grenade-overlay/LICENSE.GPL-2.0')
    $relativeFiles += @('worker.py','capture_workflow.py','reference_assets.py','reference_tool.py',
        'screen_capture.py','README.md','standing/mirage-t-spawns.png') |
        ForEach-Object { "data/obs-plugins/xen-grenade-overlay/capture/$_" }
    foreach ($relative in $relativeFiles) {
        $destination = Join-Path $ObsRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        if (Test-Path -LiteralPath $destination) {
            if ((Get-FileHash -LiteralPath $destination).Hash -eq (Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash) { continue }
            Copy-Item -LiteralPath $destination -Destination ($destination + '.previous') -Force
        }
        Copy-Item -LiteralPath (Join-Path $package $relative) -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash) { throw '插件安装回读不一致。' }
    }
    $configRoot = Join-Path $env:APPDATA 'obs-studio/plugin_config/xen-grenade-overlay'
    $settings = Join-Path $configRoot 'settings.json'
    New-Item -ItemType Directory -Path $configRoot -Force | Out-Null
    $configuration = if (Test-Path -LiteralPath $settings) { Get-Content -Raw -LiteralPath $settings | ConvertFrom-Json -AsHashtable }
        else { @{enabled=$true; auxiliary_url=$AuxiliaryUrl} }
    if (-not $configuration.ContainsKey('capture_python') -or -not $configuration.capture_python) {
        if (-not $CapturePython) {
            $CapturePython = (& py -3 -c 'import sys; print(sys.executable)').Trim()
            if ($LASTEXITCODE -ne 0) { throw '未找到主机现有Python 3，请指定CapturePython。' }
        }
        $configuration.capture_python = (Resolve-Path -LiteralPath $CapturePython).Path
    }
    if (-not $configuration.ContainsKey('capture_root') -or -not $configuration.capture_root) {
        if (-not $CaptureRoot) { $CaptureRoot = Join-Path $RepositoryRoot 'cache/releases/Xen-current/cache/grenade' }
        $configuration.capture_root = [IO.Path]::GetFullPath($CaptureRoot)
    }
    if (-not $configuration.ContainsKey('capture_destination') -or -not $configuration.capture_destination) { $configuration.capture_destination = $CaptureDestination }
    $configuration | ConvertTo-Json | Set-Content -LiteralPath $settings -Encoding utf8
    Write-Host "OBS 插件已安装并回读：$ObsRoot"
}
Write-Host "OBS 插件产物：$package"
