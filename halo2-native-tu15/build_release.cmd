@echo off
rem The player release build (scripts\build_installer.py runs this): build-release\, H2_RELEASE.
rem Needs Visual Studio 2022 (C++ x64 tools), CMake and Ninja, and the generated code in
rem src\recomp\gen and src\downloader\gen.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ x64 tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
where ninja >nul 2>nul || set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DH2_RELEASE=ON -DRECOMP_DEBUG_TOOLS=OFF -DXBOXRECOMP_BUILD_TESTS=OFF || exit /b 1
cmake --build build-release --target halo2_recomp halo2_downloader halo2_config halo2_insignia_setup halo2_updater halo2_import halo2_mappack halo2_texture_download --parallel
exit /b %errorlevel%
