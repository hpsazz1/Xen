[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    $override = Get-Variable -Name RepoRootOverride -Scope Script -ErrorAction SilentlyContinue
    if ($override -and -not [string]::IsNullOrWhiteSpace([string]$override.Value)) {
        return [System.IO.Path]::GetFullPath([string]$override.Value)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
}

function Resolve-RepoPath {
    param([Parameter(Mandatory)][string]$RelativePath)
    return [System.IO.Path]::GetFullPath((Join-Path (Get-RepoRoot) $RelativePath))
}

function Write-BuildStep {
    param(
        [Parameter(Mandatory)][string]$Message,
        [string]$Prefix = 'build'
    )
    Write-Host "[$Prefix] $Message" -ForegroundColor Cyan
}

function New-DirectoryIfMissing {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function ConvertTo-CMakePath {
    param([Parameter(Mandatory)][string]$Path)
    return ($Path -replace '\\', '/')
}

function Resolve-CudaArchitectureSelection {
    param(
        [Parameter(Mandatory)][string]$Value
    )

    # CUDA 13.2 的 nvcc 从 Turing 7.5 起提供离线编译支持。这里显式维护发布架构，
    # 避免 CMake 或 OpenCV 根据构建机显卡自动选择单一架构，导致发布包换机后无法执行内核。
    $allArchitectures = @('7.5', '8.0', '8.6', '8.7', '8.8', '8.9', '9.0', '10.0', '10.3', '11.0', '12.0', '12.1')
    $trimmedValue = $Value.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmedValue)) {
        throw 'CUDA architecture cannot be empty.'
    }

    $architectures = if ($trimmedValue.ToLowerInvariant() -eq 'all') {
        $allArchitectures
    }
    else {
        @($trimmedValue -split '[;,\s]+' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }

    $normalized = [System.Collections.Generic.List[string]]::new()
    foreach ($architecture in $architectures) {
        if ($architecture -notmatch '^(\d+)\.(\d+)$') {
            throw "Invalid CUDA architecture '$architecture'. Use values such as 8.6, a semicolon-separated list, or all."
        }

        $canonical = ([int]$Matches[1]).ToString() + '.' + ([int]$Matches[2]).ToString()
        if (-not $normalized.Contains($canonical)) {
            $normalized.Add($canonical)
        }
    }

    if ($normalized.Count -eq 0) {
        throw 'At least one CUDA architecture is required.'
    }

    # OpenCV 的旧 FindCUDA 接口使用带小数点的 CUDA_ARCH_BIN；现代 CMake 的
    # CMAKE_CUDA_ARCHITECTURES 使用不带小数点的整数。两者必须来自同一份选择。
    $cmakeArchitectures = @($normalized | ForEach-Object { $_ -replace '\.', '' })
    return [pscustomobject]@{
        Requested = $trimmedValue
        OpenCv = ($normalized -join ';')
        CMake = ($cmakeArchitectures -join ';')
        Items = @($normalized)
        IsPortablePreset = ($trimmedValue.ToLowerInvariant() -eq 'all')
    }
}

function Get-OpenCvCudaArchitectureManifestPath {
    param([Parameter(Mandatory)][string]$Root)
    return (Join-Path $Root 'opencv-cuda-build.json')
}

function Read-OpenCvCudaArchitectureManifest {
    param([Parameter(Mandatory)][string]$Root)

    $path = Get-OpenCvCudaArchitectureManifestPath -Root $Root
    if (-not (Test-Path -LiteralPath $path)) {
        return $null
    }

    try {
        return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
    }
    catch {
        Write-Warning "[cuda] Invalid OpenCV CUDA build manifest '$path': $($_.Exception.Message)"
        return $null
    }
}

function Test-OpenCvCudaArchitectureCompatible {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ExpectedOpenCvArchitectures
    )

    # 不根据 DLL 是否存在推断 CUDA 架构。旧构建没有清单时按不兼容处理并重建，
    # 因为单架构 DLL 在构建机上可正常加载，却会在其他显卡执行时才延迟失败。
    $manifest = Read-OpenCvCudaArchitectureManifest -Root $Root
    if (-not $manifest -or -not $manifest.cudaArchBin) {
        return $false
    }

    try {
        $actual = Resolve-CudaArchitectureSelection -Value ([string]$manifest.cudaArchBin)
        $expected = Resolve-CudaArchitectureSelection -Value $ExpectedOpenCvArchitectures
        return $actual.OpenCv -eq $expected.OpenCv
    }
    catch {
        return $false
    }
}

function Confirm-YesNo {
    param(
        [Parameter(Mandatory)][string]$Question,
        [bool]$Default = $true,
        [switch]$AssumeYes,
        [switch]$AssumeNo,
        [switch]$NonInteractive
    )

    if ($AssumeYes) { return $true }
    if ($AssumeNo) { return $false }
    if ($NonInteractive) { return $Default }

    $suffix = if ($Default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $answer = Read-Host "$Question $suffix"
        if ([string]::IsNullOrWhiteSpace($answer)) {
            return $Default
        }
        switch -Regex ($answer.Trim()) {
            '^(y|yes)$' { return $true }
            '^(n|no)$' { return $false }
        }
        Write-Host 'Please answer yes or no.'
    }
}

function Resolve-OptionalBoolean {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Question,
        [bool]$Default = $true,
        [switch]$NonInteractive
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) {
        return Confirm-YesNo -Question $Question -Default $Default -NonInteractive:$NonInteractive
    }

    if ($Value -is [bool]) { return [bool]$Value }
    switch -Regex ([string]$Value) {
        '^\$?true$|^1$|^y$|^yes$' { return $true }
        '^\$?false$|^0$|^n$|^no$' { return $false }
    }
    throw "Invalid boolean value '$Value' for: $Question"
}

function Resolve-OpenCvAlreadyBuilt {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Backend,
        [AllowNull()][object]$Layout,
        [switch]$NonInteractive
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) {
        if ($Layout) {
            Write-BuildStep "Detected valid OpenCV layout: $($Layout.Root)" $Backend
            return $true
        }

        Write-BuildStep "OpenCV layout is missing or incomplete; it will be prepared automatically." $Backend
        return $false
    }

    return Resolve-OptionalBoolean `
        -Value $Value `
        -Question "OpenCV already built for $($Backend.ToUpperInvariant())?" `
        -Default $false `
        -NonInteractive:$NonInteractive
}

function Invoke-External {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [switch]$DryRun
    )

    $printable = $Exe + ' ' + (($ArgumentList | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' ')
    Write-Host ">> $printable"

    if ($DryRun) { return }

    & $Exe @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Exe"
    }
}

function Get-ToolCacheDir {
    return (Resolve-RepoPath 'tools\.bin')
}

function Get-ModulesDownloadCacheDir {
    return (Resolve-RepoPath 'Xen\modules\_downloads')
}

function Get-DownloadsDirectory {
    $userProfile = [Environment]::GetFolderPath('UserProfile')
    $downloads = Join-Path $userProfile 'Downloads'
    if (Test-Path -LiteralPath $downloads) {
        return $downloads
    }
    return $userProfile
}

function Get-CommandPath {
    param([Parameter(Mandatory)][string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-VsWherePath {
    $fromPath = Get-CommandPath 'vswhere.exe'
    if ($fromPath) { return $fromPath }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $candidate = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Get-VisualStudioInstallation {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        throw 'vswhere.exe was not found. Install Visual Studio Build Tools or Visual Studio Community with C++ tools.'
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installPath)) {
        throw 'No Visual Studio installation with C++ tools was found.'
    }

    $installPath = $installPath.Trim()
    $vsDevCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd)) {
        throw "VsDevCmd.bat was not found: $vsDevCmd"
    }

    [pscustomobject]@{
        InstallationPath = $installPath
        VsDevCmd = $vsDevCmd
    }
}

function Select-VisualStudioEnvironmentPath {
    param([Parameter(Mandatory)][string[]]$Candidates)

    # 某些宿主进程会同时注入 PATH 与 Path。cmd.exe 能保留两行，而 PowerShell/.NET
    # 按大小写不敏感方式写回时会互相覆盖；优先保留 VsDevCmd 注入了 MSVC 的那一项。
    $developerPath = $Candidates |
        Where-Object { $_ -match '\\VC\\Tools\\MSVC\\' } |
        Sort-Object Length -Descending |
        Select-Object -First 1
    if ($developerPath) {
        return $developerPath
    }
    return ($Candidates | Sort-Object Length -Descending | Select-Object -First 1)
}

function Import-VisualStudioEnvironment {
    param(
        [string]$Architecture = 'x64',
        [string]$HostArchitecture = 'x64'
    )

    $requiredTools = @('cl.exe', 'link.exe', 'rc.exe', 'mt.exe')

    # Check if the correct x64 compiler is already in PATH.
    # cl.exe from Hostx64/x64 or Hostx86/x64 means x64 target is active.
    # cl.exe from Hostx86/x86 means x86 target — not acceptable.
    $clPath = Get-CommandPath 'cl.exe'
    if ($clPath) {
        $clDir = Split-Path -Path $clPath -Parent
        $archSuffix = if ($clDir -match '\\(x64|arm64|ARM64)\s*$') { $Matches[1] } else { '' }
        $isX64 = ($clDir -match '\\x64\\' -or $clDir -match '\\x64$' -or $clDir -like '*Host*x64*')
        if ($isX64) {
            $missingTools = @($requiredTools | Where-Object { -not (Get-CommandPath $_) })
            if ($missingTools.Count -eq 0) {
                Write-BuildStep "Detected x64 toolchain: $clDir" 'toolchain'
                return
            }
        }
        else {
            Write-BuildStep "Found x86 toolchain at $clDir; will re-initialize for x64" 'toolchain'
        }
    }

    $vs = Get-VisualStudioInstallation
    Write-BuildStep "Importing Visual Studio environment from VsDevCmd.bat" 'toolchain'
    $cmdLine = '"' + $vs.VsDevCmd + '"' + " -arch=$Architecture -host_arch=$HostArchitecture && set"
    $envLines = & cmd.exe /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw 'VsDevCmd.bat failed to initialize the compiler environment.'
    }

    $pathCandidates = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $envLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2]
            if ($name.Equals('Path', [System.StringComparison]::OrdinalIgnoreCase)) {
                $pathCandidates.Add($value)
                continue
            }
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
    if ($pathCandidates.Count -gt 0) {
        $developerPath = Select-VisualStudioEnvironmentPath -Candidates @($pathCandidates)
        [Environment]::SetEnvironmentVariable('Path', $developerPath, 'Process')
    }

    $missingTools = @($requiredTools | Where-Object { -not (Get-CommandPath $_) })
    if ($missingTools.Count -gt 0) {
        throw "Visual Studio environment is incomplete. Missing: $($missingTools -join ', '). Install the Windows SDK and Desktop development with C++ workload."
    }
}

function Get-VisualStudioBundledNinja {
    try {
        $vs = Get-VisualStudioInstallation
    }
    catch {
        return $null
    }

    $candidates = @(
        (Join-Path $vs.InstallationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'),
        (Join-Path $vs.InstallationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja-win\ninja.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Invoke-DownloadFile {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$OutFile,
        [switch]$DryRun
    )

    New-DirectoryIfMissing ([System.IO.Path]::GetDirectoryName($OutFile))
    Write-BuildStep "Downloading $Uri" 'download'
    if ($DryRun) {
        Write-Host ">> Invoke-WebRequest -Uri `"$Uri`" -OutFile `"$OutFile`""
        return
    }

    $tmp = "$OutFile.tmp"
    if (Test-Path -LiteralPath $tmp) {
        Remove-Item -LiteralPath $tmp -Force
    }
    Invoke-WebRequest -Uri $Uri -OutFile $tmp -MaximumRedirection 10
    Move-Item -LiteralPath $tmp -Destination $OutFile -Force
}

function Ensure-NuGet {
    param(
        [switch]$AllowDownload,
        [switch]$DryRun
    )

    $nuget = Get-CommandPath 'nuget.exe'
    if ($nuget) { return $nuget }

    $cached = Join-Path (Get-ToolCacheDir) 'nuget.exe'
    if (Test-Path -LiteralPath $cached) { return $cached }
    if (-not $AllowDownload) {
        throw 'nuget.exe was not found. Re-run with download/update enabled so it can be cached under tools\.bin.'
    }

    Invoke-DownloadFile -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $cached -DryRun:$DryRun
    return $cached
}

function Ensure-Ninja {
    param(
        [switch]$AllowDownload,
        [switch]$DryRun
    )

    $ninja = Get-CommandPath 'ninja.exe'
    if ($ninja) { return $ninja }

    $vsNinja = Get-VisualStudioBundledNinja
    if ($vsNinja) {
        $env:PATH = ([System.IO.Path]::GetDirectoryName($vsNinja)) + [System.IO.Path]::PathSeparator + $env:PATH
        return $vsNinja
    }

    $cached = Join-Path (Get-ToolCacheDir) 'ninja.exe'
    if (Test-Path -LiteralPath $cached) {
        $env:PATH = (Get-ToolCacheDir) + [System.IO.Path]::PathSeparator + $env:PATH
        return $cached
    }
    if (-not $AllowDownload) {
        throw 'ninja.exe was not found. Re-run with download/update enabled so it can be cached under tools\.bin.'
    }

    $zipPath = Join-Path (Get-ToolCacheDir) 'ninja-win.zip'
    Invoke-DownloadFile -Uri 'https://github.com/ninja-build/ninja/releases/latest/download/ninja-win.zip' -OutFile $zipPath -DryRun:$DryRun
    if (-not $DryRun) {
        Expand-Archive -LiteralPath $zipPath -DestinationPath (Get-ToolCacheDir) -Force
    }
    $env:PATH = (Get-ToolCacheDir) + [System.IO.Path]::PathSeparator + $env:PATH
    return $cached
}

function Restore-NuGetPackages {
    param(
        [switch]$UseLatest,
        [switch]$AllowDownload,
        [switch]$DryRun
    )

    $nuget = Ensure-NuGet -AllowDownload:$AllowDownload -DryRun:$DryRun
    $packagesDir = Resolve-RepoPath 'packages'
    New-DirectoryIfMissing $packagesDir

    if ($UseLatest) {
        $packages = @(
            'Microsoft.ML.OnnxRuntime.DirectML',
            'Microsoft.AI.DirectML',
            'Microsoft.Windows.CppWinRT',
            'nlohmann.json'
        )
        foreach ($package in $packages) {
            Invoke-External $nuget @(
                'install', $package,
                '-OutputDirectory', $packagesDir,
                '-DependencyVersion', 'Highest',
                '-Source', 'https://api.nuget.org/v3/index.json',
                '-NonInteractive'
            ) -DryRun:$DryRun
        }
        return
    }

    Invoke-External $nuget @(
        'restore', (Resolve-RepoPath 'Xen\packages.config'),
        '-PackagesDirectory', $packagesDir,
        '-Source', 'https://api.nuget.org/v3/index.json',
        '-NonInteractive'
    ) -DryRun:$DryRun
}

function Test-CoreSourceModules {
    $serialRoot = Resolve-RepoPath 'Xen\modules\serial'
    $simpleIni = Resolve-RepoPath 'Xen\modules\SimpleIni.h'

    [pscustomobject]@{
        SimpleIniReady = Test-Path -LiteralPath $simpleIni
        SerialReady = (
            (Test-Path -LiteralPath (Join-Path $serialRoot 'src\serial.cc')) -and
            (Test-Path -LiteralPath (Join-Path $serialRoot 'src\impl\win.cc')) -and
            (Test-Path -LiteralPath (Join-Path $serialRoot 'src\impl\list_ports\list_ports_win.cc')) -and
            (Test-Path -LiteralPath (Join-Path $serialRoot 'include\serial\serial.h'))
        )
    }
}

function Install-SerialModule {
    param([switch]$DryRun)

    $serialRoot = Resolve-RepoPath 'Xen\modules\serial'
    if (Test-Path -LiteralPath (Join-Path $serialRoot 'src\serial.cc')) {
        return
    }

    $git = Get-CommandPath 'git.exe'
    if ($git) {
        Write-BuildStep "Cloning wjwwood/serial into Xen\modules\serial" "modules"
        Invoke-External $git @(
            'clone',
            '--depth', '1',
            'https://github.com/wjwwood/serial.git',
            $serialRoot
        ) -DryRun:$DryRun
        return
    }

    $zipPath = Join-Path (Get-ModulesDownloadCacheDir) 'serial-main.zip'
    $extractRoot = Join-Path (Get-ModulesDownloadCacheDir) 'serial-main'
    Invoke-DownloadFile -Uri 'https://github.com/wjwwood/serial/archive/refs/heads/main.zip' -OutFile $zipPath -DryRun:$DryRun
    if ($DryRun) { return }

    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force
    $sourceRoot = Get-ChildItem -Path $extractRoot -Directory | Select-Object -First 1
    if (-not $sourceRoot) {
        throw 'Could not find extracted serial source root.'
    }
    Copy-Item -LiteralPath $sourceRoot.FullName -Destination $serialRoot -Recurse -Force
}

function Ensure-CoreSourceModules {
    param(
        [switch]$AllowDownload,
        [switch]$DryRun
    )

    $state = Test-CoreSourceModules
    if ($state.SimpleIniReady -and $state.SerialReady) {
        return
    }
    if (-not $AllowDownload) {
        $missing = @()
        if (-not $state.SimpleIniReady) { $missing += 'SimpleIni.h' }
        if (-not $state.SerialReady) { $missing += 'serial' }
        throw "Required source modules are missing: $($missing -join ', '). Re-run with download/update enabled."
    }

    New-DirectoryIfMissing (Resolve-RepoPath 'Xen\modules')

    if (-not $state.SimpleIniReady) {
        Invoke-DownloadFile `
            -Uri 'https://raw.githubusercontent.com/brofield/simpleini/master/SimpleIni.h' `
            -OutFile (Resolve-RepoPath 'Xen\modules\SimpleIni.h') `
            -DryRun:$DryRun
    }
    if (-not $state.SerialReady) {
        Install-SerialModule -DryRun:$DryRun
    }
}

function Find-LatestValidPackageDir {
    param(
        [Parameter(Mandatory)][string]$PackagePrefix,
        [Parameter(Mandatory)][string[]]$RequiredRelativeFiles
    )

    $packagesDir = Resolve-RepoPath 'packages'
    if (-not (Test-Path -LiteralPath $packagesDir)) { return $null }

    $candidates = Get-ChildItem -Path $packagesDir -Directory -Filter "$PackagePrefix.*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($candidate in $candidates) {
        $ok = $true
        foreach ($relative in $RequiredRelativeFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $candidate.FullName $relative))) {
                $ok = $false
                break
            }
        }
        if ($ok) { return $candidate.FullName }
    }
    return $null
}

function Get-NvidiaGpuInfo {
    $smi = Get-CommandPath 'nvidia-smi.exe'
    if (-not $smi) { return $null }

    try {
        $raw = & $smi --query-gpu=name,driver_version,compute_cap --format=csv,noheader 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $raw) { return $null }
        $parts = (($raw | Select-Object -First 1).ToString() -split ',') | ForEach-Object { $_.Trim() }
        if ($parts.Count -lt 3) { return $null }
        [pscustomobject]@{
            Name = $parts[0]
            DriverVersion = $parts[1]
            ComputeCapability = $parts[2]
        }
    }
    catch {
        return $null
    }
}

function Get-KnownDependencyCompatibilityProfiles {
    @(
        [pscustomobject]@{
            Name = "CUDA 13 latest compatible"
            CudaVersion = "13.2"
            TensorRtVersion = "10.16"
            OpenCvVersion = "4.13.0"
            OnnxRuntimeDirectMlVersion = "latest"
            DirectMlVersion = "latest"
            CudnnRequired = $false
            Notes = "cuDNN is optional because the project uses TensorRT and OpenCV CUDA preprocessing, not OpenCV DNN inference."
        },
        [pscustomobject]@{
            Name = "CUDA 13.1 fallback"
            CudaVersion = "13.1"
            TensorRtVersion = "10.14"
            OpenCvVersion = "4.13.0"
            OnnxRuntimeDirectMlVersion = "latest"
            DirectMlVersion = "latest"
            CudnnRequired = $false
            Notes = "Fallback for machines already pinned to the previous CUDA 13.1 setup."
        }
    )
}

function Get-CudaInstallations {
    $roots = New-Object System.Collections.Generic.List[string]
    if ($env:CUDA_PATH) { $roots.Add($env:CUDA_PATH) }

    $cudaBase = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA'
    if (Test-Path -LiteralPath $cudaBase) {
        Get-ChildItem -Path $cudaBase -Directory -Filter 'v*' -ErrorAction SilentlyContinue |
            ForEach-Object { $roots.Add($_.FullName) }
    }

    $roots | Select-Object -Unique | Where-Object {
        Test-Path -LiteralPath (Join-Path $_ 'bin\nvcc.exe')
    } | Sort-Object {
        $name = [System.IO.Path]::GetFileName($_) -replace '^[vV]', ''
        [version]($name -replace '[^\d\.].*$', '')
    } -Descending
}

function Find-TensorRtRoot {
    $modulesDir = Resolve-RepoPath 'Xen\modules'
    if (-not (Test-Path -LiteralPath $modulesDir)) { return $null }

    Get-ChildItem -Path $modulesDir -Directory -Filter 'TensorRT-*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'include\NvInfer.h')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'lib\nvinfer_10.lib')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\nvinfer_10.dll'))
        } |
        Select-Object -First 1 -ExpandProperty FullName
}

function Find-CudnnRoot {
    $modulesDir = Resolve-RepoPath 'Xen\modules'
    $roots = @(
        (Join-Path $modulesDir 'cudnn'),
        (Join-Path $env:ProgramFiles 'NVIDIA\CUDNN')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $header = Get-ChildItem -Path $root -Recurse -File -Filter 'cudnn.h' -ErrorAction SilentlyContinue | Select-Object -First 1
        $lib = Get-ChildItem -Path $root -Recurse -File -Include 'cudnn.lib','cudnn64_*.lib' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($header -and $lib) { return $root }
    }
    return $null
}

function Get-BestCompatibleCudaDependencySet {
    $gpu = Get-NvidiaGpuInfo
    $cudaRoot = Get-CudaInstallations | Select-Object -First 1
    $tensorRtRoot = Find-TensorRtRoot
    $cudnnRoot = Find-CudnnRoot

    $cudaVersion = ''
    if ($cudaRoot) {
        $cudaVersion = ([System.IO.Path]::GetFileName($cudaRoot.TrimEnd('\', '/')) -replace '^[vV]', '')
    }

    $arch = ''
    if ($gpu -and $gpu.ComputeCapability -match '^\d+(\.\d+)?$') {
        $arch = $gpu.ComputeCapability
    }
    if ([string]::IsNullOrWhiteSpace($arch)) {
        $arch = '8.6'
    }

    $profiles = Get-KnownDependencyCompatibilityProfiles
    $profile = $profiles | Select-Object -First 1
    if ($cudaVersion) {
        $matchingProfile = $profiles | Where-Object { $cudaVersion.StartsWith($_.CudaVersion) } | Select-Object -First 1
        if ($matchingProfile) { $profile = $matchingProfile }
    }

    [pscustomobject]@{
        CompatibilityProfile = $profile.Name
        DesiredCudaVersion = $profile.CudaVersion
        DesiredTensorRtVersion = $profile.TensorRtVersion
        DesiredOpenCvVersion = $profile.OpenCvVersion
        CudaRoot = $cudaRoot
        CudaVersion = $cudaVersion
        TensorRtRoot = $tensorRtRoot
        CudnnRoot = $cudnnRoot
        UseCudnnForOpenCvDnn = $profile.CudnnRequired
        CudaArchBin = $arch
        Gpu = $gpu
        Notes = @(
            $profile.Notes,
            'The selected CUDA set is based on locally installed CUDA plus validated TensorRT layout.'
        )
    }
}

function New-DependencyDownloadItem {
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$Reason,
        [Parameter(Mandatory)][string]$DownloadUrl,
        [Parameter(Mandatory)][string[]]$ExpectedFilePatterns,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Action
    )

    [pscustomobject]@{
        id = $Id
        name = $Name
        version = $Version
        reason = $Reason
        downloadUrl = $DownloadUrl
        expectedFilePatterns = $ExpectedFilePatterns
        destination = $Destination
        action = $Action
    }
}

function Write-DependencyDownloadManifest {
    param(
        [Parameter(Mandatory)][object[]]$Items
    )

    $buildDir = Resolve-RepoPath 'build'
    New-DirectoryIfMissing $buildDir
    $jsonPath = Join-Path $buildDir 'dependency-downloads.json'
    $mdPath = Join-Path $buildDir 'dependency-downloads.md'

    $Items | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

    $lines = @('# Dependency downloads', '')
    foreach ($item in $Items) {
        $lines += "- [ ] $($item.name) $($item.version)"
        $lines += "  - Reason: $($item.reason)"
        $lines += "  - URL: $($item.downloadUrl)"
        $lines += "  - Save to: $(Get-DownloadsDirectory)"
        $lines += "  - Destination: $($item.destination)"
    }
    $lines | Set-Content -LiteralPath $mdPath -Encoding UTF8

    [pscustomobject]@{
        JsonPath = $jsonPath
        MarkdownPath = $mdPath
    }
}

function Find-DownloadedDependencyFile {
    param(
        [Parameter(Mandatory)][string[]]$Patterns,
        [string]$DownloadsDir = (Get-DownloadsDirectory)
    )

    foreach ($pattern in $Patterns) {
        $found = Get-ChildItem -Path $DownloadsDir -File -Filter $pattern -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

function Copy-ManualDownloadsToCache {
    param(
        [Parameter(Mandatory)][object[]]$Items,
        [string]$DownloadsDir = (Get-DownloadsDirectory)
    )

    $cacheDir = Get-ModulesDownloadCacheDir
    New-DirectoryIfMissing $cacheDir
    $copied = @()

    foreach ($item in $Items) {
        $file = Find-DownloadedDependencyFile -Patterns $item.expectedFilePatterns -DownloadsDir $DownloadsDir
        if (-not $file) {
            throw "Missing downloaded file for $($item.name). Expected one of: $($item.expectedFilePatterns -join ', ')"
        }
        $dest = Join-Path $cacheDir ([System.IO.Path]::GetFileName($file))
        Copy-Item -LiteralPath $file -Destination $dest -Force
        $copied += [pscustomobject]@{
            id = $item.id
            source = $file
            cached = $dest
            action = $item.action
            destination = $item.destination
        }
    }

    return $copied
}

function Expand-DependencyArchive {
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][string]$Destination
    )

    New-DirectoryIfMissing $Destination
    if ($ArchivePath -match '\.zip$') {
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Destination -Force
        return
    }
    if ($ArchivePath -match '\.(tar\.gz|tgz)$') {
        Invoke-External "tar" @("-xf", $ArchivePath, "-C", $Destination)
        return
    }
    if ($ArchivePath -match '\.exe$') {
        Start-Process -FilePath $ArchivePath -Wait -Verb RunAs
        return
    }
    throw "Unsupported dependency archive type: $ArchivePath"
}

function Invoke-GuidedDependencyDownloads {
    param(
        [Parameter(Mandatory)][object[]]$Items,
        [switch]$OpenBrowser,
        [switch]$NonInteractive,
        [switch]$DryRun
    )

    if (-not $Items -or $Items.Count -eq 0) {
        return @()
    }

    $manifest = Write-DependencyDownloadManifest -Items $Items
    Write-Host "Missing dependency files were listed in: $($manifest.MarkdownPath)"

    if ($OpenBrowser) {
        foreach ($item in $Items) {
            Start-Process $item.downloadUrl
        }
    }

    if (-not $NonInteractive) {
        [void](Read-Host "Download the listed files to $(Get-DownloadsDirectory), then press Enter to continue")
    }

    if ($DryRun) {
        Write-Host "Dry-run mode: skipping Downloads folder scan."
        return @()
    }

    return Copy-ManualDownloadsToCache -Items $Items
}

function Write-DependencyResolution {
    param(
        [Parameter(Mandatory)][object]$Resolution
    )

    $buildDir = Resolve-RepoPath 'build'
    New-DirectoryIfMissing $buildDir
    $path = Join-Path $buildDir 'dependency-resolution.json'
    $Resolution | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

function Get-OpenCvWorldLayout {
    param(
        [Parameter(Mandatory)][string]$Root,
        [ValidateSet("Release", "Debug")]
        [string]$Configuration = "Release"
    )

    $includeDir = Join-Path $Root 'include'
    if (-not (Test-Path -LiteralPath (Join-Path $includeDir 'opencv2\opencv.hpp'))) {
        return $null
    }

    $x64Dir = Join-Path $Root 'x64'
    if (-not (Test-Path -LiteralPath $x64Dir)) { return $null }

    $vcDirs = Get-ChildItem -Path $x64Dir -Directory -Filter 'vc*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($vcDir in $vcDirs) {
        $libs = Get-ChildItem -Path (Join-Path $vcDir.FullName 'lib') -File -Filter 'opencv_world*.lib' -ErrorAction SilentlyContinue |
            Where-Object {
                if ($Configuration -eq 'Debug') {
                    $_.BaseName -match 'd$'
                }
                else {
                    $_.BaseName -notmatch 'd$'
                }
            } |
            Sort-Object Name -Descending
        foreach ($lib in $libs) {
            $stem = [System.IO.Path]::GetFileNameWithoutExtension($lib.Name)
            $dll = Join-Path $vcDir.FullName "bin\$stem.dll"
            if (Test-Path -LiteralPath $dll) {
                return [pscustomobject]@{
                    Root = $Root
                    IncludeDir = $includeDir
                    Library = $lib.FullName
                    Dll = $dll
                    VcDir = $vcDir.FullName
                    WorldStem = $stem
                }
            }
        }
    }
    if ($Configuration -ne 'Release') {
        return $null
    }

    $fallbackLibs = Get-ChildItem -Path (Join-Path $vcDir.FullName "lib") -File -Filter "opencv_world*.lib" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($lib in $fallbackLibs) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($lib.Name)
        $dllPath = Join-Path $vcDir.FullName ("bin\" + $stem + ".dll")
        if (Test-Path -LiteralPath $dllPath) {
            return [pscustomobject]@{
                Root = $Root
                IncludeDir = $includeDir
                Library = $lib.FullName
                Dll = $dllPath
                VcDir = $vcDir.FullName
                WorldStem = $stem
            }
        }
    }

    return $null
}
