<#
.SYNOPSIS
    Trusts the certificate a prebuilt driver package was signed with.

.DESCRIPTION
    make-test-cert.ps1 creates a certificate and trusts it on the same machine.
    This does only the second half, for the machine that receives an already
    signed package - which is the case the installer is in.

    Two stores, for two different reasons:

      Root             makes the signature verifiable at all. Without it the
                       package fails verification and pnputil rejects it.

      TrustedPublisher suppresses the "Would you like to install this device
                       software?" prompt. Without it the install still works,
                       it just stops and waits for a click.

    WHAT THIS ACTUALLY MEANS. A certificate in the machine's Root store is a
    certificate the machine believes. Anything signed by whoever holds its
    private key is trusted from then on - not only this driver. That is a real
    reduction in the machine's security, and it is the reason the installer
    asks before doing it rather than doing it quietly.

    To undo, from an elevated prompt:

        Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
            Where-Object { $_.Subject -like '*RadioVoice*' } | Remove-Item

.PARAMETER Path
    The .cer to trust.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Path
)

$toolsRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
. (Join-Path $toolsRoot 'Common.ps1')

Assert-Elevated 'Trusting a code-signing certificate'

if (-not (Test-Path $Path)) {
    throw "'$Path' not found. The driver package is incomplete."
}

$Path = (Resolve-Path $Path).Path

$certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $Path
Write-Host "Certificate: $($certificate.Subject)"
Write-Host "Thumbprint : $($certificate.Thumbprint)"
Write-Host ""

foreach ($storeName in @('Root', 'TrustedPublisher')) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        $storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    try {
        # Adding a certificate that is already there is a no-op rather than an
        # error, but saying so is worth more than a silent second pass.
        $already = $store.Certificates | Where-Object { $_.Thumbprint -eq $certificate.Thumbprint }
        if ($already) {
            Write-Host "  LocalMachine\$storeName - already trusted"
        } else {
            $store.Add($certificate)
            Write-Host "  LocalMachine\$storeName - added"
        }
    } finally {
        $store.Close()
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
