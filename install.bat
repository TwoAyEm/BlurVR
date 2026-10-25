@echo off
rem Copies the BlurVR proxy d3d9.dll next to Blur.exe.
setlocal
set "HERE=%~dp0"
set "GAME=%HERE%..\"
if not exist "%GAME%Blur.exe" ( echo Blur.exe not found in "%GAME%". Keep this folder inside the Blur install folder. & pause & exit /b 1 )
copy /Y "%HERE%d3d9.dll" "%GAME%d3d9.dll" >nul && echo Installed. Start SteamVR ^(your OpenXR runtime if BlurVR.ini says Api=openxr^) and then launch Blur from Steam.
pause
