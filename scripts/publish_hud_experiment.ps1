param(
    [Parameter(Mandatory=$true)][string]$BasePackagePath,
    [Parameter(Mandatory=$true)][string]$SettingsPackagePath,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'path_safety.psm1') -Force
$repo=Split-Path -Parent $PSScriptRoot
$settings=(Resolve-Path -LiteralPath $SettingsPackagePath).ProviderPath
Assert-XenNoReparsePathChain $settings '原配置包' -RequireExistingLeaf
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw '实验包必须是新目录，不能覆盖已有入口。' }
$configPath=Join-Path $settings 'config.ini'
$sourceConfig=Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
$sourceHash=(Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
if ($sourceConfig -notmatch '(?m)^\[auto_stop\]\s*$' -or $sourceConfig -match '(?m)^\s*experimental_hud_model\s*=') {
    throw '基准配置必须包含auto_stop且尚未选择实验算法。'
}
# 此任务只改变算法选择，保留用户最新参数、模型选择与压枪文件。
$config=$sourceConfig -replace '(?m)^(\[auto_stop\]\s*)$', ('$1'+"`r`nexperimental_hud_model = true")
$staging=Join-Path $repo ('cache/hud-config-'+[guid]::NewGuid().ToString('N')+'.ini')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $staging) | Out-Null
[IO.File]::WriteAllText($staging,$config,[Text.UTF8Encoding]::new($false))
$arguments=@{BasePackagePath=$BasePackagePath; BuildDirectory=$BuildDirectory; Runtime='nvidia';
    OutputDirectory=$output; ConfigPath=$staging}
$workspace=Join-Path $settings 'cache/model-workspace/settings.json'
if (Test-Path -LiteralPath $workspace) { $arguments.WorkspaceSettingsPath=$workspace }
& (Join-Path $PSScriptRoot 'publish_worker_update.ps1') @arguments
$manifestPath=Join-Path $output 'manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
function Add-Payload([string]$Source,[string]$Relative) {
    Assert-XenNoReparsePathChain $Source '实验附加载荷' -RequireExistingLeaf
    $destination=[IO.Path]::GetFullPath((Join-Path $output $Relative))
    if (!$destination.StartsWith($output.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw '附加载荷越界' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination
    $hash=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant()) { throw '附加载荷复制不一致' }
    $manifest.files=@($manifest.files | Where-Object {$_.path -ine $Relative})+@([pscustomobject]@{
        path=$Relative;runtime='';size=[long](Get-Item -LiteralPath $destination).Length;sha256=$hash;source=$Source})
}
Add-Payload (Join-Path $PSScriptRoot 'invoke_hud_stop_acceptance.ps1') 'tools/acceptance/invoke_hud_stop_acceptance.ps1'
Add-Payload (Join-Path $repo 'assets/reference_assessment/LICENSE.cs-match-hud.txt') 'licenses/cs-match-hud-MIT.txt'
Add-Payload (Join-Path $repo 'assets/reference_assessment/HUD-EXPERIMENT.md') 'tools/acceptance/HUD-EXPERIMENT.md'
$profile='cache/recoil/profiles'
if ($sourceConfig -notmatch '(?m)^profile_directory\s*=\s*cache/recoil/profiles\s*$') {
    throw '当前打包只支持包内相对压枪目录，不能静默改写外部用户路径。'
}
$profilePath=Join-Path $settings $profile
Assert-XenNoReparsePathChain $profilePath '原活动曲线目录' -RequireExistingLeaf
foreach ($file in Get-ChildItem -LiteralPath $profilePath -File) {
    if ($file.Extension -ieq '.json') { Add-Payload $file.FullName ($profile+'/'+$file.Name) }
}
Add-Payload (Join-Path $settings 'cache/recoil/weapon-timing.json') 'cache/recoil/weapon-timing.json'
# 同一辅机与依赖闭包复用现有TensorRT缓存，避免比较首次启动额外重建引擎。
$engineCache=Join-Path $settings 'cache/tensorrt'
if (Test-Path -LiteralPath $engineCache) {
    Assert-XenNoReparsePathChain $engineCache '原推理缓存' -RequireExistingLeaf
    foreach ($file in Get-ChildItem -LiteralPath $engineCache -File) {
        if ($file.Extension -in @('.engine','.timing')) { Add-Payload $file.FullName ('cache/tensorrt/'+$file.Name) }
    }
}
if ((Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne $sourceHash) { throw '原配置在打包期间变化，请重新准备新目录。' }
$manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Output $output
Write-Output '下一步使用transfer_release_bundle.ps1完整校验并传输新目录；此脚本没有启动任何程序。'
