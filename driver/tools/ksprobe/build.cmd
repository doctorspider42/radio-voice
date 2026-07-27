@echo off
:: Builds ksprobe. User-mode, so MinGW is enough - no WDK involved.
setlocal
cd /d "%~dp0"

where g++ >nul 2>&1
if %errorlevel% neq 0 (
    echo [X] g++ not found. Install it, for example:  scoop install mingw
    exit /b 1
)

g++ -std=c++17 -O2 -municode -o ksprobe.exe ksprobe.cpp -lsetupapi -lole32 -luuid
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

echo Built: %~dp0ksprobe.exe
exit /b 0
