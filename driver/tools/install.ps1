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
    [string] $Path
)

$toolsRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
. (Join-Path $toolsRoot 'Common.ps1')

Assert-Elevated 'Installing a driver'

if (-not $Path) {
    $Path = Join-Path $toolsRoot '..\build\Release'
}
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
$pnputilResult = $LASTEXITCODE

# Two non-zero codes mean success here.
#
# 259 is ERROR_NO_MORE_ITEMS, returned when the package is already present and
# unchanged.
#
# 3010 is ERROR_SUCCESS_REBOOT_REQUIRED. The package went into the store; what
# it cannot do is unload the copy already running, so the swap finishes on the
# next boot. Treating it as a failure sends the reader hunting for a problem
# that is not there - and worse, it is exactly the code a *re-install* over a
# live driver produces, which is the common case while developing one.
$script:rebootRequired = ($pnputilResult -eq 3010)

if ($pnputilResult -ne 0 -and $pnputilResult -ne 259 -and $pnputilResult -ne 3010) {
    # The SPAPI codes are reported as signed integers and mean nothing on
    # sight, so the ones that actually happen here get named.
    $explanation = switch ($pnputilResult) {
        -536870329 {   # 0xE0000247 SPAPI_E_DRIVER_STORE_ADD_FAILED
@"
SPAPI_E_DRIVER_STORE_ADD_FAILED - Windows rejected the package.

In order of likelihood:

  * The catalogue does not match the .sys. This happens when the catalogue is
    generated before the driver is signed, because embedding a signature
    changes the file. tools\sign.ps1 does them in the right order; re-run it.

  * The test certificate is not in LocalMachine\Root and TrustedPublisher.
    Run tools\make-test-cert.ps1 from an elevated prompt.

  * Test signing is not actually active. The watermark in the bottom-right
    corner of the desktop is the reliable indicator, not what bcdedit printed
    when it was set.
"@
        }
        -536870333 {   # 0xE0000243 SPAPI_E_NO_CATALOG_FOR_OEM_INF
            "SPAPI_E_NO_CATALOG_FOR_OEM_INF - the package has no catalogue. Run tools\sign.ps1."
        }
        -536870334 {   # 0xE0000242
            "The package failed signature verification. Check that the test certificate is trusted."
        }
        default { "" }
    }

    if ($explanation) {
        throw "pnputil failed ($pnputilResult).`n`n$explanation"
    }
    throw "pnputil failed with exit code $pnputilResult"
}

# 'install' creates a new device node every time it is run. On a machine that
# already has one, that means a second, duplicate cable rather than the updated
# driver - so an existing device is updated in place instead.
$existing = Get-PnpDevice -FriendlyName '*RadioVoice*' -ErrorAction SilentlyContinue

# devcon is the obvious tool and the one the WDK documents, but it ships only
# with the WDK - which a machine running the installer does not have. Where it
# is present it is used unchanged; where it is not, the same SetupAPI calls are
# made directly. Neither path is a fallback in the sense of being worse: they
# do the same four things.
$devcon = $null
try { $devcon = Find-KitTool 'devcon.exe' } catch { }

Write-Host ""

if ($devcon) {
    Write-Host "  devcon: $devcon"

    if ($existing) {
        Write-Host "Updating the existing device..."
        & $devcon update $inf 'root\RadioVoiceAudio'
    } else {
        Write-Host "Creating the root-enumerated device..."
        & $devcon install $inf 'root\RadioVoiceAudio'
    }

    # devcon returns 1 to say the work succeeded but needs a reboot, which is
    # the ordinary outcome when the driver being replaced is currently loaded.
    if ($LASTEXITCODE -eq 1) {
        $script:rebootRequired = $true
    } elseif ($LASTEXITCODE -ne 0) {
        throw @"
devcon failed with exit code $LASTEXITCODE.

If the package installed but the device could not be created, the equivalent
manual route is Device Manager -> Action -> Add legacy hardware -> Install the
hardware that I manually select -> Sound, video and game controllers -> Have
Disk -> point at RadioVoiceAudio.inf.
"@
    }
} else {
    Write-Host "  devcon: not present (no WDK); using SetupAPI directly"
    . (Join-Path $toolsRoot 'RootDevice.ps1')

    try {
        if ($existing) {
            Write-Host "Updating the existing device..."
            $needsReboot = Install-RootDevice -InfPath $inf -Update
        } else {
            Write-Host "Creating the root-enumerated device..."
            $needsReboot = Install-RootDevice -InfPath $inf
        }
    } catch {
        throw @"
Could not create the device: $($_.Exception.Message)

If the package installed but the device could not be created, the equivalent
manual route is Device Manager -> Action -> Add legacy hardware -> Install the
hardware that I manually select -> Sound, video and game controllers -> Have
Disk -> point at RadioVoiceAudio.inf.
"@
    }

    if ($needsReboot) { $script:rebootRequired = $true }
}

Write-Host ""
Write-Host "Installed. Expected endpoints:" -ForegroundColor Green
Write-Host "  Playback  : RadioVoice Output"
Write-Host "  Recording : RadioVoice Microphone"

if ($script:rebootRequired) {
    Write-Host ""
    Write-Host "A reboot is required to finish." -ForegroundColor Yellow
    Write-Host @"
  The new package is in the driver store, but the previous build is still
  loaded and Windows will not unload it while the device exists. Until the
  machine restarts, the endpoints are being served by the OLD binary - so
  testing now measures the previous build, not this one.

  To skip the reboot next time, remove the device first so nothing is holding
  the driver, then install into the gap:

      uninstall-driver.cmd
      install-driver.cmd
"@
}

Write-Host ""
Write-Host "Verify with:  Get-PnpDevice -FriendlyName '*RadioVoice*'"
