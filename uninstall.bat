@echo off
setlocal
set "GAME=%~dp0..\"
if exist "%GAME%d3d9.dll" del "%GAME%d3d9.dll" && echo Removed d3d9.dll from the Blur folder. Blur runs flat again.
pause
