# Building and running

From nothing to a processed microphone in Discord.

> **Not building anything?** Take the installer from
> [Releases](https://github.com/doctorspider42/radio-voice/releases) and skip to
> [section 3](#3a-virtual-output--option-a-vb-cable). It installs the
> application, can start it with Windows, and — if the release was built with
> the driver payload — offers the virtual cable as well, behind a page that
> explains what accepting it costs you. Everything below is for building from
> source.

---

## Contents

1. [Tools](#1-tools)
2. [The application](#2-the-application)
3. [Virtual output — option A: VB-CABLE](#3a-virtual-output--option-a-vb-cable)
4. [Virtual output — option B: the bundled driver](#3b-virtual-output--option-b-the-bundled-driver)
5. [Setting it up in Discord / OBS / Teams](#4-setting-it-up-in-the-receiving-application)
6. [Everyday use](#5-everyday-use)
7. [When something does not work](#6-when-something-does-not-work)
8. [Undoing everything](#7-undoing-everything)

---

## 1. Tools

### For the application

| What | Why |
|---|---|
| CMake ≥ 3.24 | build system |
| Ninja | generator |
| MinGW-w64 GCC ≥ 13 **or** MSVC 2019+ | compiler |
| Git | CMake uses it to fetch dependencies |

Via [scoop](https://scoop.sh):

```powershell
scoop install cmake ninja mingw git
```

Dear ImGui, nlohmann/json, the VST3 SDK and RNNoise are fetched by CMake on the
first configure, so that step needs network access. Nothing afterwards does.

### Additionally, for the driver

```powershell
winget install --id Microsoft.WindowsSDK.10.0.26100 --exact
winget install --id Microsoft.WindowsWDK.10.0.26100 --exact
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override `
  "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools"
```

The SDK and WDK install machine-wide, so expect a UAC prompt. The kernel driver
requires MSVC; MinGW cannot build it.

---

## 2. The application

### Build

From the **repository root**:

```
build.cmd
```

Produces **`build\bin\RadioVoice.exe`**, around 18 MB — most of which is the
noise suppression model, compiled in. The first configure takes a few minutes
because it clones the VST3 SDK and downloads a 56 MB model archive; later builds
take about a minute.

> `driver\build-driver.cmd` is a different thing entirely — it builds the kernel
> driver, not the application.

### Packaging it

```
make-installer.cmd
```

Produces **`dist\RadioVoice-<version>-setup.exe`**. Needs
[Inno Setup 7](https://jrsoftware.org/isinfo.php):

```powershell
winget install --id JRSoftware.InnoSetup.7 --exact
```

The `.7` matters — the plain package is still version 6, which cannot build the
64-bit installer this script asks for. The two install side by side.

The driver is folded in only if a signed one is already sitting in
`driver\build` — that is, if you have already been through section 3B on this
machine. Without it the installer is built without the driver component, which
is a perfectly good thing to hand to someone who is going to use VB-CABLE.

To build the driver and fold it in, in one command:

```
make-installer.cmd with-driver
```

That does the whole of section 3B's build half — compile, certificate, signature
— and then stages the result into the installer. It elevates itself, so expect a
UAC prompt, and on a machine that has no signing certificate yet it creates one
and adds it to the machine's trusted stores. It does **not** install the driver
here; for that, use `install-driver.cmd`.

[`installer/README.md`](installer/README.md) has the details.

Other variants:

```
build.cmd reldbg     Release plus symbols, for a usable stack trace
build.cmd debug      no optimisation, assertions on
build.cmd msvc       Visual Studio instead of MinGW
build.cmd no-vst3    no plugin host
```

Each variant has its own directory (`build`, `build-reldbg`, `build-debug`, …),
so switching between them does not force a rebuild from scratch.

To build without the noise suppressor — and without its download and its 15 MB
of weights — configure with `-DRV_ENABLE_RNNOISE=OFF`.

### ASIO

Needs nothing extra: the host subset of the SDK is in the repository, so
`build.cmd` produces a binary with ASIO support. Licensing details are in
[NOTICE.md](NOTICE.md).

To update the SDK to a newer release, should that ever be needed:

```
tools\fetch-asio-sdk.cmd -Force
```

### First run

```powershell
.\build\bin\RadioVoice.exe
```

What happens on its own:

- the system's default microphone is selected as **input**
- a virtual cable is selected as **output** if one is installed (`CABLE Input`
  is preferred); if none is, the I/O panel explains what is missing
- processing starts and VST3 plugins are scanned in the background
- the chain is filled with the noise suppressor, gate, equalizer and compressor

Files it creates:

```
%APPDATA%\RadioVoice\config.json        settings, chain, plugin state
%APPDATA%\RadioVoice\plugins.json       plugin scan cache
%APPDATA%\RadioVoice\radiovoice.log     log - the first thing to check
```

Deleting `config.json` restores factory settings.

### Checking that it works

The top bar should show a green **RUNNING**, and the **INPUT** meter at the
bottom should move when you speak. If it does not, see
[section 6](#6-when-something-does-not-work).

---

## 3A. Virtual output — option A: VB-CABLE

The quickest route. Changes nothing about the machine's security posture.

1. Download and install [VB-CABLE](https://vb-audio.com/Cable/) (run the
   installer as administrator, then reboot).
2. In RadioVoice set **Output** to `CABLE Input (VB-Audio Virtual Cable)`.

Done — go to [section 4](#4-setting-it-up-in-the-receiving-application).

---

## 3B. Virtual output — option B: the bundled driver

No external dependency, but it requires lowering the machine's security settings
and two reboots.

### The short way

```
install-driver.cmd
```

It elevates itself and does everything in order: checks test signing, builds,
creates a certificate, signs, installs. If test signing is off it offers to turn
it on and asks for a reboot, after which it has to be run again — a reboot
cannot be crossed inside one command.

To remove it: `uninstall-driver.cmd`.

Below is the same thing step by step, for when something goes wrong.

### Step 1 — build

```
driver\build-driver.cmd
```

Produces `driver\build\Release\RadioVoiceAudio.sys` and the `.inf`.

### Step 2 — turn off Secure Boot

In the firmware (UEFI). Without this the next step reports success but the
setting does not take effect: Secure Boot blocks changes to the signing policy.

To check (PowerShell **as administrator**):

```powershell
Confirm-SecureBootUEFI
```

`False`, or a "not supported" error on a legacy BIOS, means you can continue.

### Step 3 — enable test signing

PowerShell **as administrator**:

```powershell
bcdedit /set testsigning on
```

➜ **REBOOT.** Afterwards a "Test Mode" watermark appears in the bottom-right
corner of the desktop.

> **What this actually costs.** The machine starts accepting any kernel driver
> signed by a certificate its own store trusts — and any process with
> administrator rights can add one there. One of the layers protecting against
> rootkits stops applying. It is reversible: see
> [section 7](#7-undoing-everything).

### Step 4 — certificate and signature

PowerShell **as administrator** (elevation is what lets the certificate be
trusted immediately, and what puts it in the machine store where the installer
can find it):

```
driver\tools\make-test-cert.cmd
driver\tools\sign.cmd
```

Neither script asks anything. The private key stays in the certificate store and
`signtool` reaches it by thumbprint.

`sign.ps1` generates the catalogue and signs **both** files: the `.sys` so the
kernel will load the image, and the `.cat` so PnP will accept the package at
install time.

### Step 5 — install

```
driver\tools\install.cmd
```

To verify:

```powershell
Get-PnpDevice -FriendlyName '*RadioVoice*'
```

There should be two devices in state `OK`. In the sound control panel:

- **Playback** → `RadioVoice Output`
- **Recording** → `RadioVoice Microphone`

### Step 6 — select it in the application

In RadioVoice set **Output** to `RadioVoice Output`. The engine restarts by
itself.

> Reinstalling the driver destroys and recreates the endpoints, which gives them
> new identifiers. RadioVoice follows a device that reappears under the same
> name, so the selection survives.

---

## 4. Setting it up in the receiving application

The rule is the same everywhere: select the **output side of the cable** as the
microphone.

| Option | What to select as the microphone |
|---|---|
| A (VB-CABLE) | `CABLE Output (VB-Audio Virtual Cable)` |
| B (bundled driver) | `RadioVoice Microphone` |

- **Discord** — Settings → Voice & Video → Input Device.
  **Turn off its noise suppression and AGC** there (Krisp, Echo Cancellation,
  Automatic Gain Control). Otherwise Discord processes an already processed
  signal and fights with the gate.
- **OBS** — Source → Audio Input Capture.
- **Teams / Zoom / Meet** — device settings, microphone.

---

## 5. Everyday use

RadioVoice has to **keep running** for audio to flow: it is what processes the
signal and feeds the cable. By default, closing the window stops the path.

**Out of the way, but still running.** Minimise the window and RadioVoice
carries on from the notification area; your microphone keeps being processed.
Click the icon to bring the window back, or right-click it for mute, start/stop
and exit.

The **cog** in the top bar controls the rest:

- *Closing hides the window* — makes the X put RadioVoice away instead of
  shutting it down. Worth turning on once you have it set up, because closing
  the window mid-call is otherwise an easy mistake to make.
- *Start with no window* and *Start with Windows* — together, these mean the
  microphone is already being processed by the time you join the first call.
  The installer's "start with Windows" option writes the same entry.

Launching RadioVoice a second time shows the copy already running rather than
starting another one, so a stray double-click cannot leave two of them fighting
over the microphone.

A cable is silent by definition, so to hear yourself turn on **Monitor** in the
top bar and select your headphones under Audio I/O. It starts and stops without
interrupting what is being sent.

- **Bypass all** — passes the raw microphone through, for an A/B comparison
- **Mute** — silence on the output
- settings save themselves; closing the window also saves plugin state

**Noise suppressor.** Leave it around 90%. It removes steady noise — a fan, an
air conditioner — while you are talking, which the gate cannot do. Taking all
100% can sound processed in a quiet room. It adds 10 ms of latency while it is
in the chain, and runs at 48 kHz only.

**Gate.** Talk normally and lower the threshold until the **OPEN/SHUT**
indicator stops flickering between words. Raise `hyst` if it still chatters.

**Compressor.** Lower the threshold until the **GR** meter shows 3–6 dB on the
louder syllables. A `ratio` of 3:1 is enough for speech, and `auto makeup`
evens out the level. Leave **RMS** on: it follows loudness rather than
individual transients, which on speech is what makes it inaudible.

If you have a single microphone in one side of a stereo input, set the input
fold-down (under the microphone selector) to **Mono (left)** or **Mono (right)**,
or the voice will come out on one side only.

**Updates.** The cog turns blue when a newer release exists — RadioVoice asks
GitHub once a day, and everything about it is under that cog. It stops there
until you click: **Download it** fetches the installer and checks it against the
checksum GitHub published, **Install and restart** runs it and brings RadioVoice
back with everything where you left it. Windows will ask for permission at that
point, naming an unknown publisher, for the same reason the driver is unsigned.
Both the checking and the rest are optional — one checkbox in the same menu turns
the whole thing off, and installing by hand from the releases page does exactly
the same job. The README has the [longer version](README.md#updating).

---

## 6. When something does not work

First stop, always: the **`Log`** button in the top-right corner, or
`%APPDATA%\RadioVoice\radiovoice.log`.

**The INPUT meter does not move while you speak**
The application warns when a device is open but delivering nothing. Check that
the microphone is not muted in Windows sound settings, and that desktop
applications are allowed microphone access under Privacy.

**There is nothing to set Output to**
No virtual cable is installed — see [section 3A](#3a-virtual-output--option-a-vb-cable)
or [3B](#3b-virtual-output--option-b-the-bundled-driver).

**`dropouts` is climbing**
Increase **Processing block** (256 → 512). If `dsp` is near 100% the chain is
missing its deadline; switch off the heaviest plugin.

**`drift` is pinned at ±5000 ppm**
The input and output clocks are further apart than the resampler can make up.
This happens with two purely software devices. Try a different pair, or the same
sample rate on both sides.

**The application does not start**
The log records a plugin's path immediately before loading it. If a plugin takes
the process down, it is blacklisted on the next start and the application comes
up. The blacklist can be cleared from the *Add plugin* window.

**No plugins in the list**
Standard VST3 folders are scanned; extra ones can be added under **Scan folders**
in the *Add plugin* window. 32-bit plugins from
`Program Files (x86)\Common Files\VST3` will not load into a 64-bit process —
the log says so explicitly (error 193).

**Audio breaks up with DirectSound as the input**
Some interfaces deliver only a fraction of real time through DirectSound
capture. The log reports how the polls divided up when the stream stops. Use
WASAPI.

**Driver problems** — a separate list of symptoms is in
[`driver/README.md`](driver/README.md#when-something-does-not-work).

---

## 7. Undoing everything

### The driver

```
uninstall-driver.cmd
```

Removes the device, the driver package and the test certificate. Test signing
stays on: it is a machine-wide setting and may have been enabled earlier for
something else. To turn it off (PowerShell **as administrator**, then
**reboot**):

```powershell
bcdedit /set testsigning off
```

Then re-enable Secure Boot in the firmware.

### The application

Built from source it installs nothing anywhere — delete the `build` directory
and `%APPDATA%\RadioVoice`.

Installed from the installer, use *Apps → Installed apps → RadioVoice →
Uninstall*. That removes the program, the shortcuts and the autostart entry, and
removes the driver too if it installed one. `%APPDATA%\RadioVoice` is left
behind on purpose — it holds your chain and your plugin state — so delete it by
hand if you want it gone. Its `updates` subfolder holds at most one downloaded
installer, replaced by the next one; deleting it costs nothing.

### The tools

```powershell
winget uninstall Microsoft.WindowsWDK.10.0.26100
winget uninstall Microsoft.WindowsSDK.10.0.26100
winget uninstall Microsoft.VisualStudio.2022.BuildTools
```
