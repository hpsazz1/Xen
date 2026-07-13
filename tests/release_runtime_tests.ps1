[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('CUDA', 'DML')]
    [string]$Backend,

    [Parameter(Mandatory)]
    [string]$ReleaseDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-FileExists {
    param([Parameter(Mandatory)][string]$Name)
    $path = Join-Path $ReleaseDir $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "[$Backend] Required release file is missing: $Name"
    }
}

function Assert-PatternAbsent {
    param([Parameter(Mandatory)][string]$Pattern)
    $matches = @(Get-ChildItem -LiteralPath $ReleaseDir -File -Filter $Pattern -ErrorAction Stop)
    if ($matches.Count -gt 0) {
        throw "[$Backend] Stale or non-runtime release file found: $($matches.Name -join ', ')"
    }
}

function Assert-PatternExists {
    param([Parameter(Mandatory)][string]$Pattern)
    $matches = @(Get-ChildItem -LiteralPath $ReleaseDir -File -Filter $Pattern -ErrorAction Stop)
    if ($matches.Count -eq 0) {
        throw "[$Backend] Required release file pattern has no matches: $Pattern"
    }
}

if (-not (Test-Path -LiteralPath $ReleaseDir -PathType Container)) {
    throw "Release directory does not exist: $ReleaseDir"
}

@(
    'Xen.exe'
    'opencv_world4130.dll'
    'Processing.NDI.Lib.x64.dll'
    'ghub_mouse.dll'
    'rzctl.dll'
) | ForEach-Object { Assert-FileExists -Name $_ }

@('Xen.exp', 'Xen.lib', 'xen_*_tests.exe') |
    ForEach-Object { Assert-PatternAbsent -Pattern $_ }

if ($Backend -eq 'CUDA') {
    Assert-PatternExists -Pattern 'nvinfer_builder_resource_*.dll'
    @(
        'nvinfer_10.dll'
        'nvonnxparser_10.dll'
        'cublas64_13.dll'
        'cublasLt64_13.dll'
        'cufft64_12.dll'
        'nppc64_13.dll'
        'nppial64_13.dll'
        'nppicc64_13.dll'
        'nppidei64_13.dll'
        'nppig64_13.dll'
        'nppist64_13.dll'
        'nppitc64_13.dll'
        'concrt140.dll'
        'msvcp140.dll'
        'vcruntime140.dll'
        'vcruntime140_1.dll'
    ) | ForEach-Object { Assert-FileExists -Name $_ }

    @(
        'DirectML.dll'
        'onnxruntime*.dll'
        'cpbox_mouse_dll.dll'
        'cudart*.dll'
        'cudnn*.dll'
        'cufftw*.dll'
        'nppif*.dll'
        'nppim*.dll'
        'nppisu*.dll'
        'npps*.dll'
        'nvinfer_dispatch*.dll'
        'nvinfer_lean*.dll'
        'nvinfer_plugin*.dll'
        'nvinfer_vc_plugin*.dll'
        'nvJitLink*.dll'
        'nvrtc*.dll'
        'msvcp140_1.dll'
        'msvcp140_2.dll'
        'msvcp140_atomic_wait.dll'
        'msvcp140_codecvt_ids.dll'
        'vccorlib140.dll'
        'vcruntime140_threads.dll'
    ) | ForEach-Object { Assert-PatternAbsent -Pattern $_ }
}
else {
    @(
        'DirectML.dll'
        'onnxruntime.dll'
        'onnxruntime_providers_shared.dll'
    ) | ForEach-Object { Assert-FileExists -Name $_ }

    @('cudart*.dll', 'cudnn*.dll', 'nvinfer*.dll', 'nvonnxparser*.dll', 'onnxruntime_providers_cuda.dll') |
        ForEach-Object { Assert-PatternAbsent -Pattern $_ }
}

Write-Host "[test] $Backend release runtime layout passed." -ForegroundColor Green
