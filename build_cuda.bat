@echo off
setlocal EnableExtensions

if /I "%~1"=="--help" goto usage
if /I "%~1"=="/?" goto usage

pushd "%~dp0" || exit /b 1

where powershell >nul 2>nul
if errorlevel 1 (
    echo [build-cuda] ERROR: powershell was not found in PATH.
    goto fail
)

set "AIMBOT_WRAPPER_ARGS="
set "AIMBOT_HAS_NONINTERACTIVE="
set "AIMBOT_HAS_DOWNLOAD_OPTION="
for %%A in (%*) do (
    if /I "%%~A"=="-NonInteractive" set "AIMBOT_HAS_NONINTERACTIVE=1"
    if /I "%%~A"=="-DownloadOrUpdateNeeded" set "AIMBOT_HAS_DOWNLOAD_OPTION=1"
)
if not defined AIMBOT_HAS_NONINTERACTIVE set "AIMBOT_WRAPPER_ARGS=%AIMBOT_WRAPPER_ARGS% -NonInteractive"
if not defined AIMBOT_HAS_DOWNLOAD_OPTION set "AIMBOT_WRAPPER_ARGS=%AIMBOT_WRAPPER_ARGS% -DownloadOrUpdateNeeded $true"

echo [build-cuda] Checking dependencies and OpenCV automatically.
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build_cuda.ps1" %AIMBOT_WRAPPER_ARGS% %*
if errorlevel 1 goto fail

popd
exit /b 0

:fail
set "AIMBOT_EXIT_CODE=%ERRORLEVEL%"
if "%AIMBOT_EXIT_CODE%"=="0" set "AIMBOT_EXIT_CODE=1"
echo [build-cuda] Failed with exit code %AIMBOT_EXIT_CODE%.
popd
exit /b %AIMBOT_EXIT_CODE%

:usage
echo Usage: build_cuda.bat [tools\build_cuda.ps1 arguments] [extra CMake arguments]
echo.
echo Common options:
echo   -OpenCvAlreadyBuilt $true
echo   -DownloadOrUpdateNeeded $true
echo   -UseLatestPackages
echo   -OpenBrowserForDownloads
echo   -CudaArchBin 8.6
echo   -CudaArchBin all
echo   -OpenCvBuildList cudev,core,imgproc,imgcodecs,videoio,highgui,dnn,cudaarithm,cudaimgproc,cudawarping
echo   -OpenCvMaxCpuCount 2
echo   -OpenCvCleanBuild
echo.
echo Defaults:
echo   BuildDir=build\cuda
echo   Configuration=Release
echo   Generator=Ninja Multi-Config
echo   NonInteractive dependency checks enabled by this wrapper
exit /b 0
