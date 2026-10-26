param(
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [Parameter(Mandatory=$true)][string]$NamesPath,
    [Parameter(Mandatory=$true)][string]$ReportPath,
    [switch]$Apply
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../../scripts/path_safety.psm1') -Force
$root=(Resolve-Path -LiteralPath $PackageRoot).ProviderPath.TrimEnd('\','/')
Assert-XenNoReparsePathChain $root '方案迁移包根' -RequireExistingLeaf
$sourceRoot=Join-Path $root 'cache/movement/plans'
$targetRoot=Join-Path $root 'cache/grenade/plans'
Assert-XenNoReparsePathChain $sourceRoot '身法方案库' -RequireExistingLeaf
Assert-XenNoReparsePathChain $targetRoot '道具方案库'
$names=@(Get-Content -LiteralPath $NamesPath -Raw -Encoding UTF8 | ConvertFrom-Json)
if(-not $names.Count -or @($names | Sort-Object -Unique).Count -ne $names.Count){throw '迁移清单为空或重复'}
$entries=@()
# 整批预检后才移动；已迁移项可重入，不覆盖目标侧人工修改。
foreach($name in $names){
    if($name -isnot [string] -or $name -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_.-]*\.json$'){throw '迁移项必须是单层JSON文件名'}
    $source=Resolve-XenDirectChildPath $sourceRoot $name '待迁移方案'
    $target=[IO.Path]::GetFullPath((Join-Path $targetRoot $name))
    if([IO.Path]::GetDirectoryName($target) -ine $targetRoot){throw '道具方案目标越界'}
    Assert-XenNoReparsePathChain $source '待迁移方案'
    Assert-XenNoReparsePathChain $target '道具方案目标'
    $hasSource=Test-Path -LiteralPath $source -PathType Leaf
    $hasTarget=Test-Path -LiteralPath $target -PathType Leaf
    if($hasSource -and $hasTarget){throw "两库存在同名文件，保留双方并停止：$name"}
    if(-not $hasSource -and -not $hasTarget){throw "迁移项不存在：$name"}
    $existing=if($hasSource){$source}else{$target}
    $hash=(Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash.ToLowerInvariant()
    $entries+= [pscustomobject]@{name=$name;source=$source;target=$target;sha256=$hash;already_moved=(-not $hasSource)}
}
$protected=@(Get-ChildItem -LiteralPath $sourceRoot -File -Filter '*.json' | Where-Object {$_.Name -cnotin $names} | ForEach-Object {
    @{path=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
if($Apply){
    New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
    foreach($entry in $entries){
        if($entry.already_moved){continue}
        Assert-XenNoReparsePathChain $entry.source '迁移前来源' -RequireExistingLeaf
        Assert-XenNoReparsePathChain $entry.target '迁移前目标'
        if((Get-FileHash -LiteralPath $entry.source).Hash.ToLowerInvariant() -cne $entry.sha256){throw '迁移期间方案被修改，停止'}
        # 单文件同卷重命名；不递归移动、不使用Force、不覆盖目标。
        Move-Item -LiteralPath $entry.source -Destination $entry.target
        if((Test-Path -LiteralPath $entry.source) -or (Get-FileHash -LiteralPath $entry.target).Hash.ToLowerInvariant() -cne $entry.sha256){throw '迁移回读失败'}
    }
    foreach($entry in $protected){
        if((Get-FileHash -LiteralPath $entry.path).Hash.ToLowerInvariant() -cne $entry.sha256){throw '非道具方案发生变化'}
    }
}
$report=[ordered]@{schema=1;package=$root;applied=[bool]$Apply;count=$entries.Count;protected=$protected;entries=$entries}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Host "道具方案迁移：$($entries.Count)项；Apply=$([bool]$Apply)；保护$($protected.Count)项。"
