@echo off
::=============================================================================
::  RadioVoice virtual audio cable - build, sign and install in one command.
::
::  Double-click it, or run it from any shell. It elevates itself, so there is
::  no need to remember to open an administrator prompt first.
::
::  What it does, in order:
::    1. builds the driver             (driver\build.ps1)
::    2. creates a test certificate    (tools\make-test-cert.ps1)  - once
::    3. signs the .sys and the .cat   (tools\sign.ps1)
::    4. installs and creates the device (tools\install.ps1)
::
::  Test signing has to be on before step 4 can work, and turning it on needs a
::  reboot. This script detects that, offers to turn it on, and tells you to run
::  it again afterwards - it cannot do anything useful across a reboot.
::=============================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

::-----------------------------------------------------------------------------
:: Elevate
::-----------------------------------------------------------------------------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo.
echo ================================================================
echo  RadioVoice Virtual Audio Cable - install
echo ================================================================
echo.

::-----------------------------------------------------------------------------
:: Test signing
::
:: Checked before anything is built: there is no point compiling and signing a
:: driver that the machine will refuse to load.
::-----------------------------------------------------------------------------
bcdedit /enum "{current}" | findstr /i "testsigning" | findstr /i "Yes" >nul
if %errorlevel% neq 0 (
    echo [!] Test signing is OFF.
    echo.
    echo     Windows will not load a test-signed driver without it. Turning it
    echo     on means this machine accepts any kernel driver signed by a
    echo     certificate in its own trusted stores - one of the layers that
    echo     protects against rootkits stops applying.
    echo.
    echo     It is reversible:  bcdedit /set testsigning off
    echo.
    choice /c YN /m "Turn test signing on now"
    if !errorlevel! equ 2 (
        echo.
        echo Aborted. Nothing was changed.
        goto :done
    )

    bcdedit /set testsigning on
    if !errorlevel! neq 0 (
        echo.
        echo [X] bcdedit failed. If it reported success but the setting does not
        echo     stick, Secure Boot is still enabled - turn it off in the
        echo     firmware first.
        goto :fail
    )

    echo.
    echo [*] Test signing enabled. REBOOT, then run this script again.
    goto :done
)

echo [1/4] Test signing is on.

::-----------------------------------------------------------------------------
:: Build
::-----------------------------------------------------------------------------
echo.
echo [2/4] Building the driver...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Configuration Release
if %errorlevel% neq 0 goto :fail

::-----------------------------------------------------------------------------
:: Certificate
::
:: Created only once. Re-running would mint a second certificate and leave the
:: first one trusted for no reason.
::-----------------------------------------------------------------------------
echo.
if exist "%~dp0build\cert\thumbprint.txt" (
    echo [3/4] Reusing the existing test certificate.
) else (
    echo [3/4] Creating a test certificate...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\make-test-cert.ps1"
    if !errorlevel! neq 0 goto :fail
)

echo.
echo       Signing...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\sign.ps1"
if %errorlevel% neq 0 goto :fail

::-----------------------------------------------------------------------------
:: Install
::-----------------------------------------------------------------------------
echo.
echo [4/4] Installing...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1"
if %errorlevel% neq 0 goto :fail

echo.
echo ================================================================
echo  Done.
echo.
echo   Playback  : RadioVoice Output      ^<- point RadioVoice here
echo   Recording : RadioVoice Microphone  ^<- point Discord/OBS here
echo ================================================================
goto :done

:fail
echo.
echo ================================================================
echo  FAILED - see the output above.
echo ================================================================
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
