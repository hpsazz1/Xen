param(
    [Parameter(Mandatory = $true)][string]$InboxDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../cache/lineup-host-captures'),
    [string]$Executable = (Join-Path $PSScriptRoot '../build/lineup/Release/XenLineupHost.exe'),
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$binary = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "找不到主机采集工具：$binary" }
$arguments = @('--inbox', [IO.Path]::GetFullPath($InboxDirectory), '--output', [IO.Path]::GetFullPath($OutputDirectory))
if ($CheckOnly) { $arguments += '--dry-run' }
& $binary @arguments
if ($LASTEXITCODE -ne 0) { throw "主机采集工具返回 $LASTEXITCODE；请查看上方原因。" }
