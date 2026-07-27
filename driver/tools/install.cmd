@echo off
:: Installs an already-built, already-signed package.
::
:: For the whole sequence at once - build, certificate, signature, install -
:: use ..\install-driver.cmd instead.
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set RESULT=%errorlevel%
echo.
pause
exit /b %RESULT%
