<#
.SYNOPSIS
    Builds, signs and stages the driver so that the installer can carry it.

.DESCRIPTION
    What `make-installer.cmd with-driver` runs. Four steps that have to happen
    in this order, and that are easy to get half-right by hand:

      1. driver\build.ps1                 the .sys
      2. driver\tools\make-test-cert.ps1  a certificate, if there is not one
      3. driver\tools\sign.ps1            the .sys, then the catalogue, then it
      4. installer\stage-driver.ps1       into installer\payload\driver

    Elevation is required, and not incidentally: the signing key lives in
    Cert:\LocalMachine\My, because that is the only store an elevated installer
    can reach. signtool cannot open it from an ordinary prompt, and reports "No
    certificates were found that met all the given criteria" - which reads as a
    missing certificate rather than an unreadable one. So this script elevates
    itself rather than letting anyone meet that message.

    ON A MACHINE WITH NO CERTIFICATE YET, step 2 creates one and adds it to
    LocalMachine\Root and LocalMachine\TrustedPublisher. That is the machine
    trusting anything signed with that key from then on. It is the same thing
    install-driver.cmd does, and the same thing the installer's warning page
    describes - but it is worth knowing that this script does it too, rather
    than discovering it afterwards.

.PARAMETER Configuration
    Which driver build to produce and stage. Release unless told otherwise.
#>

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
$root = (Resolve-Path (Join-Path $here '..')).Path

. (Join-Path $root 'driver\tools\Common.ps1')

#-----------------------------------------------------------------------------
# Elevation
#
# Relaunched rather than refused, so that `make-installer.cmd with-driver` is
# genuinely one command. The child's output goes to a file and is replayed
# here, because the elevated console is a different window that closes with it.
#-----------------------------------------------------------------------------
if (-not (Test-Elevated)) {
    $log = Join-Path $root 'driver\build\payload-build.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
    Remove-Item $log -ErrorAction SilentlyContinue

    Write-Host "Signing a driver needs administrator rights - approve the UAC prompt."

    try {
        # -PassThru and then WaitForExit(), rather than -Wait.
        #
        # -Wait does not simply wait for the process: it waits for the whole
        # tree it spawns, and this one shells out to cl, link, signtool and
        # Inf2Cat. The elevated run finishes, writes its last line, exits - and
        # the caller sits there forever. Waiting on the one process handle is
        # what was meant all along.
        $child = Start-Process powershell -Verb RunAs -PassThru -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
            "& '$PSCommandPath' -Configuration $Configuration *> '$log'")
        $child.WaitForExit()
    } catch {
        throw @"
The elevation prompt was dismissed, so the driver was not built.

The installer will be built without it - which works, it simply will not offer
the driver. To include it, run this again and accept the prompt.
"@
    }

    if (Test-Path $log) { Get-Content $log }
    exit $child.ExitCode
}

$buildDir = Join-Path $root "driver\build\$Configuration"

Write-Host ""
Write-Host "[1/4] Building the driver..." -ForegroundColor Cyan
& (Join-Path $root 'driver\build.ps1') -Configuration $Configuration

Write-Host ""
Write-Host "[2/4] Certificate..." -ForegroundColor Cyan
# -IfMissing asks whether the key is reachable, not whether a file records one,
# so a certificate that exists is reused rather than replaced.
& (Join-Path $root 'driver\tools\make-test-cert.ps1') -IfMissing

Write-Host ""
Write-Host "[3/4] Signing..." -ForegroundColor Cyan
& (Join-Path $root 'driver\tools\sign.ps1') -Path $buildDir

Write-Host ""
Write-Host "[4/4] Staging..." -ForegroundColor Cyan
& (Join-Path $here 'stage-driver.ps1') -Configuration $Configuration -Clean

Write-Host ""
Write-Host "Driver payload ready - the installer will offer the driver." -ForegroundColor Green
