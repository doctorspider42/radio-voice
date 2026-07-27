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

    # Which store holds the key decides whether signtool needs /sm, and the
    # machine store is tried first for a reason: install-driver.cmd elevates
    # itself, and an elevated process has a different CurrentUser store from the
    # account that created the certificate. A key in Cert:\CurrentUser\My is
    # therefore invisible to precisely the run that has to sign unattended.
    if (Test-Path "Cert:\LocalMachine\My\$thumb") {
        $signArguments = @('/sm', '/sha1', $thumb)
        Write-Host "Signing with certificate $thumb from LocalMachine\My"
    } elseif (Test-Path "Cert:\CurrentUser\My\$thumb") {
        $signArguments = @('/sha1', $thumb)
        Write-Host "Signing with certificate $thumb from CurrentUser\My"
    } else {
        throw @"
Certificate $thumb is recorded but is in neither Cert:\LocalMachine\My nor
Cert:\CurrentUser\My.

The usual cause is a certificate created without elevation: it lives in the
personal store of that account, and this run is a different one. Re-run
tools\make-test-cert.ps1 from an elevated prompt to put it in the machine store.
"@
    }
}

$inf2cat  = Find-KitTool 'Inf2Cat.exe'
$signtool = Find-KitTool 'signtool.exe'

Write-Host "Inf2Cat  : $inf2cat"
Write-Host "signtool : $signtool"
Write-Host ""

#-----------------------------------------------------------------------------
# Order matters, and getting it wrong fails much later with an opaque error.
#
#   1. Sign the .sys.  Embedding a signature changes the file.
#   2. Generate the .cat.  Inf2Cat hashes the files the INF lists, so it has to
#      run against the *signed* .sys.
#   3. Sign the .cat.
#
# Cataloguing first and signing afterwards leaves the catalogue holding the hash
# of a file that no longer exists in that form. Everything appears to succeed,
# and pnputil then rejects the package with SPAPI_E_DRIVER_STORE_ADD_FAILED
# (0xE0000247), which says nothing about hashes.
#-----------------------------------------------------------------------------

$sys = Join-Path $Path 'RadioVoiceAudio.sys'
$cat = Join-Path $Path 'RadioVoiceAudio.cat'

Write-Host "Signing RadioVoiceAudio.sys..."

# No /t timestamp: a test signature is only meaningful while the test
# certificate is trusted on this machine anyway, and a timestamp server would
# make the build depend on the network.
& $signtool sign /fd SHA256 @signArguments $sys
if ($LASTEXITCODE -ne 0) {
    throw "signtool failed on $sys"
}

# A catalogue left over from a previous run would be hashed into nothing useful
# and could mask the regeneration failing.
Remove-Item $cat -Force -ErrorAction SilentlyContinue

Write-Host "Generating the catalogue..."

# Inf2Cat insists on an absolute path without a trailing separator.
& $inf2cat /driver:"$Path" /os:$OsList
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed. The usual cause is a mismatch between the INF's DriverVer/CatalogFile and the files present."
}

if (-not (Test-Path $cat)) {
    throw "Inf2Cat reported success but produced no catalogue."
}

Write-Host "Signing RadioVoiceAudio.cat..."
& $signtool sign /fd SHA256 @signArguments $cat
if ($LASTEXITCODE -ne 0) {
    throw "signtool failed on $cat"
}

$targets = @($sys, $cat)

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

# Does the catalogue actually vouch for the .sys as it now stands? This is the
# check that catches a stale catalogue at the point where it can be explained,
# rather than leaving pnputil to reject the package later with a code that says
# nothing about hashes.
$ErrorActionPreference = 'Continue'
$catalogueCheck = & $signtool verify /pa /c $cat $sys 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = 'Stop'

if ($catalogueCheck -match 'not found in the catalog|hash of the file') {
    throw @"
The catalogue does not match RadioVoiceAudio.sys.

That means the two were produced out of order - the catalogue records a hash of
the file as it was before it got its signature. Re-run this script; it signs the
.sys first and only then builds the catalogue.
"@
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
