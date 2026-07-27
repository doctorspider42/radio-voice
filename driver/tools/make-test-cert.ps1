<#
.SYNOPSIS
    Creates a local test-signing certificate and trusts it on this machine.

.DESCRIPTION
    Windows will not load a 64-bit kernel driver that is not signed. A
    production driver carries a signature chaining to a Microsoft cross-
    certificate, obtained by submitting the driver to the Hardware Dev Center
    with an EV code-signing certificate.

    For local development the alternative is test signing: the machine is put
    into a mode where it accepts any driver signed by a certificate in its own
    trusted stores. That mode is a deliberate reduction in the machine's
    security posture - see README.md before enabling it.

    This script does three things:

      1. Creates a self-signed code-signing certificate.
      2. Exports it as a .pfx (for signing) and a .cer (for trusting).
      3. Installs the .cer into the machine's Trusted Root and Trusted
         Publishers stores, which is what makes Windows accept signatures
         made with it.

    Step 3 needs elevation; steps 1 and 2 do not.

.PARAMETER Subject
    Certificate subject. Anything is fine; it appears in the driver's signature.

.PARAMETER Password
    Password protecting the exported .pfx. Prompted for if omitted.

.PARAMETER OutputDirectory
    Where to write RadioVoiceTest.pfx and RadioVoiceTest.cer.
#>

[CmdletBinding()]
param(
    [string] $Subject = 'CN=RadioVoice Test Signing',
    [securestring] $Password,
    [string] $OutputDirectory = (Join-Path $PSScriptRoot '..\build\cert')
)

. (Join-Path $PSScriptRoot 'Common.ps1')

if (-not $Password) {
    $Password = Read-Host -AsSecureString "Password for the exported .pfx"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path

$pfx = Join-Path $OutputDirectory 'RadioVoiceTest.pfx'
$cer = Join-Path $OutputDirectory 'RadioVoiceTest.cer'

#-----------------------------------------------------------------------------
# 1. Create
#-----------------------------------------------------------------------------

Write-Host "Creating a code-signing certificate for '$Subject'..."

# The TextExtension pins the Enhanced Key Usage to Code Signing (OID
# 1.3.6.1.5.5.7.3.3). Without it the certificate is general-purpose and
# signtool will refuse to use it for a driver.
$certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyExportPolicy Exportable `
    -KeyUsage DigitalSignature `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(5) `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')

Write-Host "  thumbprint: $($certificate.Thumbprint)"

#-----------------------------------------------------------------------------
# 2. Export
#-----------------------------------------------------------------------------

Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $Password | Out-Null
Export-Certificate   -Cert $certificate -FilePath $cer | Out-Null

Write-Host "  pfx: $pfx"
Write-Host "  cer: $cer"

#-----------------------------------------------------------------------------
# 3. Trust
#-----------------------------------------------------------------------------

if (Test-Elevated) {
    # Root makes the signature chain verifiable; TrustedPublisher stops the
    # "Would you like to install this device software?" prompt. Both are needed
    # for an unattended install to work.
    foreach ($store in @('Root', 'TrustedPublisher')) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        Write-Host "  trusted in LocalMachine\$store"
    }

    Write-Host ""
    Write-Host "Certificate created and trusted." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Warning @"
The certificate was created and exported, but NOT trusted: that needs elevation.

Run this from an elevated PowerShell to finish:

    Import-Certificate -FilePath "$cer" -CertStoreLocation Cert:\LocalMachine\Root
    Import-Certificate -FilePath "$cer" -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
"@
}

Write-Host ""
Write-Host "Next: enable test signing (elevated, then reboot):"
Write-Host "    bcdedit /set testsigning on"
