param([Parameter(Mandatory = $true)][string]$HostAddress, [int]$Port = 5014)
$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $python)) { throw '请先运行 install.ps1。' }
& $python -X utf8 -B (Join-Path $PSScriptRoot 'client.py') --host $HostAddress --port $Port
exit $LASTEXITCODE
