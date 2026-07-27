<#
.SYNOPSIS
    Installs the driver and creates the virtual device.

.DESCRIPTION
    Two separate steps that are easy to confuse:

      1. `pnputil /add-driver` puts the package into the driver store. That
         makes the driver *available*; it does not create anything.

      2. The device itself has to be created. There is no hardware to enumerate
         it, so it is a root-enumerated device and something has to say "create
         a node with hardware ID root\RadioVoiceAudio". That is what devcon
         does here.

    Skipping step 2 is the usual reason a virtual driver installs "successfully"
    and no endpoint ever appears.

.PARAMETER Path
    Directory holding the signed driver package.
#>

[CmdletBinding()]
param(
    [string] $Path = (Join-Path $PSScriptRoot '..\build\Release')
)

. (Join-Path $PSScriptRoot 'Common.ps1')

Assert-Elevated 'Installing a driver'

$Path = (Resolve-Path $Path).Path
$inf  = Join-Path $Path 'RadioVoiceAudio.inf'
$cat  = Join-Path $Path 'RadioVoiceAudio.cat'

if (-not (Test-Path $inf)) { throw "'$inf' not found. Build the driver first." }
if (-not (Test-Path $cat)) {
    throw "'$cat' not found. Run tools\sign.ps1 - an unsigned package will be rejected."
}

# Warn early rather than letting the install fail with an opaque code.
$testSigning = (bcdedit /enum '{current}' | Select-String 'testsigning\s+Yes')
if (-not $testSigning) {
    Write-Warning @"
Test signing does not appear to be enabled.

A test-signed driver will not load without it. Enable it and reboot:

    bcdedit /set testsigning on

If that command reports it was set but the setting does not stick, Secure Boot
is still on; turn it off in the firmware first.
"@
}

Write-Host "Adding the driver package to the driver store..."
pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
    throw "pnputil failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Creating the root-enumerated device..."

$devcon = Find-KitTool 'devcon.exe'
Write-Host "  devcon: $devcon"

& $devcon install $inf 'root\RadioVoiceAudio'
if ($LASTEXITCODE -ne 0) {
    throw @"
devcon failed with exit code $LASTEXITCODE.

If the package installed but the device could not be created, the equivalent
manual route is Device Manager -> Action -> Add legacy hardware -> Install the
hardware that I manually select -> Sound, video and game controllers -> Have
Disk -> point at RadioVoiceAudio.inf.
"@
}

Write-Host ""
Write-Host "Installed. Expected endpoints:" -ForegroundColor Green
Write-Host "  Playback  : RadioVoice Output"
Write-Host "  Recording : RadioVoice Microphone"
Write-Host ""
Write-Host "Verify with:  Get-PnpDevice -FriendlyName '*RadioVoice*'"
