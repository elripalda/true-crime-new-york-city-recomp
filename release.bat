@echo off
cd /d "%~dp0"
setlocal
set "CI=1"
set "OUT=out\build\release"
set "PAL_OUT=out\build\pal"
set "PAL_EU_OUT=out\build\pal-eu"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & exit /b 1)
call build.bat || exit /b 1
call build-pal.bat || exit /b 1
call build-pal.bat eu || exit /b 1
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && exit /b 1
set /p SDK=<out\build\release\sdk_dir.txt
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package-release.ps1" -BuildDir "%OUT%" -PalBuildDir "%PAL_OUT%" -PalEuBuildDir "%PAL_EU_OUT%" -SdkDir "%SDK%" -RuntimeDir "%VCRUNTIME%"
exit /b %errorlevel%
