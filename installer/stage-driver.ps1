<#
.SYNOPSIS
    Collects a built and signed driver into installer\payload\driver.

.DESCRIPTION
    The installer offers the driver component only when this payload is there,
    so this script is what decides whether a given installer can install a
    driver at all.

    It deliberately refuses to stage an unsigned package. An installer carrying
    a .sys with no catalogue would get all the way to pnputil before failing,
    on the user's machine rather than on the machine that built it.

    Everything staged is copied, not linked: the payload directory has to stand
    on its own, because that is what Inno Setup reads.

.PARAMETER Configuration
    Which driver build to take. Release unless told otherwise.

.PARAMETER Clean
    Empty the payload directory first, so that a stale file from an earlier
    layout cannot survive into the installer.
#>

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'

$here       = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
$root       = Resolve-Path (Join-Path $here '..')
$driverRoot = Join-Path $root 'driver'
$buildDir   = Join-Path $driverRoot "build\$Configuration"
$certDir    = Join-Path $driverRoot 'build\cert'
$payload    = Join-Path $here 'payload\driver'

# The package. Every one of these is required: the catalogue is what PnP checks
# at install time, and the certificate is what makes the catalogue verifiable
# on a machine that has never seen this project before.
$package = @(
    @{ Path = Join-Path $buildDir 'RadioVoiceAudio.sys'; Why = 'build the driver: driver\build-driver.cmd' }
    @{ Path = Join-Path $buildDir 'RadioVoiceAudio.inf'; Why = 'build the driver: driver\build-driver.cmd' }
    @{ Path = Join-Path $buildDir 'RadioVoiceAudio.cat'; Why = 'sign the driver: driver\tools\sign.ps1' }
    @{ Path = Join-Path $certDir  'RadioVoiceTest.cer';  Why = 'create the certificate: driver\tools\make-test-cert.ps1' }
)

# The scripts that install it on the target machine. RootDevice.ps1 is what
# replaces devcon there, and Common.ps1 is dot-sourced by all of them.
$tools = @(
    'Common.ps1'
    'RootDevice.ps1'
    'install.ps1'
    'uninstall.ps1'
    'trust-cert.ps1'
)

$missing = @()
foreach ($item in $package) {
    if (-not (Test-Path $item.Path)) {
        $missing += "  $(Split-Path -Leaf $item.Path)  ->  $($item.Why)"
    }
}

if ($missing) {
    Write-Host ""
    Write-Host "Driver payload is incomplete; the installer will be built without it." -ForegroundColor Yellow
    Write-Host ""
    $missing | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
    Write-Host ""
    # Not an error. An application-only installer is a perfectly good thing to
    # produce, and it is what CI produces on every push.
    exit 0
}

if ($Clean -and (Test-Path $payload)) {
    Remove-Item $payload -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $payload            | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $payload 'tools') | Out-Null

foreach ($item in $package) {
    Copy-Item $item.Path $payload -Force
    Write-Host "  staged  $(Split-Path -Leaf $item.Path)"
}

foreach ($tool in $tools) {
    $source = Join-Path $driverRoot "tools\$tool"
    if (-not (Test-Path $source)) { throw "'$source' not found." }
    Copy-Item $source (Join-Path $payload 'tools') -Force
    Write-Host "  staged  tools\$tool"
}

Write-Host ""
Write-Host "Driver payload ready: $payload" -ForegroundColor Green
