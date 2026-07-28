# RadioVoice Virtual Audio Cable — driver

A kernel driver that creates a pair of audio endpoints wired to each other:

| Endpoint | Appears as | Role |
|---|---|---|
| `RadioVoice Output` | playback device | RadioVoice renders into this |
| `RadioVoice Microphone` | recording device | Discord / OBS / Teams select this |

Everything written to the first comes out of the second. A functional equivalent
of VB-CABLE with no external dependency.

Format: 48 kHz, stereo, 16- or 24-bit PCM.

---

## Requirements

- Windows 10 version 2004 (build 19041) or newer, x64
- Windows SDK and WDK at the same version
- The MSVC x64 toolset

On a clean machine:

```powershell
winget install --id Microsoft.WindowsSDK.10.0.26100 --exact
winget install --id Microsoft.WindowsWDK.10.0.26100 --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override `
  "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools"
```

The SDK and WDK install machine-wide, so expect a UAC prompt.

---

## All at once

From the repository root:

```
install-driver.cmd
```

It elevates itself and runs through the steps in order: test-signing check,
build, certificate, signature, installation. The rest of this file describes
those steps separately, which is what you need when one of them fails.

To remove it: `uninstall-driver.cmd`.

## Building

```
build-driver.cmd            Release
build-driver.cmd debug      with DbgPrintEx tracing
```

or directly:

```powershell
.\build.ps1 -Configuration Release
```

The name is `build-driver.cmd` rather than `build.cmd` so it cannot be confused
with the `build.cmd` in the repository root, which builds the application.

Produces `driver\build\Release\RadioVoiceAudio.sys` and the `.inf`.

`build.ps1` invokes `cl.exe` and `link.exe` directly instead of using a
`.vcxproj`. A driver project needs the WDK's Visual Studio integration, which
installs as a VSIX and attaches only to full Visual Studio, not to Build Tools.
Calling the compiler directly removes that coupling and puts every flag in one
readable file.

**Bump `DriverVer` in the INF whenever the `.sys` changes.** Windows compares
package versions and will keep running the binary it already has if the version
has not moved, which looks exactly like a build that did not take effect.

---

## Signing

64-bit Windows will not load an unsigned kernel driver. There are two routes.

### Production

An EV code-signing certificate, a Microsoft Partner Center account, submission
to the Hardware Dev Center and a Microsoft signature in return. The driver then
loads anywhere, with no changes to the machine's configuration.

### Local (test signing)

Three steps, all reversible.

#### 1. Turn off Secure Boot

In the firmware (UEFI). Without this `bcdedit /set testsigning on` reports
success but the setting does not take effect: Secure Boot blocks changes to the
signing policy.

To check (PowerShell as administrator):

```powershell
Confirm-SecureBootUEFI
```

#### 2. Enable test signing

PowerShell **as administrator**, then reboot:

```powershell
bcdedit /set testsigning on
```

After the reboot a "Test Mode" watermark appears in the bottom-right corner of
the desktop.

> **What this actually means.** The machine starts accepting any kernel driver
> signed by a certificate its own store trusts — and any process with
> administrator rights can add one to that store. One of the layers protecting
> against rootkits stops applying. On a development machine that is a reasonable
> trade; on a machine holding anything valuable, think again. To undo:
> `bcdedit /set testsigning off` and reboot.

#### 3. Certificate and signature

```powershell
cd driver
.\tools\make-test-cert.ps1          # elevated
.\tools\sign.ps1
```

Neither script asks anything.

`make-test-cert.ps1` creates the certificate, leaves the private key in the
certificate store, writes the public `.cer` and the thumbprint, and installs the
`.cer` into `LocalMachine\Root` and `LocalMachine\TrustedPublisher`. The first
store is what makes the signature verifiable; the second suppresses the "Would
you like to install this device software?" prompt.

**Run it elevated.** Elevated, the certificate goes into `Cert:\LocalMachine\My`;
unelevated it lands in the personal store of that account, where the elevated
installer — running as a different account — cannot see it. `sign.ps1` looks in
the machine store first and falls back to the user one.

`sign.ps1` points `signtool` at the key by thumbprint. There is no `.pfx`, so
there is no password to invent and remember — a password would protect a file
sitting next to the thing it protects, which achieves nothing.

A `.pfx` is only useful for moving the certificate to another machine or into
CI, and is optional:

```powershell
.\tools\make-test-cert.ps1 -ExportPfx      # this is what prompts for a password
.\tools\sign.ps1 -Pfx .\build\cert\RadioVoiceTest.pfx
```

`sign.ps1` generates the catalogue (`Inf2Cat`) and signs **both** files:

- the `.sys`, so the kernel will load the image. Without it: code 577,
  `STATUS_INVALID_IMAGE_HASH`.
- the `.cat`, so PnP will accept the package at install time.

**Order matters.** Sign the `.sys` first, then generate the catalogue, then sign
the catalogue. Embedding a signature changes the file, so a catalogue generated
first holds the hash of a file that no longer exists in that form. Everything
appears to succeed and `pnputil` then rejects the package with
`SPAPI_E_DRIVER_STORE_ADD_FAILED` (0xE0000247), which says nothing about hashes.

---

## Installation

```powershell
.\tools\install.ps1     # elevated
```

Two separate steps, easily confused:

1. `pnputil /add-driver` puts the package into the driver store, which makes it
   *available* but creates nothing.
2. The device has to be brought into existence separately. There is no hardware
   to enumerate it, so it is a root-enumerated device and `devcon` must
   explicitly create a node with the ID `root\RadioVoiceAudio`.

Skipping step 2 is the most common reason a virtual driver installs
"successfully" and no endpoint ever appears.

Exit code 3010 from `pnputil` — `ERROR_SUCCESS_REBOOT_REQUIRED` — means the
package reached the store but the copy already running could not be unloaded.
**Until the machine restarts, the endpoints are served by the previous binary**,
so testing then measures the build before the one just installed. To avoid the
reboot while developing, remove the device first and install into the gap:

```
uninstall-driver.cmd
install-driver.cmd
```

To verify:

```powershell
Get-PnpDevice -FriendlyName '*RadioVoice*'
```

Then set **Output** to `RadioVoice Output` in RadioVoice, and the microphone to
`RadioVoice Microphone` in the receiving application.

### Uninstalling

```powershell
.\tools\uninstall.ps1   # elevated
```

---

## How it works

```
            application renders
                     │
                     ▼
   ┌─────────────────────────────────┐
   │  WaveRender  ──►  TopologyRender│  ──► endpoint "RadioVoice Output"
   └────────┬────────────────────────┘
            │  timer copies the WaveRT buffer ──► ring
            ▼
      ┌───────────┐
      │  40 ms    │   LoopbackBuffer
      │   ring    │
      └───────────┘
            │  timer copies the ring ──► WaveRT buffer
            ▲
   ┌────────┴────────────────────────┐
   │ TopologyCapture ──► WaveCapture │  ──► endpoint "RadioVoice Microphone"
   └─────────────────────────────────┘
                     │
                     ▼
           application records
```

Four filters: a wave and a topology filter for each direction. The topology
filter is what makes an endpoint appear in the sound control panel at all — the
endpoint builder looks for a pin whose category is "speaker" or "microphone". A
wave filter on its own would be invisible.

**No hardware means no DMA.** In WaveRT it is the DMA engine that advances the
position register; here a high-resolution timer does it, copying as many bytes
per tick as the declared format implies. The position is derived from the
interrupt-time clock rather than by adding a constant per tick: timer callbacks
run late by varying amounts, and a position accumulating that error would drift
away from real time.

**The tick is 1 ms, and fixed.** With no register to read, the tick interval *is*
the resolution of the reported position — it is not a buffering choice. The audio
engine polls roughly every 10 ms and expects what it reads to track real time.
Deriving the tick from the buffer size, as an earlier version did, produced a
64 ms tick on a 64 ms buffer: the position stood still and then jumped by a whole
buffer, and the engine wrote in bursts separated by silence. Notifications are
therefore driven by the position crossing a period boundary rather than by the
callback running.

**Two formats, deliberately.** Pins advertise 16- and 24-bit PCM; the loopback
ring is always 32-bit signed PCM. Advertising a single format leaves the audio
engine nothing to negotiate, and it declines silently — indistinguishable from a
driver that never loaded.

The internal format is integer rather than float, and that is not a detail.
Kernel code on x64 may only touch floating-point or SSE registers between
`KeSaveExtendedProcessorState` and `KeRestoreExtendedProcessorState`; using them
bare corrupts the FPU state of whatever user-mode thread was interrupted. With a
32-bit integer hub every conversion is a shift, so the question never arises on a
path that runs every millisecond. Thirty-two bits also means the widest
advertised width converts in and back out losslessly.

### Files

| File | Role |
|---|---|
| `Common.h` | formats, buffer sizes, pin indices, subdevice names |
| `Driver.cpp` | `DriverEntry`, `AddDevice`, unload |
| `Adapter.cpp` | `StartDevice` — creates the four filters and the physical connections |
| `Descriptors.cpp` | filter and pin descriptors, data ranges |
| `MinWaveRT.cpp` | the WaveRT miniport and its streams, both directions |
| `MinTopo.cpp` | the topology miniport |
| `LoopbackBuffer.cpp` | the ring joining render to capture |
| `Diagnostics.cpp` | counters written to the service's registry key |

---

## Diagnostics

A driver that loads cleanly and then does nothing is the hardest kind to
investigate: no crash, no error code, and `DbgPrintEx` needs a debugger on the
other end. So the driver records what it did into its own service key, where any
PowerShell prompt can read it:

```
HKLM\SYSTEM\CurrentControlSet\Services\RadioVoiceAudio\Diagnostics
```

```powershell
.\tools\diagnose.ps1
```

reports the device state, those counters, the registered KS interfaces and the
resulting endpoints. Counters incremented from the timer callback cannot write
themselves — the IRQL there forbids it — so they are published on the next state
change. **Stop the stream before reading them.**

`tools\ksprobe` dumps the pins, dataflow, communication, categories, physical
connections and data ranges of any KS filter. Pointing it at this driver and at a
known-good one side by side is what found the format mismatch that kept the
endpoints from being created at all.

---

## When something does not work

**The driver does not load (code 577)** — the `.sys` signature. Check that test
mode is really active (the watermark on the desktop, not what `bcdedit` printed)
and that the certificate is in `LocalMachine\Root`.

**`pnputil` fails with `SPAPI_E_DRIVER_STORE_ADD_FAILED` (0xE0000247)** — most
often the catalogue does not match the `.sys`, because it was generated before
the binary was signed. Re-run `tools\sign.ps1`, which does them in the right
order.

**It installs but there are no endpoints** — two candidates. First, a mismatch
between subdevice names: `RV_WAVE_RENDER_NAME` and its relatives in `Common.h`
must match the `KSNAME_*` entries in the INF character for character. A mismatch
produces no error at all; the driver loads and says nothing.

Second, the advertised data ranges. If the audio engine cannot negotiate a
format it declines silently, which looks the same. Compare against a working
driver with `tools\ksprobe`.

**The endpoint exists but there is silence** — the timer loop in
`MinWaveRT.cpp::OnTick`. In order of suspicion: whether `SetState(KSSTATE_RUN)`
arrives at all, whether the timer started, and what the reported position is
doing. The counters under `Diagnostics` answer all three.

**Crackling, or drift** — `RV_LOOPBACK_MS` (40 ms) and the position arithmetic.
Both endpoints run independent timers, so the ring has to absorb the difference.
A burst longer than the ring is data loss on every burst, which is audible as a
regular rasp.

Log viewing: DebugView with "Capture Kernel" enabled. The `RV_LOG` macro calls
`DbgPrintEx` and is compiled in only for Debug builds.

---

## Licence

GPL-3.0-or-later, like the rest of the project. It links only against Windows
Driver Kit headers and import libraries, which are covered by the Microsoft WDK
licence terms and are not redistributed here.
