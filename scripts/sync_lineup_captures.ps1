param(
    [Parameter(Mandatory = $true)][string]$LibraryDirectory,
    [Parameter(Mandatory = $true)][string]$InboxDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules') + [IO.Path]::PathSeparator + $env:PSModulePath
$names = @('full.png','roi.png','overview.png','capture.json')

function Require-OrdinaryPath([string]$Path, [bool]$Directory) {
    $entry = Get-Item -LiteralPath $Path -Force
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $entry.PSIsContainer -ne $Directory) {
        throw "采集目录只能使用普通文件或目录：$Path"
    }
}
function Require-Child([string]$Path, [string]$Parent) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw '传送目标超出指定收件目录。' }
}
function Require-LocalAncestors([string]$Path) {
    $current = [IO.Path]::GetPathRoot($Path)
    foreach ($component in $Path.Substring($current.Length).Split('\',[StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $component
        if (-not (Test-Path -LiteralPath $current)) { break }
        Require-OrdinaryPath $current $true
    }
}
function Get-PackageHashes([string]$Directory, [string]$Id) {
    Require-OrdinaryPath $Directory $true
    $entries = @(Get-ChildItem -LiteralPath $Directory -Force)
    if ($entries.Count -ne $names.Count) { throw "截图包不是完整四件套：$Id" }
    $hashes = @{}
    foreach ($name in $names) {
        $path = Join-Path $Directory $name
        Require-OrdinaryPath $path $false
        $size = (Get-Item -LiteralPath $path).Length
        $limit = if ($name -eq 'capture.json') { 65536 } else { 128MB }
        if ($size -le 0 -or $size -gt $limit) { throw "截图文件大小无效：$Id/$name" }
        $hashes[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $metadata = [IO.File]::ReadAllText((Join-Path $Directory 'capture.json')) | ConvertFrom-Json
    if ($metadata.schema -ne 1 -or $metadata.id -cne $Id -or $metadata.origin -cne 'desktop_duplication') {
        throw "截图包身份不匹配：$Id"
    }
    return $hashes
}
function Assert-SameHashes($Expected, $Actual, [string]$Id) {
    foreach ($name in $names) {
        if ($Expected[$name] -cne $Actual[$name]) { throw "同名截图内容不一致，拒绝覆盖：$Id/$name" }
    }
}

$library = [IO.Path]::GetFullPath($LibraryDirectory)
if ($library.StartsWith('\\') -or ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($library))).DriveType -notin @('Fixed','Removable','Ram')) {
    throw '采集源必须是主机本地目录。'
}
Require-LocalAncestors $library
Require-OrdinaryPath $library $true
$source = Join-Path $library 'captures'
Require-OrdinaryPath $source $true
$inbox = [IO.Path]::GetFullPath($InboxDirectory)
Require-OrdinaryPath $inbox $true
$destination = Join-Path $inbox 'captures'
Require-Child $destination $inbox
if (-not (Test-Path -LiteralPath $destination)) { [void][IO.Directory]::CreateDirectory($destination) }
Require-OrdinaryPath $destination $true
$sent = 0; $skipped = 0
foreach ($entry in Get-ChildItem -LiteralPath $source -Force | Sort-Object Name) {
    # 正在写入的截图还未原子发布，本次不排队也不传半包。
    if ($entry.Name.StartsWith('.pending-')) { continue }
    $id = $entry.Name
    if (-not $entry.PSIsContainer -or $id -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,119}$') { throw "采集编号无效：$id" }
    $expected = Get-PackageHashes $entry.FullName $id
    $target = Join-Path $destination $id
    Require-Child $target $destination
    if (Test-Path -LiteralPath $target) {
        Assert-SameHashes $expected (Get-PackageHashes $target $id) $id
        $skipped++
        continue
    }
    $stage = Join-Path $destination ('.pending-transfer-' + [Guid]::NewGuid().ToString('N'))
    Require-Child $stage $destination
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $entry.FullName $name) -Destination (Join-Path $stage $name) }
        Assert-SameHashes $expected (Get-PackageHashes $stage $id) $id
        Assert-SameHashes $expected (Get-PackageHashes $entry.FullName $id) $id
        # 移动前再次解析精确目标，仅提交本次创建的暂存包。
        Require-Child $stage $destination
        Require-Child $target $destination
        if (Test-Path -LiteralPath $target) { throw "传送期间目标编号已出现，请重新发送：$id" }
        [IO.Directory]::Move($stage,$target)
        $sent++
    } finally {
        if (Test-Path -LiteralPath $stage) {
            Require-Child $stage $destination
            Require-OrdinaryPath $stage $true
            foreach ($name in $names) {
                $temporaryFile = Join-Path $stage $name
                if (Test-Path -LiteralPath $temporaryFile) { Remove-Item -LiteralPath $temporaryFile }
            }
            [IO.Directory]::Delete($stage,$false)
        }
    }
}
Write-Output "发送完成：新增 $sent 张，已有相同数据 $skipped 张。本地原图保留；辅机画面源就绪后导入，未启动任何验证程序。"
