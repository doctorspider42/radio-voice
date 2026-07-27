@echo off
::=============================================================================
::  Builds the KERNEL DRIVER  ->  driver\build\Release\RadioVoiceAudio.sys
::
::  For the application, use build.cmd in the repository root instead.
::
::  Usage:
::      build-driver.cmd            release build
::      build-driver.cmd debug      with DbgPrintEx tracing and assertions
::
::  To build, sign and install in one go, use install-driver.cmd.
::=============================================================================

setlocal
cd /d "%~dp0"

set CONFIG=Release
if /i "%~1"=="debug" set CONFIG=Debug

echo.
echo === RadioVoice kernel driver ^(%CONFIG%^) ===
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Configuration %CONFIG%
exit /b %errorlevel%
