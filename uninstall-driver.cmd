@echo off
::=============================================================================
::  Removes the driver, the device and the test certificate.
::
::  Test signing is left alone: turning it off needs a reboot, and it is a
::  machine-wide setting that may well have been on for something else before
::  this driver existed. The command to turn it off is printed at the end.
::=============================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set DRIVER=%~dp0driver

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo.
echo ================================================================
echo  RadioVoice Virtual Audio Cable - uninstall
echo ================================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%DRIVER%\tools\uninstall.ps1"
set UNINSTALL_RESULT=%errorlevel%

echo.
echo Removing the test certificate...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher, Cert:\CurrentUser\My -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq 'CN=RadioVoice Test Signing' } | Remove-Item -Force -ErrorAction SilentlyContinue; Write-Host '  done'"

echo.
if %UNINSTALL_RESULT% neq 0 (
    echo [!] The driver removal reported a problem - see above.
) else (
    echo Removed.
)

echo.
echo To leave test-signing mode as well ^(then reboot^):
echo     bcdedit /set testsigning off
echo.
pause
exit /b %UNINSTALL_RESULT%
