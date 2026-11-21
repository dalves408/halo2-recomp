@echo off
setlocal
pushd "%~dp0" || exit /b 1
rem The updater (F1 / Config.exe) starts this launcher again after an update.
set "H2_LAUNCHER=%~f0"

rem The console identity, when Insignia Setup saved one. Halo 2 makes its disk
rem caches with it, so every launcher gives it, or the next launch with the other
rem identity shows the "disc is dirty or damaged" error. Nothing secret is printed.
set "ID=SaveData\Identity"
if exist "%ID%\eeprom.bin" if exist "%ID%\hddinfo.txt" if exist "%ID%\eepromkey.bin" (
    set "RECOMP_XBOX_EEPROM=%CD%\%ID%\eeprom.bin"
    set "RECOMP_XBOX_EEPROM_KEY_FILE=%CD%\%ID%\eepromkey.bin"
    if exist "%ID%\titlekeys.bin" set "RECOMP_XBOX_TITLE_KEYS=%CD%\%ID%\titlekeys.bin"
    for /f "usebackq delims=" %%k in (`powershell -NoProfile -Command "$l = Select-String -Path '%ID%\hddinfo.txt' -Pattern 'XboxHDKey' | Select-Object -First 1; if ($l) { (($l.Line -replace '.*XboxHDKey','') -replace '[^0-9A-Fa-f]','').Substring(0,32) }"`) do set "RECOMP_XBOX_HDKEY=%%k"
)

set "RECOMP_FB_WINDOW=1"
if not defined RECOMP_KEYBOARD set "RECOMP_KEYBOARD=1"
if not defined RECOMP_GPU set "RECOMP_GPU=d3d11"

rem The exe asks Windows/NVIDIA/AMD for the high-performance GPU on its own.
rem Console output goes to stderr.log in this folder; the previous run is kept as stderr.prev.log.
rem Debug/test only: enable the DEL screenshot hotkey. Do not set this in a release build.
set "RECOMP_DEBUG_SCREENSHOT=1"
rem Debug/test only: frame rate in the top-left corner (dev builds only).
if not defined RECOMP_DEBUG_FPS set "RECOMP_DEBUG_FPS=1"
echo Debug: press DEL in the game window to save a test screenshot and sidecar into build-ninja\screenshots.
echo Settings and controls: F1 in the game, or build-ninja\Config.exe. Esc asks to quit.
echo Settings are saved to SaveData\settings.ini, controls to SaveData\keymap.ini.
if exist "stderr.log" move /y "stderr.log" "stderr.prev.log" >nul
"build-ninja\halo2_recomp.exe" 1>stdout.log 2>stderr.log
set "halo2_exit_code=%errorlevel%"
if not "%halo2_exit_code%"=="0" echo Halo 2 exited with code %halo2_exit_code%. Send stderr.log and xbox_kernel.log from "%~dp0".
popd
exit /b %halo2_exit_code%
