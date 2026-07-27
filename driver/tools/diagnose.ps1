<#
.SYNOPSIS
    Dumps everything worth knowing about an installed RadioVoice driver.

.DESCRIPTION
    Gathers, in one place, the four things that between them say where an
    install went wrong:

      1. the device's PnP state - did the driver load and start at all
      2. the counters the driver recorded for itself - was it ever asked
         anything after starting
      3. the KS interfaces it registered, and whether the INF gave them the
         proxy CLSID that makes them visible to the audio stack
      4. whether any audio endpoint was actually built from them

    Needs no elevation to read.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

function Section($title) {
    Write-Host ""
    Write-Host "=== $title " -NoNewline -ForegroundColor Cyan
    Write-Host ("=" * [math]::Max(0, 60 - $title.Length)) -ForegroundColor Cyan
}

#-----------------------------------------------------------------------------
Section "Device"
#-----------------------------------------------------------------------------

$device = Get-PnpDevice -FriendlyName '*RadioVoice*' -ErrorAction SilentlyContinue
if (-not $device) {
    Write-Host "  not installed" -ForegroundColor Yellow
} else {
    $device | Select-Object Status, Class, FriendlyName, InstanceId, Problem, ProblemDescription |
        Format-List | Out-String | Write-Host
}

$service = Get-Service RadioVoiceAudio -ErrorAction SilentlyContinue
if ($service) {
    Write-Host ("  service: {0}, start {1}" -f $service.Status, $service.StartType)
} else {
    Write-Host "  service: not present" -ForegroundColor Yellow
}

#-----------------------------------------------------------------------------
Section "What the driver recorded"
#-----------------------------------------------------------------------------

$diag = 'HKLM:\SYSTEM\CurrentControlSet\Services\RadioVoiceAudio\Diagnostics'
if (-not (Test-Path $diag)) {
    Write-Host "  no diagnostics key - the running driver predates the instrumented build" -ForegroundColor Yellow
} else {
    $values = Get-Item -LiteralPath $diag
    $names  = $values.GetValueNames() | Sort-Object
    if (-not $names) {
        Write-Host "  key exists but is empty" -ForegroundColor Yellow
    }
    foreach ($n in $names) {
        $v = $values.GetValue($n)
        # Statuses are recorded as their raw NTSTATUS; show the hex too.
        if ($n -like '*_ok') {
            $text = if ($v -eq 1) { 'OK' } else { 'FAILED' }
            Write-Host ("  {0,-32} {1}" -f $n, $text) -ForegroundColor $(if ($v -eq 1) { 'Green' } else { 'Red' })
        } elseif ($v -gt 0x80000000) {
            Write-Host ("  {0,-32} 0x{1:X8}" -f $n, $v)
        } else {
            Write-Host ("  {0,-32} {1}" -f $n, $v)
        }
    }
}

#-----------------------------------------------------------------------------
Section "KS interfaces"
#-----------------------------------------------------------------------------

if ($device) {
    $instanceKey = $device.InstanceId -replace '\\', '#'
    $classesRoot = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses'

    $any = $false
    foreach ($category in (Get-ChildItem -LiteralPath $classesRoot -ErrorAction SilentlyContinue)) {
        foreach ($iface in (Get-ChildItem -LiteralPath $category.PSPath -ErrorAction SilentlyContinue)) {
            if ($iface.PSChildName -notlike "*$instanceKey*") { continue }

            foreach ($ref in (Get-ChildItem -LiteralPath $iface.PSPath -ErrorAction SilentlyContinue)) {
                $parameters = Get-ItemProperty -LiteralPath (Join-Path $ref.PSPath 'Device Parameters') `
                                               -ErrorAction SilentlyContinue
                $any = $true
                Write-Host ("  {0,-38} {1,-18} CLSID={2}" -f `
                    $category.PSChildName, $ref.PSChildName,
                    $(if ($parameters.CLSID) { 'yes' } else { 'MISSING' }))
            }
        }
    }
    if (-not $any) {
        Write-Host "  none registered" -ForegroundColor Red
    }
}

#-----------------------------------------------------------------------------
Section "Audio endpoints"
#-----------------------------------------------------------------------------

$endpoints = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
             Where-Object { $_.FriendlyName -like '*RadioVoice*' }

if ($endpoints) {
    $endpoints | Select-Object Status, FriendlyName | Format-Table -AutoSize | Out-String | Write-Host
} else {
    Write-Host "  none - the audio stack did not build an endpoint from the filters above" -ForegroundColor Red
}

Write-Host ""
