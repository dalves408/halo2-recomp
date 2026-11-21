@echo off
rem Xbox Live (Insignia) test session: network card on, the player's own console
rem identity from SaveData\Identity (eeprom.bin + hddinfo.txt, never shared).
rem DEL saves a screenshot + sidecar + draw list; the session log and frame
rem timing go to build-ninja\screenshots too. Only one copy of this console
rem identity may be online at a time (console, xemu, CounterStrikeXB, this).
setlocal
pushd "%~dp0" || exit /b 1
rem The updater (F1 / Config.exe) starts this launcher again after an update.
set "H2_LAUNCHER=%~f0"

set "ID=SaveData\Identity"
if not exist "%ID%\eeprom.bin" (echo No console identity: run build-ninja\"Insignia Setup.exe" first. & popd & exit /b 1)
if not exist "%ID%\hddinfo.txt" (echo No %ID%\hddinfo.txt: run build-ninja\"Insignia Setup.exe" first. & popd & exit /b 1)
rem Insignia Setup verified the EEPROM and hard-drive key against the console's own
rem BIOS; the game checks them again with this key before anything reaches the internet.
if not exist "%ID%\eepromkey.bin" (echo Your console identity is not verified yet: run build-ninja\"Insignia Setup.exe" first. & popd & exit /b 1)
set "RECOMP_XBOX_EEPROM_KEY_FILE=%CD%\%ID%\eepromkey.bin"
set "RECOMP_XBOX_EEPROM=%CD%\%ID%\eeprom.bin"
rem The hard-drive key: the XboxHDKey line of hddinfo.txt, 32 hex digits. Not echoed.
for /f "usebackq delims=" %%k in (`powershell -NoProfile -Command "$l = Select-String -Path '%ID%\hddinfo.txt' -Pattern 'XboxHDKey' | Select-Object -First 1; if ($l) { (($l.Line -replace '.*XboxHDKey','') -replace '[^0-9A-Fa-f]','').Substring(0,32) }"`) do set "RECOMP_XBOX_HDKEY=%%k"
if not defined RECOMP_XBOX_HDKEY (echo No XboxHDKey line in %ID%\hddinfo.txt & popd & exit /b 1)
rem The title keys a real kernel derives from the BIOS (installer\derive_title_keys.py).
if exist "%ID%\titlekeys.bin" set "RECOMP_XBOX_TITLE_KEYS=%CD%\%ID%\titlekeys.bin"

set "RECOMP_FB_WINDOW=1"
if not defined RECOMP_KEYBOARD set "RECOMP_KEYBOARD=1"
if not defined RECOMP_GPU set "RECOMP_GPU=d3d11"
set "RECOMP_DEBUG_SCREENSHOT=1"
rem Debug/test only: frame rate in the top-left corner (dev builds only).
if not defined RECOMP_DEBUG_FPS set "RECOMP_DEBUG_FPS=1"
set "RECOMP_NET=1"
rem Test builds: log every second with Xbox Live traffic bursts (match detection tuning).
if not defined RECOMP_NET_LIVE_LOG set "RECOMP_NET_LIVE_LOG=1"
rem XLaunchNewImage between the game and the disc's content downloader: each
rem image name maps to its native program (launch data goes through SaveData).
set "RECOMP_LAUNCH_MAP=downloader.xbe=build-ninja\halo2_downloader.exe;default.xbe=build-ninja\halo2_recomp.exe"

set "SHOTS=build-ninja\screenshots"
if not exist "%SHOTS%" mkdir "%SHOTS%"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%t"
set "LOG=%SHOTS%\session_%STAMP%.log"
set "RECOMP_PERF_CSV=%SHOTS%\session_%STAMP%_perf.csv"

echo Insignia session %STAMP% (identity loaded from %ID%)
echo   console log: %LOG%
if not defined H2_EXE set "H2_EXE=build-ninja\halo2_recomp.exe"
"%H2_EXE%" 1>"%SHOTS%\session_%STAMP%_stdout.log" 2>"%LOG%"
set "halo2_exit_code=%errorlevel%"
echo Halo 2 exited with code %halo2_exit_code%.
popd
exit /b %halo2_exit_code%
