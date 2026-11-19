@echo off
rem System Link debug session: DEL saves a screenshot + sidecar + draw list into
rem build-ninja\screenshots; this session's console log and per-frame timing
rem go there too, named by start time, so every DEL can be matched to them.
setlocal
pushd "%~dp0" || exit /b 1

set "RECOMP_FB_WINDOW=1"
if not defined RECOMP_KEYBOARD set "RECOMP_KEYBOARD=1"
if not defined RECOMP_GPU set "RECOMP_GPU=d3d11"
set "RECOMP_DEBUG_SCREENSHOT=1"
rem Ethernet controller on: System Link (LAN, Tailscale). No internet without a console identity.
set "RECOMP_NET=1"

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
