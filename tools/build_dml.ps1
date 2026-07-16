[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$RepoRoot = "",
    [string]$BuildDir = "build\dml",
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel", "Debug")]
    [string]$Configuration = "Release",
    [string]$Generator = "Ninja Multi-Config",
    [object]$OpenCvAlreadyBuilt = $null,
    [object]$DownloadOrUpdateNeeded = $null,
    [switch]$UseLatestPackages,
    [switch]$SkipNuGetRestore,
    [switch]$NonInteractive,
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraCMakeArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\build_common.ps1"

if (-not [string]::IsNullOrWhiteSpace($RepoRoot)) {
    $script:RepoRootOverride = $RepoRoot
}

$pushedLocation = $false
try {
    $repo = Get-RepoRoot
    Push-Location $repo
    $pushedLocation = $true

    $allowDownloads = Resolve-OptionalBoolean -Value $DownloadOrUpdateNeeded -Question "Download or update needed files?" -Default $true -NonInteractive:$NonInteractive

    Import-VisualStudioEnvironment
    $ninja = Ensure-Ninja -AllowDownload:$allowDownloads -DryRun:$DryRun
    if ($SkipNuGetRestore) {
        if ($UseLatestPackages) {
            throw '-SkipNuGetRestore cannot be combined with -UseLatestPackages.'
        }
        if (-not (Test-NuGetPackagesReady)) {
            throw 'NuGet restore was skipped, but one or more packages from Xen\packages.config are missing.'
        }
        Write-BuildStep 'Using the complete repository package cache without NuGet restore.' 'dml'
    }
    else {
        Restore-NuGetPackages -UseLatest:$UseLatestPackages -AllowDownload:$allowDownloads -DryRun:$DryRun
    }
    Ensure-CoreSourceModules -AllowDownload:$allowDownloads -DryRun:$DryRun

    $opencvDmlRoot = Resolve-RepoPath "Xen\modules\opencv\build\dml"
    $opencvLayout = Get-OpenCvWorldLayout -Root $opencvDmlRoot -Configuration $Configuration
    $opencvBuilt = Resolve-OpenCvAlreadyBuilt -Value $OpenCvAlreadyBuilt -Backend "dml" -Layout $opencvLayout -NonInteractive:$NonInteractive
    if (-not $opencvBuilt -or -not $opencvLayout) {
        if (-not $allowDownloads) {
            throw "DML OpenCV is not prepared. Re-run with download/update enabled so the prebuilt package can be downloaded."
        }

        $setupArgs = @()
        if ($DryRun) { $setupArgs += "-DryRun" }
        $psArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Resolve-RepoPath "tools\setup_opencv_dml.ps1")
        ) + $setupArgs
        Invoke-External "powershell" $psArgs -DryRun:$DryRun
    }

    $onnxDir = Find-LatestValidPackageDir -PackagePrefix "Microsoft.ML.OnnxRuntime.DirectML" -RequiredRelativeFiles @(
        "build\native\include\onnxruntime_cxx_api.h",
        "runtimes\win-x64\native\onnxruntime.lib"
    )
    if (-not $onnxDir) {
        throw "ONNX Runtime DirectML NuGet package was not restored correctly."
    }

    $directMlDir = Find-LatestValidPackageDir -PackagePrefix "Microsoft.AI.DirectML" -RequiredRelativeFiles @(
        "bin\x64-win\DirectML.dll"
    )
    if (-not $directMlDir) {
        throw "DirectML NuGet package was not restored correctly."
    }

    $buildPath = Resolve-RepoPath $BuildDir
    $resolutionPath = Write-DependencyResolution -OutputDirectory $buildPath -Resolution ([pscustomobject]@{
        backend = "dml"
        generator = $Generator
        configuration = $Configuration
        ninja = $ninja
        opencvRoot = $opencvDmlRoot
        onnxRuntimeDir = $onnxDir
        directMlDir = $directMlDir
    })
    Write-BuildStep "Dependency resolution written to $resolutionPath" "dml"

    $cmakeArgs = @(
        "-S", (ConvertTo-CMakePath $repo),
        "-B", (ConvertTo-CMakePath $buildPath),
        "-G", $Generator,
        "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $ninja)",
        "-DAIMBOT_USE_CUDA=OFF",
        "-DAIMBOT_ONNXRUNTIME_DIR=$(ConvertTo-CMakePath $onnxDir)",
        "-DAIMBOT_DIRECTML_DIR=$(ConvertTo-CMakePath $directMlDir)"
    )
    if ($ExtraCMakeArgs) {
        $cmakeArgs += $ExtraCMakeArgs
    }

    Write-BuildStep "Configuring $BuildDir with $Generator" "dml"
    Invoke-External "cmake" $cmakeArgs -DryRun:$DryRun

    Write-BuildStep "Building $Configuration" "dml"
    Invoke-External "cmake" (New-CMakeApplicationBuildArguments `
        -BuildPath (ConvertTo-CMakePath $buildPath) `
        -Configuration $Configuration) -DryRun:$DryRun

    $canonicalExecutable = Join-Path $buildPath "$Configuration\Xen.exe"
    if (-not $DryRun -and -not (Test-Path -LiteralPath $canonicalExecutable -PathType Leaf)) {
        throw "DML build completed without the canonical executable: $canonicalExecutable"
    }
    Write-LegacyExecutableWarning -Backend DML -CanonicalExecutable $canonicalExecutable
    Write-BuildStep "Done: $canonicalExecutable" "dml"
}
finally {
    if ($pushedLocation) {
        Pop-Location
    }
}
