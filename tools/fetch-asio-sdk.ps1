<#
.SYNOPSIS
    Downloads the Steinberg ASIO SDK into third_party\asiosdk.

.DESCRIPTION
    ASIO support cannot be shipped ready-built: the SDK's licence forbids
    redistributing it, which is why it is neither in this repository nor fetched
    by CMake the way Dear ImGui and the VST3 SDK are.

    Running this script means accepting Steinberg's ASIO SDK Licensing
    Agreement, a copy of which lands in third_party\asiosdk alongside the code.
    Read it if the terms matter to you - in particular, distributing a binary
    built against it carries obligations.

    Once the SDK is in place CMake finds it on its own; no flags are needed.
#>

[CmdletBinding()]
param(
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$toolsRoot = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $PSCommandPath }
$repoRoot  = Split-Path -Parent $toolsRoot
$target    = Join-Path $repoRoot 'third_party\asiosdk'
$downloadUrl = 'https://www.steinberg.net/asiosdk'

if ((Test-Path (Join-Path $target 'common\asio.h')) -and -not $Force) {
    Write-Host "ASIO SDK already present at $target"
    Write-Host "Use -Force to replace it."
    exit 0
}

Write-Host ""
Write-Host "About to download the Steinberg ASIO SDK from:"
Write-Host "    $downloadUrl"
Write-Host ""
Write-Host "Doing so accepts Steinberg's ASIO SDK Licensing Agreement."
Write-Host ""

$temp = Join-Path ([IO.Path]::GetTempPath()) "asiosdk-$(Get-Random).zip"
$extract = Join-Path ([IO.Path]::GetTempPath()) "asiosdk-$(Get-Random)"

try {
    Write-Host "Downloading..."
    Invoke-WebRequest -Uri $downloadUrl -OutFile $temp -UseBasicParsing

    # Steinberg has previously served an HTML consent page from this URL rather
    # than the archive. Checking the magic bytes turns that into a clear message
    # instead of a confusing failure three steps later.
    $magic = [IO.File]::ReadAllBytes($temp)[0..1]
    if ($magic[0] -ne 0x50 -or $magic[1] -ne 0x4B) {
        throw @"
What was downloaded is not a ZIP archive - the site probably served a consent
page instead. Download it manually from $downloadUrl and extract it so that
'$target\common\asio.h' exists.
"@
    }

    Write-Host "Extracting..."
    Expand-Archive -Path $temp -DestinationPath $extract -Force

    # The archive wraps everything in a single versioned directory whose name
    # changes between releases, so it is located rather than assumed.
    $sdkRoot = Get-ChildItem $extract -Recurse -Filter 'asio.h' -File |
               Where-Object { $_.Directory.Name -eq 'common' } |
               Select-Object -First 1
    if (-not $sdkRoot) {
        throw "The archive does not contain common\asio.h - its layout has changed."
    }

    $source = $sdkRoot.Directory.Parent.FullName

    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Move-Item $source $target

    Write-Host ""
    Write-Host "ASIO SDK installed at $target" -ForegroundColor Green
    Write-Host ""
    Write-Host "Rebuild to pick it up:"
    Write-Host "    cmake --preset mingw"
    Write-Host "    cmake --build --preset mingw"
} finally {
    Remove-Item $temp, $extract -Recurse -Force -ErrorAction SilentlyContinue
}
