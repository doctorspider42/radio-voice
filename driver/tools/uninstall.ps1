<#
.SYNOPSIS
    Removes the virtual device and deletes the driver package.

.DESCRIPTION
    The reverse of install.ps1, in the reverse order: the device node goes
    first, because a package still bound to a live device cannot be deleted
    from the store.
#>

[CmdletBinding()]
param()

$toolsRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
. (Join-Path $toolsRoot 'Common.ps1')

Assert-Elevated 'Removing a driver'

Write-Host "Removing the device..."

# devcon ships only with the WDK, which the machine running the installer does
# not have. Remove-PnpDevice is in the box on every version this driver targets
# and does the same thing, so devcon is used where it exists and not missed
# where it does not.
$devcon = $null
try { $devcon = Find-KitTool 'devcon.exe' } catch { }

if ($devcon) {
    & $devcon remove 'root\RadioVoiceAudio'
} else {
    $devices = Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'ROOT\MEDIA\*' -and
                              $_.FriendlyName -like '*RadioVoice*' }

    if (-not $devices) {
        Write-Host "  no RadioVoice device found."
    } else {
        foreach ($device in $devices) {
            Write-Host "  removing $($device.InstanceId)"
            # -Confirm:$false because this runs unattended from the uninstaller,
            # where a prompt nobody can see is a hang.
            Remove-PnpDevice -InstanceId $device.InstanceId -Confirm:$false `
                             -ErrorAction Stop
        }
    }
}

Write-Host ""
Write-Host "Deleting the driver package from the store..."

# Published INFs are renamed to oemNN.inf in the store, so the original file
# name is no help; the package has to be found by its original name instead.
$packages = pnputil /enum-drivers
$current = $null
$found = @()

foreach ($line in $packages) {
    if ($line -match '^Published Name\s*:\s*(oem\d+\.inf)') {
        $current = $Matches[1]
    } elseif ($line -match 'RadioVoiceAudio\.inf' -and $current) {
        $found += $current
        $current = $null
    }
}

if (-not $found) {
    Write-Host "  no RadioVoiceAudio package found in the driver store."
} else {
    foreach ($package in $found) {
        Write-Host "  deleting $package"
        pnputil /delete-driver $package /uninstall /force
    }
}

Write-Host ""
Write-Host "Removed." -ForegroundColor Green
Write-Host "To leave test-signing mode as well (elevated, then reboot):"
Write-Host "    bcdedit /set testsigning off"
