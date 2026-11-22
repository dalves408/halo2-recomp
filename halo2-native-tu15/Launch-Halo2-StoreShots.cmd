@echo off
rem Store screenshots: the offline launcher at 3x render scale (2560x1440 at 16:9),
rem 16x anisotropic filtering and FXAA. MSAA stays off: DEL captures lose the HUD
rem with it. Your saved settings are not changed. Press DEL for each shot; they go
rem to build-ninja\screenshots.
set "RECOMP_GPU_SCALE=3"
set "RECOMP_GPU_ANISO=16"
set "RECOMP_GPU_FXAA=1"
set "RECOMP_GPU_MSAA=1"
set "RECOMP_DEBUG_FPS=0"
call "%~dp0Launch-Halo2.cmd" %*
