@echo off
:: Generates the catalogue and signs the .sys and the .cat.
::
:: Needs an elevated prompt whenever the certificate lives in LocalMachine\My,
:: which is where make-test-cert.ps1 puts it when run elevated - and it has to
:: be run elevated, or the installer cannot see the key at all. signtool then
:: fails with "No certificates were found that met all the given criteria",
:: which sounds like the certificate is missing rather than unreadable.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign.ps1" %*
exit /b %errorlevel%
