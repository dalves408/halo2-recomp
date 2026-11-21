@echo off
rem Builds build-ninja\halo2_recomp.exe and halo2_downloader.exe. Needs Visual Studio
rem 2022 (C++ x64 tools), CMake and Ninja on PATH, and the generated code in
rem src\recomp\gen and src\downloader\gen.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio C++ x64 tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build-ninja\build.ninja cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXBOXRECOMP_BUILD_TESTS=OFF
cmake --build build-ninja --target halo2_recomp halo2_downloader halo2_config halo2_insignia_setup halo2_updater halo2_import --parallel
echo BUILD_EXIT=%errorlevel%
