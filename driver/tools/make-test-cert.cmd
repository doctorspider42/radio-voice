@echo off
:: Creates the test signing certificate.
::
:: Run elevated to have it trusted immediately; without elevation the
:: certificate is still created and signing works, but the driver will not
:: install until it is trusted.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-test-cert.ps1" %*
exit /b %errorlevel%
