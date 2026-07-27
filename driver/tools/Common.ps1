<#
    Shared helpers for the driver tooling.

    Dot-source this, do not run it.
#>

$ErrorActionPreference = 'Stop'

<#
    Directory of the running script.

    $PSScriptRoot is unreliable inside a param() block: launched as
    `powershell -File .\tools\x.ps1` with a relative path, Windows PowerShell
    leaves it empty there while populating it correctly in the body. Defaults
    that depend on it therefore have to be resolved after the parameters are
    bound, which is what this exists for.
#>
function Get-ToolsRoot {
    if ($PSScriptRoot) { return $PSScriptRoot }
    if ($PSCommandPath) { return (Split-Path -Parent $PSCommandPath) }
    return (Get-Location).Path
}

function Get-KitRoot {
    $root = "${env:ProgramFiles(x86)}\Windows Kits\10"
    if (-not (Test-Path $root)) {
        throw "Windows Kits\10 not found. Install the Windows SDK and the WDK."
    }
    return $root
}

<#
    Finds a kit tool, preferring the x64 build.

    The kit ships arm64, x86 and x64 copies of most tools side by side, and a
    naive recursive search finds whichever sorts first - usually arm64, which
    will not run here. The preference order is explicit for that reason.
#>
function Find-KitTool {
    param([Parameter(Mandatory)][string] $Name)

    $root = Get-KitRoot
    $matches = Get-ChildItem $root -Recurse -Filter $Name -File -ErrorAction SilentlyContinue

    if (-not $matches) {
        throw "'$Name' not found under '$root'."
    }

    foreach ($architecture in @('\x64\', '\x86\')) {
        $hit = $matches | Where-Object { $_.FullName -like "*$architecture*" } |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }

    # Some tools (signtool under the App Certification Kit) sit outside the
    # per-architecture layout entirely.
    return ($matches | Select-Object -First 1).FullName
}

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Elevated {
    param([string] $What = 'This operation')

    if (-not (Test-Elevated)) {
        throw "$What requires an elevated PowerShell. Right-click PowerShell and choose 'Run as administrator'."
    }
}
