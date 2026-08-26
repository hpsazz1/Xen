param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-release-096e1a7-nvidia"),
    [ValidatePattern('^udp://[^:]+:[0-9]+$')]
    [string]$BindUrl = "udp://192.168.3.10:5011"
)

$ErrorActionPreference = "Stop"
$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$executable = Join-Path $resolvedBuild "Release\XenClockSource.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "缺少源机时钟旁路可执行文件：$executable"
}

Write-Host "启动 NDI 源机时钟旁路：$BindUrl"
Write-Host "该进程只返回时间证据；Ctrl+C 停止，不会访问 KMBOX 或发送鼠标输入。"
& $executable --bind $BindUrl
exit $LASTEXITCODE
