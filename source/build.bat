@echo off
setlocal enabledelayedexpansion
rem ---------------------------------------------------------------------------------------------
rem  BlurVR build script (Windows).  Double-click, or run from a command prompt:
rem      build.bat            build d3d9.dll and install it into the game folder (close the game first)
rem      build.bat nodeploy   build only; leaves d3d9.dll in this folder
rem  Needs Python 3 on the PATH; the Zig compiler is installed automatically with pip the first time.
rem  The previous game d3d9.dll is kept in ..\previous_build\ (named by its MD5) before it is replaced.
rem ---------------------------------------------------------------------------------------------
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY ( where python >nul 2>nul && set "PY=python" )
if not defined PY (
    echo Python 3 was not found. Install it from https://www.python.org/downloads/ and tick "Add to PATH".
    goto :fail
)

%PY% -c "import ziglang" >nul 2>nul
if errorlevel 1 (
    echo Installing the Zig compiler ^(pip install ziglang^) ...
    %PY% -m pip install ziglang
    if errorlevel 1 goto :fail
)

set "SOURCES="
for %%f in (src\*.cpp) do set "SOURCES=!SOURCES! %%f"

echo Building d3d9.dll ...
if exist d3d9.dll del /q d3d9.dll
%PY% -m ziglang c++ -target x86-windows-gnu -O2 -s -ffunction-sections -fdata-sections -Wl,--gc-sections -shared -fno-exceptions -fno-rtti -std=c++17 -Ithird_party -o d3d9.dll !SOURCES! d3d9.def -ld3d11 -ldxgi -luser32 -lkernel32
set "BUILD_ERROR=!errorlevel!"
del /q *.obj *.lib 2>nul
if not "!BUILD_ERROR!"=="0" goto :fail
if not exist d3d9.dll goto :fail

for /f "skip=1 tokens=*" %%h in ('certutil -hashfile d3d9.dll MD5') do if not defined NEWHASH set "NEWHASH=%%h"
set "NEWHASH=!NEWHASH: =!"
echo Built d3d9.dll   MD5 !NEWHASH!

if /i "%~1"=="nodeploy" goto :done

if not exist "..\..\Blur.exe" (
    echo This folder is not inside the Blur game folder, so nothing was installed.  d3d9.dll is here: %cd%
    goto :done
)

if exist "..\..\d3d9.dll" (
    set "OLDHASH="
    for /f "skip=1 tokens=*" %%h in ('certutil -hashfile "..\..\d3d9.dll" MD5') do if not defined OLDHASH set "OLDHASH=%%h"
    set "OLDHASH=!OLDHASH: =!"
    if not exist "..\previous_build" mkdir "..\previous_build"
    if not exist "..\previous_build\d3d9_!OLDHASH:~0,8!.dll" copy /y "..\..\d3d9.dll" "..\previous_build\d3d9_!OLDHASH:~0,8!.dll" >nul
)

copy /y d3d9.dll "..\..\d3d9.dll" >nul
if errorlevel 1 (
    echo Could not copy into the game folder - is Blur still running?  Close it and run build.bat again.
    goto :fail
)
copy /y d3d9.dll "..\d3d9.dll" >nul
echo Installed to the game folder and BlurVR\.  Start the game to try it.

:done
echo Done.
if "%~1"=="" pause
exit /b 0

:fail
echo.
echo BUILD FAILED.
if "%~1"=="" pause
exit /b 1
