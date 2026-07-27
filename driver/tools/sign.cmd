@echo off
:: Generates the catalogue and signs the .sys and the .cat. No elevation needed.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign.ps1" %*
exit /b %errorlevel%
