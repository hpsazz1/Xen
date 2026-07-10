@echo off
setlocal EnableExtensions

pushd "%~dp0" || exit /b 1

echo ============================================================
echo  build_no-options - No-download main program builder
echo ============================================================
echo.

where powershell >nul 2>nul
if errorlevel 1 (
    echo [build_no-options] ERROR: powershell was not found in PATH.
    echo.
    pause
    popd
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_no-options.ps1" %*
set "BUILD_EXIT_CODE=%ERRORLEVEL%"

echo.
if "%BUILD_EXIT_CODE%"=="0" (
    echo [build_no-options] Build complete.
) else (
    echo [build_no-options] Build failed with exit code %BUILD_EXIT_CODE%.
)
echo.
pause

popd
exit /b %BUILD_EXIT_CODE%
