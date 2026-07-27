<#
.SYNOPSIS
    Builds the driver catalogue and signs both it and the .sys.

.DESCRIPTION
    A driver package needs two signatures, and they are not interchangeable:

      * The .sys is signed so the kernel will load the image. Without this the
        driver fails to start with STATUS_INVALID_IMAGE_HASH (code 577).

      * The .cat is signed so PnP will accept the package during installation.
        The catalogue is a manifest of hashes of every file the INF references;
        Inf2Cat generates it from the INF. Without this the install fails or
        prompts about an unsigned package, depending on policy.

    Signing only one of the two produces a confusing half-working state, so
    this script always does both.

.PARAMETER Path
    Directory holding RadioVoiceAudio.sys and RadioVoiceAudio.inf.

.PARAMETER Pfx
    Sign using a .pfx file instead of the certificate store. Only needed when
    the key is not on this machine - see -ExportPfx in make-test-cert.ps1.

.PARAMETER Password
    Password for the .pfx. Only used with -Pfx; prompted for if omitted.

.PARAMETER OsList
    Inf2Cat target list. 10_X64 covers Windows 10 and 11 on x64.
#>

[CmdletBinding()]
param(
    [string] $Path,
    [string] $Pfx,
    [securestring] $Password,
    [string] $OsList = '10_X64'
)

$toolsRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
. (Join-Path $toolsRoot 'Common.ps1')

if (-not $Path) {
    $Path = Join-Path $toolsRoot '..\build\Release'
}
$Path = (Resolve-Path $Path).Path

foreach ($required in @('RadioVoiceAudio.sys', 'RadioVoiceAudio.inf')) {
    if (-not (Test-Path (Join-Path $Path $required))) {
        throw "'$required' not found in '$Path'. Build the driver first."
    }
}

#-----------------------------------------------------------------------------
# How to sign
#
# By default the private key stays in the certificate store and signtool is
# pointed at it by thumbprint. That avoids inventing a password purely to
# protect a file that then has to sit next to the thing it protects.
#-----------------------------------------------------------------------------

$signArguments = @()

if ($Pfx) {
    if (-not (Test-Path $Pfx)) {
        throw "'$Pfx' not found."
    }
    if (-not $Password) {
        $Password = Read-Host -AsSecureString "Password for $(Split-Path $Pfx -Leaf)"
    }
    $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
        [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password))

    $signArguments = @('/f', $Pfx, '/p', $plainPassword)
    Write-Host "Signing with: $Pfx"
} else {
    $thumbprintFile = Join-Path $toolsRoot '..\build\cert\thumbprint.txt'
    if (-not (Test-Path $thumbprintFile)) {
        throw "No certificate found. Run tools\make-test-cert.ps1 first, or pass -Pfx."
    }

    $thumb = (Get-Content $thumbprintFile -Raw).Trim()
    if (-not (Test-Path "Cert:\CurrentUser\My\$thumb")) {
        throw @"
Certificate $thumb is recorded but no longer in Cert:\CurrentUser\My.
Re-run tools\make-test-cert.ps1.
"@
    }

    $signArguments = @('/sha1', $thumb)
    Write-Host "Signing with certificate $thumb from the store"
}

$inf2cat  = Find-KitTool 'Inf2Cat.exe'
$signtool = Find-KitTool 'signtool.exe'

Write-Host "Inf2Cat  : $inf2cat"
Write-Host "signtool : $signtool"
Write-Host ""

#-----------------------------------------------------------------------------
# Catalogue
#-----------------------------------------------------------------------------

Write-Host "Generating the catalogue..."

# Inf2Cat insists on a path without a trailing separator and rejects relative
# ones outright.
& $inf2cat /driver:"$Path" /os:$OsList /verbose
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed. The usual cause is a mismatch between the INF's DriverVer/CatalogFile and the files present."
}

#-----------------------------------------------------------------------------
# Signatures
#-----------------------------------------------------------------------------

$targets = @(
    (Join-Path $Path 'RadioVoiceAudio.sys')
    (Join-Path $Path 'RadioVoiceAudio.cat')
)

foreach ($target in $targets) {
    Write-Host "Signing $(Split-Path $target -Leaf)..."

    # No /t timestamp: a test signature is only meaningful while the test
    # certificate is trusted on this machine anyway, and a timestamp server
    # would make the build depend on the network.
    & $signtool sign /fd SHA256 @signArguments $target
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed on $target"
    }
}

Write-Host ""
Write-Host "Verifying..."

$untrusted = $false

# Verification is diagnostic, not a gate: an untrusted test root is the normal
# state until the certificate is imported, and signtool reports it on stderr.
# With ErrorActionPreference at Stop that stderr would abort a script that has
# in fact already done its job.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    foreach ($target in $targets) {
        # /pa selects the "default authenticode" policy, which is what the
        # kernel applies to a test-signed driver. Verification against the
        # driver policy (/kp) would fail by design - that policy requires a
        # Microsoft-issued signature.
        $output = & $signtool verify /pa /v $target 2>&1 | ForEach-Object { "$_" }

        $output | Select-String -Pattern 'Successfully verified|Issued to' | ForEach-Object {
            Write-Host "  $($_.Line.Trim())"
        }
        if ($output -match 'terminated in a root') {
            $untrusted = $true
        }
    }
} finally {
    $ErrorActionPreference = $previousPreference
}

Write-Host ""
Write-Host "Signed: $Path" -ForegroundColor Green

if ($untrusted) {
    # Worth spelling out: the signature itself is fine, and this exact failure
    # is what an untrusted test certificate looks like. Reading it as a signing
    # error sends you looking in the wrong place.
    $cer = (Resolve-Path (Join-Path $toolsRoot '..\build\cert\RadioVoiceTest.cer') `
                -ErrorAction SilentlyContinue)

    Write-Warning @"
Verification reports an untrusted root. The files ARE signed correctly - this
means the test certificate has not been added to the machine's trusted stores
yet, which needs elevation:

    Import-Certificate -FilePath "$cer" -CertStoreLocation Cert:\LocalMachine\Root
    Import-Certificate -FilePath "$cer" -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

Re-running tools\make-test-cert.ps1 from an elevated prompt does the same thing.
"@
}

# Explicit, because `powershell -File` otherwise propagates the exit code of the
# last native process - which is signtool's verify, and that fails by design
# while the certificate is untrusted. Reaching this line means signing worked.
exit 0
