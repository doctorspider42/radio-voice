@echo off
:: Builds the driver. Arguments are passed straight through to build.ps1, so
::     build.cmd -Configuration Debug
:: works the same as calling the script directly.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
