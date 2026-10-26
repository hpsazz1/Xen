param(
    [Parameter(Mandatory = $true)][string]$HostAddress,
    [ValidateRange(1,65535)][int]$WebPort = 5015,
    [string]$CaptureRoot = '',
    [string]$DestinationRoot = '\\192.168.3.20\XenLab$\releases\Xen-current\cache\grenade',
    [string]$PythonLauncher = 'py'
)
$ErrorActionPreference = 'Stop'
if (-not $CaptureRoot) { $CaptureRoot = Join-Path $PSScriptRoot '../../cache/grenade' }
$python = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
$arguments = @('-X','utf8','-B',(Join-Path $PSScriptRoot 'capture_hotkey.py'),
    '--host',$HostAddress,'--web-port',"$WebPort",'--root',$CaptureRoot,'--destination',$DestinationRoot)
if (Test-Path -LiteralPath $python) { & $python @arguments }
else { & $PythonLauncher -3 @arguments }
exit $LASTEXITCODE
