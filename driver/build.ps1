<#
.SYNOPSIS
    Builds RadioVoiceAudio.sys by driving cl.exe and link.exe directly.

.DESCRIPTION
    A driver .vcxproj needs the WDK's Visual Studio integration, which ships as
    a VSIX and only attaches to a full Visual Studio install - not to Build
    Tools. Calling the compiler and linker with explicit flags removes that
    coupling entirely: all this needs is an MSVC toolset and the WDK headers
    and libraries, wherever they came from.

    Every flag below is one the WDK's own build rules pass; they are spelled
    out here rather than inherited so that what the driver is built with is
    visible and auditable.

.PARAMETER Configuration
    Debug keeps assertions and DbgPrintEx tracing. Release is optimised and
    silent.

.PARAMETER Sign
    Sign the result with the local test certificate. See tools/make-test-cert.ps1.
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug',

    [switch] $Sign
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

#-----------------------------------------------------------------------------
# Locate the MSVC toolset
#-----------------------------------------------------------------------------

function Find-MsvcToolset {
    # Every install is enumerated and probed for an actual toolset directory,
    # rather than trusting `vswhere -latest -requires ...`. On a machine with
    # both a newer Visual Studio that lacks the C++ workload and an older Build
    # Tools that has it, `-latest` selects the former and the `-requires` filter
    # then yields nothing at all - which reads as "no compiler installed" when
    # one is sitting right there.
    $candidates = @()

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $candidates += & $vswhere -all -prerelease -products * -property installationPath
    }

    # Default locations, in case vswhere is missing or an install predates it.
    $candidates += @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools"
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community"
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional"
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise"
    )

    $toolsets = foreach ($install in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $msvcRoot = Join-Path $install 'VC\Tools\MSVC'
        if (Test-Path $msvcRoot) {
            Get-ChildItem $msvcRoot -Directory | Where-Object {
                Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')
            }
        }
    }

    if (-not $toolsets) {
        throw @"
No MSVC x64 toolset found.

Install it with:
  winget install --id Microsoft.VisualStudio.2022.BuildTools --override `
    "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools"
"@
    }

    # Version directories sort correctly as [version], not as strings.
    return ($toolsets | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).FullName
}

#-----------------------------------------------------------------------------
# Locate the WDK
#-----------------------------------------------------------------------------

function Find-Wdk {
    $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
    if (-not (Test-Path $kitRoot)) {
        throw "Windows Kits\10 not found. Install the Windows SDK and the WDK."
    }

    # Pick the newest kit version that actually contains the kernel-mode headers;
    # an SDK-only install has an Include directory but no km subdirectory.
    $candidates = Get-ChildItem (Join-Path $kitRoot 'Include') -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'km\portcls.h') } |
        Sort-Object Name -Descending

    if (-not $candidates) {
        throw "No WDK found: no kit version under '$kitRoot' contains km\portcls.h."
    }

    return [pscustomobject]@{
        Root    = $kitRoot
        Version = $candidates[0].Name
    }
}

$toolset = Find-MsvcToolset
$wdk     = Find-Wdk

$cl   = Join-Path $toolset 'bin\Hostx64\x64\cl.exe'
$link = Join-Path $toolset 'bin\Hostx64\x64\link.exe'

foreach ($tool in @($cl, $link)) {
    if (-not (Test-Path $tool)) { throw "Not found: $tool" }
}

Write-Host "MSVC : $toolset"
Write-Host "WDK  : $($wdk.Root) [$($wdk.Version)]"
Write-Host "Build: $Configuration"
Write-Host ""

#-----------------------------------------------------------------------------
# Paths
#-----------------------------------------------------------------------------

# Kernel-mode include order. The MSVC include directory is deliberately absent:
# km\crt is a complete replacement for it, down to intrin.h and the C++ headers
# that kernel code is allowed to use. Putting MSVC's include first pulls in
# crtdefs.h from the user-mode CRT, which then reaches for corecrt.h in the UCRT
# - a header no kernel driver may link against.
$include = @(
    (Join-Path $wdk.Root "Include\$($wdk.Version)\km")
    (Join-Path $wdk.Root "Include\$($wdk.Version)\km\crt")
    (Join-Path $wdk.Root "Include\$($wdk.Version)\shared")
)

$libPath = @(
    (Join-Path $toolset 'lib\x64')
    (Join-Path $wdk.Root "Lib\$($wdk.Version)\km\x64")
)

# Intermediates live outside the package directory. Inf2Cat catalogues every
# file it finds under the directory it is pointed at, so an obj\ subdirectory
# would end up hashed into the catalogue - inflating it with build droppings and
# tying the signature to files that have nothing to do with the driver.
$outDir = Join-Path $root "build\$Configuration"
$objDir = Join-Path $root "build\obj\$Configuration"
New-Item -ItemType Directory -Force -Path $outDir, $objDir | Out-Null

$sources = Get-ChildItem (Join-Path $root 'src') -Filter '*.cpp' -File

#-----------------------------------------------------------------------------
# Compile
#-----------------------------------------------------------------------------

# /kernel      - kernel-mode C++: no exceptions, no RTTI, restricted language
# /GR-         - RTTI off (implied by /kernel, stated for clarity)
# /Gz          - not used; PortCls interfaces are __stdcall via the SDK macros
# /Zc:wchar_t- - matches how the WDK headers were compiled
# /GS          - stack cookies; pairs with BufferOverflowFastFailK.lib below
$commonFlags = @(
    '/c', '/nologo', '/W4', '/WX-'
    # C4996 fires inside the WDK's own stdunk.h, which still calls the
    # deprecated ExAllocatePoolWithTag. It is not this driver's code to fix,
    # and the noise would bury real warnings.
    '/wd4996'
    '/kernel', '/GR-', '/GS', '/Gy', '/Gm-'
    '/Zc:wchar_t-', '/Zc:forScope', '/Zc:inline'
    '/fp:precise', '/Zp8'
    '/D_WIN64', '/D_AMD64_', '/DAMD64'
    # _KERNEL_MODE is predefined by /kernel; defining it again is an error.
    # Windows 10 2004. That is the floor for ExAllocatePool2, which replaced
    # the deprecated ExAllocatePoolWithTag; targeting anything older would mean
    # using an API Microsoft has marked for removal.
    '/DNTDDI_VERSION=0x0A000008'
    '/D_WIN32_WINNT=0x0A00'
    '/DWINVER=0x0A00'
    '/DPOOL_NX_OPTIN=1'
    '/Zi', '/FS'
)

if ($Configuration -eq 'Debug') {
    $commonFlags += @('/Od', '/DDBG=1', '/DDEPRECATE_DDK_FUNCTIONS=1')
} else {
    $commonFlags += @('/O2', '/Oi', '/DNDEBUG')
}

$includeFlags = $include | ForEach-Object { "/I$_" }

Write-Host "Compiling $($sources.Count) file(s)..."

$objects = @()
foreach ($source in $sources) {
    $obj = Join-Path $objDir ($source.BaseName + '.obj')
    $objects += $obj

    $arguments = $commonFlags + $includeFlags + @(
        "/Fo$obj"
        "/Fd$(Join-Path $objDir 'vc.pdb')"
        $source.FullName
    )

    & $cl @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed: $($source.Name)"
    }
}

#-----------------------------------------------------------------------------
# Link
#-----------------------------------------------------------------------------

# /DRIVER            - kernel driver image; implies NATIVE subsystem and the
#                      DriverEntry entry point
# /NODEFAULTLIB      - the user-mode CRT must not be pulled in
# /INTEGRITYCHECK    - required for a driver that will be loaded on 64-bit
#                      Windows with signature enforcement
# BufferOverflowFastFailK.lib - the /GS runtime for /kernel code
$linkFlags = @(
    '/NOLOGO'
    '/DRIVER'
    '/SUBSYSTEM:NATIVE,10.00'
    '/ENTRY:DriverEntry'
    '/MACHINE:X64'
    '/NODEFAULTLIB'
    '/MANIFEST:NO'
    '/INCREMENTAL:NO'
    '/INTEGRITYCHECK'
    '/DEBUG'
    '/OPT:REF', '/OPT:ICF'
    '/RELEASE'
)

$libFlags = $libPath | ForEach-Object { "/LIBPATH:$_" }

$libraries = @(
    'ntoskrnl.lib'
    'hal.lib'
    'wmilib.lib'
    'portcls.lib'
    'stdunk.lib'
    'ks.lib'
    'ksguid.lib'
    'BufferOverflowFastFailK.lib'
)

$sys = Join-Path $outDir 'RadioVoiceAudio.sys'

Write-Host "Linking..."

& $link @linkFlags @libFlags @objects @libraries `
    "/OUT:$sys" "/PDB:$(Join-Path $objDir 'RadioVoiceAudio.pdb')"

if ($LASTEXITCODE -ne 0) {
    throw "Link failed."
}

Copy-Item (Join-Path $root 'RadioVoiceAudio.inf') $outDir -Force

Write-Host ""
Write-Host "Built: $sys" -ForegroundColor Green

#-----------------------------------------------------------------------------
# Optional signing
#-----------------------------------------------------------------------------

if ($Sign) {
    & (Join-Path $root 'tools\sign.ps1') -Path $outDir
}
