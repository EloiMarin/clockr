@echo off
rem Forge-agnostic CI entry point for Windows (MSVC + Ninja).
rem
rem Mirrors ci/ci.sh: forge workflows should only set up the toolchain and
rem then call this script. Must be run from a shell where vswhere.exe is
rem available (any Visual Studio installation provides it); the script calls
rem vcvars64.bat itself, so a Developer Command Prompt is not required.
rem The actual build is delegated to build.sh (run through Git Bash), so the
rem configure/build/test logic lives in one place.
rem
rem Usage: ci\ci.cmd <windows_x64_debug|windows_x64_release>

setlocal enabledelayedexpansion

set "PRESET=%~1"
if "%PRESET%"=="" (
    echo Usage: ci\ci.cmd ^<windows_x64_debug^|windows_x64_release^>
    exit /b 1
)

if /i not "%PRESET%"=="windows_x64_debug" if /i not "%PRESET%"=="windows_x64_release" (
    echo ci: unknown preset "%PRESET%"
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ci: vswhere.exe not found; install Visual Studio with the C++ workload
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo ci: Visual Studio C++ tools not found
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0.."

set "BUILD_TYPE=debug"
if /i "%PRESET%"=="windows_x64_release" set "BUILD_TYPE=release"

bash build.sh --platform windows --type %BUILD_TYPE% --no-debugger || exit /b 1
