@echo off
setlocal
pushd "%~dp0" || exit /b 1

set "RECOMP_FB_WINDOW=1"
if not defined RECOMP_KEYBOARD set "RECOMP_KEYBOARD=1"
if not defined RECOMP_GPU set "RECOMP_GPU=d3d11"

rem The exe asks Windows/NVIDIA/AMD for the high-performance GPU on its own.
rem Console output goes to stderr.log in this folder; the previous run is kept as stderr.prev.log.
rem Debug/test only: enable the DEL screenshot hotkey. Do not set this in a release build.
set "RECOMP_DEBUG_SCREENSHOT=1"
echo Debug: press DEL in the game window to save a test screenshot and sidecar into build-ninja\screenshots.
echo Settings: F1 in the game, or run build-ninja\Config.exe. ESC opens a quit prompt.
echo Settings are saved to %LOCALAPPDATA%\Halo2Native\settings.ini
if exist "stderr.log" move /y "stderr.log" "stderr.prev.log" >nul
"build-ninja\halo2_recomp.exe" 1>stdout.log 2>stderr.log
set "halo2_exit_code=%errorlevel%"
if not "%halo2_exit_code%"=="0" echo Halo 2 exited with code %halo2_exit_code%. Send stderr.log and xbox_kernel.log from "%~dp0".
popd
exit /b %halo2_exit_code%
