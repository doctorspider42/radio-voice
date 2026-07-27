@echo off
:: Downloads the Steinberg ASIO SDK into third_party\asiosdk.
::
:: Running this accepts Steinberg's ASIO SDK Licensing Agreement; a copy is
:: placed next to the sources. The SDK is not redistributable, which is why it
:: is not in the repository and CMake does not fetch it the way it fetches the
:: other dependencies.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fetch-asio-sdk.ps1" %*
exit /b %errorlevel%
