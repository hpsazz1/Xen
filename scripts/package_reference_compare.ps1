param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputRoot) { throw '独立包目录必须不存在，拒绝覆盖。' }
$status = & git -C $repo status --porcelain
if ($LASTEXITCODE -ne 0 -or $status) { throw '必须先提交本任务源码，再从干净工作树打包。' }
$commit = (& git -C $repo rev-parse HEAD).Trim()
$identity = Get-Content -LiteralPath (Join-Path $buildRoot 'xen-build-identity.json') -Raw | ConvertFrom-Json
if ($identity.git_commit -ne $commit -or $identity.git_dirty) { throw '构建身份未绑定当前干净提交，请重新配置并构建。' }
$binaryRoot = Join-Path $buildRoot 'Release'
$names = @('XenReferenceCompare.exe','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')
foreach ($name in $names) {
    if (!(Test-Path -LiteralPath (Join-Path $binaryRoot $name) -PathType Leaf)) { throw "缺少独立工具运行文件：$name" }
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $binaryRoot $name) -Destination (Join-Path $outputRoot $name) }
foreach ($name in @('example.json','GUIDE.md','LICENSE.cs-match-hud.txt','UPSTREAM.json')) {
    Copy-Item -LiteralPath (Join-Path $repo "assets/reference_assessment/$name") -Destination (Join-Path $outputRoot $name)
}
$files = @(Get-ChildItem -LiteralPath $outputRoot -File | Sort-Object Name | ForEach-Object {
    @{name=$_.Name; bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
@{schema=1; task='AUTO-STOP-REFERENCE-001'; git_commit=$commit; physical_output=$false; files=$files} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot 'manifest.json') -Encoding utf8
Write-Output $outputRoot
