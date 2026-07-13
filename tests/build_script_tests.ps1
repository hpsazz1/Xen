[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot '..\tools\build_common.ps1')

function Assert-Equal {
    param(
        [Parameter(Mandatory)][object]$Expected,
        [Parameter(Mandatory)][object]$Actual,
        [Parameter(Mandatory)][string]$Message
    )
    if ([string]$Expected -ne [string]$Actual) {
        throw "$Message Expected='$Expected', Actual='$Actual'."
    }
}

$all = Resolve-CudaArchitectureSelection -Value 'all'
Assert-Equal '7.5;8.0;8.6;8.7;8.8;8.9;9.0;10.0;10.3;11.0;12.0;12.1' $all.OpenCv 'all must expand to the portable OpenCV architecture set.'
Assert-Equal '75;80;86;87;88;89;90;100;103;110;120;121' $all.CMake 'Application architectures must match OpenCV architectures.'

$deduplicated = Resolve-CudaArchitectureSelection -Value '8.6;8.6, 12.0'
Assert-Equal '8.6;12.0' $deduplicated.OpenCv 'Explicit architectures must be normalized and deduplicated.'
Assert-Equal '86;120' $deduplicated.CMake 'Explicit architectures must use CMake integer format.'

$invalidRejected = $false
try {
    [void](Resolve-CudaArchitectureSelection -Value '86')
}
catch {
    $invalidRejected = $true
}
Assert-Equal $true $invalidRejected 'Ambiguous OpenCV architectures without a dot must be rejected.'

$selectedPath = Select-VisualStudioEnvironmentPath -Candidates @(
    'C:\Windows\System32;C:\Program Files\CMake\bin',
    'C:\Visual Studio\VC\Tools\MSVC\14.51\bin\Hostx64\x64;C:\Windows\System32'
)
Assert-Equal 'C:\Visual Studio\VC\Tools\MSVC\14.51\bin\Hostx64\x64;C:\Windows\System32' $selectedPath 'VsDevCmd PATH must win over a duplicate stale Path variable.'

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('xen-cuda-arch-test-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    Assert-Equal $false (Test-OpenCvCudaArchitectureCompatible -Root $temporaryRoot -ExpectedOpenCvArchitectures $all.OpenCv) 'Legacy OpenCV without a manifest must not be reused.'

    [pscustomobject]@{ cudaArchBin = '12.0' } |
        ConvertTo-Json |
        Set-Content -LiteralPath (Get-OpenCvCudaArchitectureManifestPath -Root $temporaryRoot) -Encoding UTF8
    Assert-Equal $false (Test-OpenCvCudaArchitectureCompatible -Root $temporaryRoot -ExpectedOpenCvArchitectures $all.OpenCv) 'A single-architecture OpenCV build must not satisfy all.'

    [pscustomobject]@{ cudaArchBin = $all.OpenCv } |
        ConvertTo-Json |
        Set-Content -LiteralPath (Get-OpenCvCudaArchitectureManifestPath -Root $temporaryRoot) -Encoding UTF8
    Assert-Equal $true (Test-OpenCvCudaArchitectureCompatible -Root $temporaryRoot -ExpectedOpenCvArchitectures 'all') 'Matching architecture sets must be reusable.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host '[test] CUDA build architecture tests passed.' -ForegroundColor Green
