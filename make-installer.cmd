@echo off
::=============================================================================
::  Builds the RadioVoice INSTALLER  ->  dist\RadioVoice-<version>-setup.exe
::
::  Usage:
::      make-installer.cmd                 build the app, then the installer
::      make-installer.cmd with-driver     ... and fold the driver in as well
::      make-installer.cmd no-build        use whatever is already in build\bin
::
::  The options combine:  make-installer.cmd no-build with-driver
::
::  WITHOUT with-driver, the driver is included only if a signed one is already
::  sitting in driver\build. If it is not, the installer is built without it and
::  simply does not offer the component - which is a perfectly good installer to
::  hand to someone who is going to use VB-CABLE.
::
::  WITH with-driver, the driver is built, signed and staged first, so that one
::  command produces an installer that can install it. That step needs the WDK
::  and administrator rights, so expect a UAC prompt - and on a machine with no
::  signing certificate yet it creates one and adds it to this machine's trusted
::  stores. installer\README.md spells out what that means.
::
::  To install the driver on THIS machine instead of packaging it:
::      install-driver.cmd
::
::  Needs Inno Setup 7 (note the ".7" - the plain package is still 6):
::      winget install --id JRSoftware.InnoSetup.7 --exact
::=============================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set /p VERSION=<VERSION

::-----------------------------------------------------------------------------
:: Options
::-----------------------------------------------------------------------------
set BUILD_APP=1
set WITH_DRIVER=

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="no-build" (
    set BUILD_APP=
    shift
    goto :parse
)
if /i "%~1"=="with-driver" (
    set WITH_DRIVER=1
    shift
    goto :parse
)
echo [X] Unknown option: %~1
echo     Expected: no-build, with-driver
exit /b 1
:parsed

echo.
echo === RadioVoice installer ^(version %VERSION%^) ===
echo.

::-----------------------------------------------------------------------------
:: Inno Setup
::-----------------------------------------------------------------------------
:: Inno Setup 7 specifically - the script sets SetupArchitecture=x64, which 6
:: does not know. 7 installs alongside 6, so finding a 6 here is not a reason to
:: uninstall anything; it just is not the one to compile with.
::
:: winget installs it per-user under %LOCALAPPDATA%; the installer's own default
:: puts it under Program Files. Both are looked for, because which one you get
:: depends on how you installed it.
::
:: %%~D strips the quotes the list needs, so that ISCC can be quoted once when
:: it is used rather than twice.
set ISCC=
for %%D in (
    "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
    "%ProgramFiles%\Inno Setup 7\ISCC.exe"
    "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe"
) do (
    if exist %%D if not defined ISCC set ISCC=%%~D
)

if not defined ISCC (
    echo [X] Inno Setup 7 not found. Install it:
    echo        winget install --id JRSoftware.InnoSetup.7 --exact
    echo.
    echo     Note the ".7" - the plain JRSoftware.InnoSetup package is still
    echo     version 6, which cannot build the 64-bit installer this script asks
    echo     for. The two versions install side by side.
    goto :fail
)

echo Using: %ISCC%

::-----------------------------------------------------------------------------
:: Application
::-----------------------------------------------------------------------------
echo.
if defined BUILD_APP (
    echo Building the application...
    call "%~dp0build.cmd"
    if !errorlevel! neq 0 goto :fail
) else (
    echo Skipping the application build.
)

if not exist "build\bin\RadioVoice.exe" (
    echo.
    echo [X] build\bin\RadioVoice.exe not found. Run build.cmd first.
    goto :fail
)

::-----------------------------------------------------------------------------
:: Driver payload
::-----------------------------------------------------------------------------
echo.
if defined WITH_DRIVER (
    echo Building, signing and staging the driver...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\build-driver-payload.ps1"
    if !errorlevel! neq 0 goto :fail
) else (
    echo Staging the driver...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\stage-driver.ps1"
    if !errorlevel! neq 0 goto :fail
)

::-----------------------------------------------------------------------------
:: Installer
::-----------------------------------------------------------------------------
echo.
echo Compiling the installer...
"%ISCC%" /Q "%~dp0installer\RadioVoice.iss"
if !errorlevel! neq 0 goto :fail

echo.
echo ================================================================
echo  Built: dist\RadioVoice-%VERSION%-setup.exe
echo ================================================================
exit /b 0

:fail
echo.
echo Installer build failed - see the output above.
exit /b 1
