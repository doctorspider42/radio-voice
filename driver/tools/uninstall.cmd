@echo off
:: Removes the device and the driver package, leaving the test certificate in
:: place. To remove that too, use ..\uninstall-driver.cmd.
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
set RESULT=%errorlevel%
echo.
pause
exit /b %RESULT%
