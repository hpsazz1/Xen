[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$RepoRoot = "",
    [string]$OpenCvVersion = "4.13.0",
    [string]$OpenCvModulesDir = "",
    [string]$OpenCvSourceDir = "",
    [string]$OpenCvContribDir = "",
    [string]$OpenCvBuildDir = "",
    [string]$InstallPrefix = "",
    [string]$Generator = "Ninja",
    [string]$NinjaPath = "",
    [string]$Architecture = "x64",
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel", "Debug")]
    [string]$Configuration = "Release",
    [string]$CudaPath = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2",
    [string]$CudnnRoot = "C:\Program Files\NVIDIA\CUDNN\v9.17",
    [string]$CudaArchBin = "",
    [switch]$AutoDetectCudaArch,
    [switch]$DisableCuDNN,
    [switch]$SkipDownload,
    [switch]$ConfigureOnly,
    [switch]$BuildOnly,
    [switch]$NoInstall,
    [string]$BuildList = "",
    [ValidateRange(0, 256)]
    [int]$MaxCpuCount = 0,
    [switch]$CleanBuild,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host "[opencv-cuda] $Message" -ForegroundColor Cyan
}

function Test-Tool {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required tool '$Name' was not found in PATH."
    }
}

function Resolve-NormalizedPath {
    param([string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function ConvertTo-CMakePath {
    param([string]$Path)
    return ($Path -replace "\\", "/")
}

function Invoke-External {
    param(
        [string]$Exe,
        [string[]]$ArgumentList
    )

    $printable = $Exe + " " + (($ArgumentList | ForEach-Object {
                if ($_ -match "\s") { '"' + $_ + '"' } else { $_ }
            }) -join " ")
    Write-Host ">> $printable"

    if ($DryRun) {
        return
    }

    & $Exe @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Exe"
    }
}

function New-DirectoryIfMissing {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Find-FirstExistingFile {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Find-FirstFileRecursive {
    param(
        [string]$Root,
        [string[]]$Patterns
    )

    if (-not (Test-Path -LiteralPath $Root)) {
        return $null
    }

    foreach ($pattern in $Patterns) {
        $found = Get-ChildItem -Path $Root -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }

    return $null
}

function Test-CMakeCacheContains {
    param(
        [string]$BuildDir,
        [string]$Pattern
    )

    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        return $false
    }

    return $null -ne (Select-String -Path $cachePath -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1)
}

function Repair-OpenCvCudevZipHeader {
    param([string]$ContribDir)

    $zipHeader = Join-Path $ContribDir "modules\cudev\include\opencv2\cudev\ptr2d\zip.hpp"
    if (-not (Test-Path -LiteralPath $zipHeader)) {
        throw "OpenCV cudev header was not found: $zipHeader"
    }

    $text = Get-Content -LiteralPath $zipHeader -Raw
    if ($text -match "_CCCL_BEGIN_NAMESPACE_CUDA_STD") {
        return
    }
    if ($text -notmatch "_LIBCUDACXX_BEGIN_NAMESPACE_STD") {
        return
    }

    # CUDA 13.2 renamed the libcu++ namespace helper macros to CCCL names.
    # OpenCV 4.13.0 still references the older names in this cudev header.
    $conditionLine = "#if defined(__CUDACC_VER_MAJOR__) && (__CUDACC_VER_MAJOR__ > 12 || (__CUDACC_VER_MAJOR__ == 12 && __CUDACC_VER_MINOR__ >= 4))"
    $beginPattern = [regex]::Escape($conditionLine) + "\r?\n_LIBCUDACXX_BEGIN_NAMESPACE_STD"
    $beginReplacement = @(
        $conditionLine,
        "#if defined(_LIBCUDACXX_BEGIN_NAMESPACE_STD)",
        "_LIBCUDACXX_BEGIN_NAMESPACE_STD",
        "#elif defined(_CCCL_BEGIN_NAMESPACE_CUDA_STD)",
        "_CCCL_BEGIN_NAMESPACE_CUDA_STD",
        "#else",
        "namespace cuda { namespace std {",
        "#endif"
    ) -join "`r`n"

    $endPattern = "\r?\n_LIBCUDACXX_END_NAMESPACE_STD\r?\n\r?\n#endif\r?\n#endif\s*$"
    $endReplacement = "`r`n" + (@(
        "#if defined(_LIBCUDACXX_END_NAMESPACE_STD)",
        "_LIBCUDACXX_END_NAMESPACE_STD",
        "#elif defined(_CCCL_END_NAMESPACE_CUDA_STD)",
        "_CCCL_END_NAMESPACE_CUDA_STD",
        "#else",
        "}}",
        "#endif",
        "",
        "#endif",
        "#endif"
    ) -join "`r`n")

    $updated = [regex]::Replace($text, $beginPattern, $beginReplacement)
    $updated = [regex]::Replace($updated, $endPattern, $endReplacement)
    if ($updated -eq $text) {
        throw "Could not apply CUDA 13.2 cudev compatibility patch to: $zipHeader"
    }

    Write-Step "Patching OpenCV cudev zip.hpp for CUDA 13.2 CCCL namespace macros"
    if ($DryRun) {
        Write-Host ">> Set-Content -LiteralPath `"$zipHeader`" -Value <patched cudev zip.hpp>"
        return
    }

    Set-Content -LiteralPath $zipHeader -Value $updated -NoNewline
}

function Get-CudaArchFromNvidiaSmi {
    if (-not (Get-Command "nvidia-smi" -ErrorAction SilentlyContinue)) {
        return $null
    }

    try {
        $raw = & nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $raw) {
            return $null
        }

        $line = ($raw | Select-Object -First 1).ToString().Trim()
        $line = $line -replace ",.*$", ""
        if ($line -match "^\d+(\.\d+)?$") {
            return $line
        }
    }
    catch {
        return $null
    }

    return $null
}

function Get-AvailableVisualStudioGenerators {
    $helpText = & cmake --help 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $helpText) {
        return @()
    }

    $generators = $helpText | ForEach-Object {
        if ($_ -match "^\s*\*?\s*(Visual Studio \d+ \d{4})\s*=") {
            $Matches[1]
        }
    } | Select-Object -Unique

    if (-not $generators) {
        return @()
    }

    return $generators | Sort-Object { [int](($_ -split "\s+")[2]) } -Descending
}

function Initialize-OpenCvRepo {
    param(
        [string]$TargetDir,
        [string]$RemoteUrl,
        [string]$Branch
    )

    if (Test-Path -LiteralPath $TargetDir) {
        return
    }

    if ($SkipDownload) {
        throw "Directory not found and -SkipDownload is set: $TargetDir"
    }

    Test-Tool "git"
    New-DirectoryIfMissing ([System.IO.Path]::GetDirectoryName($TargetDir))
    Write-Step "Cloning $RemoteUrl ($Branch) -> $TargetDir"
    Invoke-External "git" @(
        "-c", "advice.detachedHead=false",
        "clone",
        "--depth", "1",
        "--branch", $Branch,
        $RemoteUrl,
        $TargetDir
    )
}

try {
    # Import Visual Studio x64 environment before looking for tools.
    $commonScript = Join-Path $PSScriptRoot 'build_common.ps1'
    if (Test-Path -LiteralPath $commonScript) {
        . $commonScript
        Import-VisualStudioEnvironment -Architecture x64 -HostArchitecture x64
    }

    Test-Tool "cmake"
    if ($Generator -notmatch "^Ninja") {
        throw "Only Ninja generators are supported by this automation. Use -Generator `"Ninja Multi-Config`" or -Generator `"Ninja`"."
    }
    if ([string]::IsNullOrWhiteSpace($NinjaPath)) {
        $ninjaCommand = Get-Command "ninja" -ErrorAction SilentlyContinue
        if ($ninjaCommand) {
            $NinjaPath = $ninjaCommand.Source
        }
    }
    if ([string]::IsNullOrWhiteSpace($NinjaPath) -and -not $DryRun) {
        throw "Required tool 'ninja' was not found in PATH. Run tools/build_cuda.ps1 so it can find or cache Ninja first."
    }

    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
        $RepoRoot = Resolve-NormalizedPath (Join-Path $scriptDir "..")
    }
    else {
        $RepoRoot = Resolve-NormalizedPath $RepoRoot
    }

    if ([string]::IsNullOrWhiteSpace($OpenCvModulesDir)) {
        $OpenCvModulesDir = Join-Path $RepoRoot "Xen\modules\opencv"
    }
    $OpenCvModulesDir = Resolve-NormalizedPath $OpenCvModulesDir

    if ([string]::IsNullOrWhiteSpace($OpenCvSourceDir)) {
        $OpenCvSourceDir = Join-Path $OpenCvModulesDir ("src\opencv-" + $OpenCvVersion)
    }
    $OpenCvSourceDir = Resolve-NormalizedPath $OpenCvSourceDir

    if ([string]::IsNullOrWhiteSpace($OpenCvContribDir)) {
        $OpenCvContribDir = Join-Path $OpenCvModulesDir ("src\opencv_contrib-" + $OpenCvVersion)
    }
    $OpenCvContribDir = Resolve-NormalizedPath $OpenCvContribDir

    if ([string]::IsNullOrWhiteSpace($OpenCvBuildDir)) {
        $OpenCvBuildDir = Join-Path $RepoRoot "build\ocv-cuda"
    }
    $OpenCvBuildDir = Resolve-NormalizedPath $OpenCvBuildDir

    if ([string]::IsNullOrWhiteSpace($InstallPrefix)) {
        $InstallPrefix = Join-Path $RepoRoot "Xen\modules\opencv\build\cuda"
    }
    $InstallPrefix = Resolve-NormalizedPath $InstallPrefix

    $CudaPath = Resolve-NormalizedPath $CudaPath
    $CudnnRoot = Resolve-NormalizedPath $CudnnRoot

    $runConfigure = $true
    $runBuild = $true
    if ($ConfigureOnly) {
        $runBuild = $false
    }
    if ($BuildOnly) {
        $runConfigure = $false
    }
    $runInstall = $runBuild -and (-not $NoInstall)
    $isMultiConfigGenerator = $Generator -match "Multi-Config"
    $parallelArgs = @("--parallel")
    if ($MaxCpuCount -gt 0) {
        $parallelArgs += $MaxCpuCount.ToString()
    }

    Write-Step "Repo root: $RepoRoot"
    Write-Step "OpenCV source: $OpenCvSourceDir"
    Write-Step "OpenCV contrib: $OpenCvContribDir"
    Write-Step "OpenCV build: $OpenCvBuildDir"
    Write-Step "Install prefix: $InstallPrefix"
    Write-Step "CMake generator: $Generator"
    Write-Step "Build parallel args: $($parallelArgs -join ' ')"

    New-DirectoryIfMissing $OpenCvModulesDir
    Initialize-OpenCvRepo -TargetDir $OpenCvSourceDir -RemoteUrl "https://github.com/opencv/opencv.git" -Branch $OpenCvVersion
    Initialize-OpenCvRepo -TargetDir $OpenCvContribDir -RemoteUrl "https://github.com/opencv/opencv_contrib.git" -Branch $OpenCvVersion
    Repair-OpenCvCudevZipHeader -ContribDir $OpenCvContribDir

    $nvccPath = Join-Path $CudaPath "bin\nvcc.exe"
    if (-not (Test-Path -LiteralPath $nvccPath)) {
        throw "CUDA compiler was not found: $nvccPath"
    }

    if ([string]::IsNullOrWhiteSpace($CudaArchBin) -and $AutoDetectCudaArch) {
        $CudaArchBin = Get-CudaArchFromNvidiaSmi
        if ($CudaArchBin) {
            Write-Step "Detected CUDA_ARCH_BIN from nvidia-smi: $CudaArchBin"
        }
    }
    if ([string]::IsNullOrWhiteSpace($CudaArchBin)) {
        $CudaArchBin = "8.6"
        Write-Warning "[opencv-cuda] CUDA_ARCH_BIN is not set; using default value '$CudaArchBin'. Use -CudaArchBin to override."
    }
    $architectureSelection = Resolve-CudaArchitectureSelection -Value $CudaArchBin
    $CudaArchBin = $architectureSelection.OpenCv
    if ($architectureSelection.IsPortablePreset) {
        Write-Step "Using CUDA_ARCH_BIN preset 'all': $CudaArchBin"
    }
    if (-not [string]::IsNullOrWhiteSpace($BuildList)) {
        $buildListModules = @($BuildList -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $requiredBuildListModules = @("cudev", "core", "imgproc", "imgcodecs", "videoio", "highgui")
        foreach ($module in $requiredBuildListModules) {
            if ($buildListModules -notcontains $module) {
                $buildListModules = @($module) + $buildListModules
            }
        }
        $BuildList = ($buildListModules | Select-Object -Unique) -join ","
    }

    $cudaVersion = [System.IO.Path]::GetFileName($CudaPath.TrimEnd("\", "/")) -replace "^[vV]", ""

    $cudnnIncludeDir = ""
    $cudnnLibPath = ""
    if (-not $DisableCuDNN) {
        $cudnnHeader = Find-FirstExistingFile @(
            (Join-Path $CudnnRoot ("include\" + $cudaVersion + "\cudnn.h")),
            (Join-Path $CudnnRoot "include\13.1\cudnn.h"),
            (Join-Path $CudnnRoot "include\12.9\cudnn.h"),
            (Join-Path $CudnnRoot "include\cudnn.h")
        )
        if (-not $cudnnHeader) {
            $cudnnHeader = Find-FirstFileRecursive -Root (Join-Path $CudnnRoot "include") -Patterns @("cudnn.h")
        }
        if (-not $cudnnHeader) {
            throw "cuDNN header was not found under '$CudnnRoot'. Use -CudnnRoot or -DisableCuDNN."
        }
        $cudnnIncludeDir = Split-Path -Parent $cudnnHeader

        $cudnnLibPath = Find-FirstExistingFile @(
            (Join-Path $CudnnRoot ("lib\" + $cudaVersion + "\x64\cudnn.lib")),
            (Join-Path $CudnnRoot ("lib\" + $cudaVersion + "\cudnn.lib")),
            (Join-Path $CudnnRoot ("lib\" + $cudaVersion + "\x64\cudnn64_9.lib")),
            (Join-Path $CudnnRoot "lib\13.1\cudnn.lib"),
            (Join-Path $CudnnRoot "lib\13.1\x64\cudnn.lib"),
            (Join-Path $CudnnRoot "lib\13.1\x64\cudnn64_9.lib"),
            (Join-Path $CudnnRoot "lib\12.9\x64\cudnn.lib"),
            (Join-Path $CudnnRoot "lib\12.9\x64\cudnn64_9.lib"),
            (Join-Path $CudnnRoot "lib\x64\cudnn.lib")
        )
        if (-not $cudnnLibPath) {
            $cudnnLibPath = Find-FirstFileRecursive -Root (Join-Path $CudnnRoot "lib") -Patterns @("cudnn.lib", "cudnn64_*.lib")
        }
        if (-not $cudnnLibPath) {
            throw "cuDNN library was not found under '$CudnnRoot'. Use -CudnnRoot or -DisableCuDNN."
        }

        Write-Step "cuDNN include: $cudnnIncludeDir"
        Write-Step "cuDNN library: $cudnnLibPath"
    }

    if ($CleanBuild -and (Test-Path -LiteralPath $OpenCvBuildDir)) {
        Write-Step "Cleaning build directory: $OpenCvBuildDir"
        if ($DryRun) {
            Write-Host ">> Remove-Item -LiteralPath `"$OpenCvBuildDir`" -Recurse -Force"
        }
        else {
            Remove-Item -LiteralPath $OpenCvBuildDir -Recurse -Force
        }
    }

    New-DirectoryIfMissing $OpenCvBuildDir

    $cmakeConfigureArgs = @(
        "-S", (ConvertTo-CMakePath $OpenCvSourceDir),
        "-B", (ConvertTo-CMakePath $OpenCvBuildDir),
        "-G", $Generator,
        "-DCMAKE_INSTALL_PREFIX=$(ConvertTo-CMakePath $InstallPrefix)",
        "-DCMAKE_SUPPRESS_REGENERATION=ON",
        "-DCMAKE_CUDA_FLAGS=--allow-unsupported-compiler",
        "-DCUDA_NVCC_FLAGS=--allow-unsupported-compiler",
        "-DOPENCV_EXTRA_MODULES_PATH=$(ConvertTo-CMakePath (Join-Path $OpenCvContribDir 'modules'))",
        "-DWITH_CUDA=ON",
        "-DWITH_CUBLAS=ON",
        "-DENABLE_FAST_MATH=ON",
        "-DCUDA_FAST_MATH=ON",
        "-DBUILD_opencv_world=ON",
        "-DWITH_NVCUVENC=OFF",
        "-DWITH_NVCUVID=OFF",
        # UDP 与 NDI 均由项目自己的采集器提供帧，不依赖 OpenCV FFmpeg；IPP 也不是
        # CUDA 推理必需项。关闭二者可避免发布构建被可选二进制下载阻塞。
        "-DWITH_FFMPEG=OFF",
        "-DWITH_IPP=OFF",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_PERF_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DCUDA_ARCH_BIN=$CudaArchBin"
    )
    if (-not [string]::IsNullOrWhiteSpace($BuildList)) {
        $cmakeConfigureArgs += "-DBUILD_LIST=$BuildList"
    }
    if (-not $isMultiConfigGenerator) {
        $cmakeConfigureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
    }
    if (-not [string]::IsNullOrWhiteSpace($NinjaPath)) {
        $cmakeConfigureArgs += "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $NinjaPath)"
    }

    if ($DisableCuDNN) {
        $cmakeConfigureArgs += @(
            "-DWITH_CUDNN=OFF",
            "-DOPENCV_DNN_CUDA=OFF"
        )
    }
    else {
        $cmakeConfigureArgs += @(
            "-DWITH_CUDNN=ON",
            "-DOPENCV_DNN_CUDA=ON",
            "-DCUDNN_INCLUDE_DIR=$(ConvertTo-CMakePath $cudnnIncludeDir)",
            "-DCUDNN_LIBRARY=$(ConvertTo-CMakePath $cudnnLibPath)"
        )
    }

    if ($runConfigure) {
        Write-Step "Configuring OpenCV..."
        Invoke-External "cmake" $cmakeConfigureArgs
    }

    if ($runBuild -and -not $DryRun) {
        $hasNvccBypass = Test-CMakeCacheContains -BuildDir $OpenCvBuildDir -Pattern "^CUDA_NVCC_FLAGS:.*allow-unsupported-compiler"
        if (-not $hasNvccBypass) {
            throw "CMake cache does not contain CUDA_NVCC_FLAGS with --allow-unsupported-compiler. Re-run configure (use script without -BuildOnly, or with -ConfigureOnly first)."
        }
    }

    if ($runBuild) {
        Write-Step "Building all ($Configuration)..."
        $cmakeBuildAllArgs = @(
            "--build", (ConvertTo-CMakePath $OpenCvBuildDir),
            "--target", "all"
        )
        if ($isMultiConfigGenerator) {
            $cmakeBuildAllArgs = @(
                "--build", (ConvertTo-CMakePath $OpenCvBuildDir),
                "--config", $Configuration,
                "--target", "all"
            )
        }
        $cmakeBuildAllArgs += $parallelArgs
        Invoke-External "cmake" $cmakeBuildAllArgs
    }

    if ($runInstall) {
        Write-Step "Building install ($Configuration)..."
        $cmakeBuildInstallArgs = @(
            "--build", (ConvertTo-CMakePath $OpenCvBuildDir),
            "--target", "install"
        )
        if ($isMultiConfigGenerator) {
            $cmakeBuildInstallArgs = @(
                "--build", (ConvertTo-CMakePath $OpenCvBuildDir),
                "--config", $Configuration,
                "--target", "install"
            )
        }
        $cmakeBuildInstallArgs += $parallelArgs
        Invoke-External "cmake" $cmakeBuildInstallArgs
    }

    if (-not $DryRun -and $runInstall) {
        $installedLibDir = Join-Path $InstallPrefix "x64\vc18\lib"
        if (-not (Test-Path -LiteralPath $installedLibDir)) {
            $x64InstallDir = Join-Path $InstallPrefix "x64"
            if (Test-Path -LiteralPath $x64InstallDir) {
                $vcDir = Get-ChildItem -Path $x64InstallDir -Directory -Filter "vc*" -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending |
                    Select-Object -First 1
                if ($vcDir) {
                    $installedLibDir = Join-Path $vcDir.FullName "lib"
                }
            }
        }
        if (Test-Path -LiteralPath $installedLibDir) {
            $opencvWorld = Get-ChildItem -Path $installedLibDir -Filter "opencv_world*.lib" -ErrorAction SilentlyContinue
            if ($opencvWorld) {
                Write-Step "Done. Installed: $($opencvWorld[0].FullName)"
            }
            else {
                Write-Warning "[opencv-cuda] Build completed, but opencv_world*.lib was not found in $installedLibDir"
            }
        }

        # 清单与安装产物放在同一根目录，主构建据此验证架构后才允许复用 OpenCV。
        # 这比读取某个临时 CMakeCache 更可靠，也适用于复制后的依赖目录。
        $manifestPath = Get-OpenCvCudaArchitectureManifestPath -Root $InstallPrefix
        [pscustomobject]@{
            schemaVersion = 1
            openCvVersion = $OpenCvVersion
            cudaToolkit = $CudaPath
            cudaArchBin = $CudaArchBin
            cmakeCudaArchitectures = $architectureSelection.CMake
            configuration = $Configuration
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
        Write-Step "CUDA architecture manifest written: $manifestPath"
    }
}
catch {
    Write-Error $_
    exit 1
}
