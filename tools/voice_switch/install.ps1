param([string]$PythonLauncher = 'py')
$ErrorActionPreference = 'Stop'
& $PythonLauncher -3 -m venv (Join-Path $PSScriptRoot '.venv')
if ($LASTEXITCODE -ne 0) { throw '创建 Python 环境失败。' }
$python = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
& $python -m pip install -r (Join-Path $PSScriptRoot 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw '安装语音依赖失败。' }
& $python -X utf8 -B (Join-Path $PSScriptRoot 'prepare_model.py')
if ($LASTEXITCODE -ne 0) { throw '安装中文模型失败。' }
Write-Host '依赖和中文模型安装完成，请使用 start.ps1 -HostAddress <辅机地址> 启动。'
