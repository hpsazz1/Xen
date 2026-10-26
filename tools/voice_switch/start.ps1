param([Parameter(Mandatory = $true)][string]$HostAddress, [int]$Port = 5014,
    [ValidateSet('left','right')][string]$Corner = 'right',
    [string]$References = '', [switch]$Voice, [string]$PythonLauncher = 'py')
$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if ($Voice -and -not (Test-Path -LiteralPath $python)) { throw '语音模式请先运行 install.ps1；目录模式不需要安装语音环境。' }
if (-not $References) { $References = Join-Path $PSScriptRoot '../../cache/grenade/references' }
$clientArguments = @('-X','utf8','-B',(Join-Path $PSScriptRoot 'client.py'),'--host',$HostAddress,
    '--port',"$Port",'--corner',$Corner,'--references',$References)
if ($Voice) { $clientArguments += '--voice' }
if (Test-Path -LiteralPath $python) { & $python @clientArguments }
else { & $PythonLauncher -3 @clientArguments }
exit $LASTEXITCODE
