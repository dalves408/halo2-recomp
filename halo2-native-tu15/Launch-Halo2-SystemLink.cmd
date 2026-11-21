@echo off
rem System Link debug session: DEL saves a screenshot + sidecar + draw list into
rem build-ninja\screenshots; this session's console log and per-frame timing
rem go there too, named by start time, so every DEL can be matched to them.
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
set "RECOMP_DEBUG_SCREENSHOT=1"
rem Ethernet controller on: System Link (LAN, Tailscale). No internet without a console identity.
set "RECOMP_NET=1"
rem System Link only: the router to the internet stays off, identity or not.
set "RECOMP_NET_NAT=0"

set "SHOTS=build-ninja\screenshots"
if not exist "%SHOTS%" mkdir "%SHOTS%"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%t"
set "LOG=%SHOTS%\session_%STAMP%.log"
set "RECOMP_PERF_CSV=%SHOTS%\session_%STAMP%_perf.csv"

echo Debug session %STAMP%
echo   DEL in the game window = screenshot + sidecar + draw list in %SHOTS%
echo   console log: %LOG%
echo   frame timing: %RECOMP_PERF_CSV%
"build-ninja\halo2_recomp.exe" 1>"%SHOTS%\session_%STAMP%_stdout.log" 2>"%LOG%"
set "halo2_exit_code=%errorlevel%"
echo Halo 2 exited with code %halo2_exit_code%. Session files are in %SHOTS% (session_%STAMP%*).
popd
exit /b %halo2_exit_code%
