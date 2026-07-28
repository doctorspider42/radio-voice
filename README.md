# RadioVoice

Real-time microphone processor for Windows. Neural noise suppression, a noise
gate, a graphic equaliser, a compressor, a brickwall limiter and a **reorderable
VST3 plugin chain** — routed to a virtual device that other applications see as
a microphone.

C++20, Dear ImGui and Direct3D 11. WASAPI, ASIO and DirectSound backends.

---

## Quickstart

Grab the installer from
[Releases](https://github.com/doctorspider42/radio-voice/releases) and run it.
It can put RadioVoice in the notification area at sign-in, and it offers to
install the virtual audio driver — read the page it shows you before accepting
that part.

Or build it:

```bash
build.cmd
```

Then run `build\bin\RadioVoice.exe`. To produce an installer of your own,
`make-installer.cmd` (see [installer/README.md](installer/README.md)).

To get the processed signal into Discord, OBS, Teams or anything else, it has to
reach a virtual audio device. Pick one:

**Using VB-CABLE** — install [VB-CABLE](https://vb-audio.com/Cable/), set
**Output** to `CABLE Input`, and select `CABLE Output` as the microphone in the
receiving application. RadioVoice detects an installed cable and selects it on
first run.

**Using the bundled driver** — from an elevated prompt:

```bash
install-driver.cmd
```

This needs Secure Boot off and test signing on; the script offers to enable test
signing and tells you when a reboot is required. Set **Output** to
`RadioVoice Output` and select `RadioVoice Microphone` in the receiving
application.

Because a virtual cable is silent by definition, turn on **Monitor** in the top
bar and point it at your headphones to hear yourself.

[QUICKSTART.md](QUICKSTART.md) covers the whole path in detail, including driver
signing.

---

## Signal flow

```
microphone ──> WASAPI / ASIO / DirectSound capture
                     │
                     ▼
              lock-free ring buffer
                     │
                     ▼
            clock-drift resampler
                     │
    gain ──> [ suppressor | gate | EQ | compressor | VST3 … ] ──> gain ──> limiter
                     │
        ┌────────────┴────────────┐
        ▼                         ▼
  virtual device            monitor device
        │                    (headphones)
        ▼
  Discord / OBS / Teams / …
```

Capture and playback run on two independent clocks. Even at a nominal 48 kHz
they differ by tens to thousands of parts per million, so a variable-ratio
resampler driven by a PI controller holds the bridging buffer at a target fill.
The correction in effect is shown as `drift` in the status bar.

The whole DSP chain runs on the output device thread.

---

## Features

**Noise Suppressor** — [RNNoise](https://github.com/xiph/rnnoise). Attenuates
steady noise while you are speaking, which a gate cannot do. Mix control from 0
to 100%; costs 10 ms of latency while in the chain. 48 kHz only.

**Noise Gate** — threshold, range, hysteresis, attack/hold/release, lookahead
and a high-pass filter in the detector path.

**Equalizer** — ten ISO bands (shelves at the extremes), 24 dB/oct high- and
low-pass filters, response curve drawn over the live input spectrum. Drag the
band handles; the wheel over a handle sets Q.

**Compressor** — soft knee, RMS or peak detection, lookahead, detector-path
high-pass, auto makeup. Ballistics computed in the dB domain.

**Output Limiter** — brickwall with lookahead.

**Processing Chain** — the built-in modules and any VST3 plugins in one
drag-to-reorder list. Each has its own enable switch; plugins open their native
editor window or a parameter list.

**Monitor** — a second output carrying the same processed signal, with its own
level and mute. Starts and stops without restarting the engine.

**Downmix** — input fold-down (stereo, left, right, sum) under the microphone
selector, and a sum-to-mono switch under the output. The first is for a single
microphone plugged into one side of a stereo input.

**Notification area** — the window and the engine are independent. Hide the
window and your microphone carries on being processed; the icon's menu offers
mute, start/stop and exit. Whether minimising hides the window, whether closing
does, and whether RadioVoice starts with Windows are all under **Tray** in the
top bar. Launching a second copy shows the first rather than contending for the
same devices.

**Updates** — once a day RadioVoice asks GitHub which release is the newest, and
says so on the version button in the top bar. Nothing is downloaded until you
click **Download it**, and nothing is installed until you click **Install and
restart**; the whole thing can be turned off with one checkbox behind the same
button. See [Updating](#updating).

Configuration, the chain and plugin state are saved to
`%APPDATA%\RadioVoice\config.json`. The log is `%APPDATA%\RadioVoice\radiovoice.log`.

### Plugin scanning

Standard VST3 folders are scanned in the background at start-up and cached in
`plugins.json`. Extra folders can be added under **Scan folders** in the *Add
plugin* window.

A plugin can take the process down as it loads, so the scanner writes the bundle
it is about to touch into a sentinel file and clears it afterwards. If that file
survives a restart, the bundle is blacklisted instead of being tried again. The
blacklist can be cleared from the same window.

---

## Updating

The button on the right of the top bar shows the version this build is. Behind
it is everything to do with replacing it.

There is no update server. RadioVoice reads the same release list you would:
`api.github.com/repos/doctorspider42/radio-voice/releases/latest`, published by
the workflow that builds the installer. A check is that one request and nothing
else — it happens half a minute after start-up, then once a day, and only while
**Check for updates automatically** is ticked. Untick it and RadioVoice never
touches the network unless you press **Check now**.

Nothing happens on its own beyond the check:

1. A newer release lights the button up. The release notes shown are the
   `CHANGELOG.md` section for that version.
2. **Download it** fetches the installer into `%APPDATA%\RadioVoice\updates`. It
   is checked against the size and the SHA-256 GitHub publishes for the asset,
   and rejected if either disagrees. If the window is hidden when the download
   finishes, the notification icon says so once.
3. **Install and restart** closes RadioVoice, runs the installer silently and
   starts RadioVoice again when it is done. Your devices, chain, plugin state and
   settings are kept — the installer is an upgrade in place, not a reinstall.

**Windows will ask for permission at step 3**, and there is no way around it:
RadioVoice installs under Program Files because the driver component has to, and
writing there needs elevation. The installer is also unsigned — the same reason
the driver is — so the prompt names an unknown publisher.

That is worth being clear about: an update is TLS to GitHub and a checksum
published beside the file, not a signature. If you would rather see what you are
running before you run it, untick automatic checking and take the installer from
the [releases page](https://github.com/doctorspider42/radio-voice/releases) by
hand. Installing over an existing copy works exactly the same way.

---

## Building

Requirements:

- Windows 10 1903+ or Windows 11 (x64)
- CMake ≥ 3.24 and Ninja
- MinGW-w64 GCC ≥ 13, or MSVC 2019+
- A Direct3D 11 driver (feature level 10.0; WARP as a fallback)

```bash
build.cmd
```

Produces `build\bin\RadioVoice.exe`. Variants: `build.cmd reldbg`, `debug`,
`msvc`, `no-vst3`. Or through CMake directly:

```bash
cmake --preset mingw
cmake --build --preset mingw
```

Dear ImGui, nlohmann/json, the VST3 SDK and RNNoise are fetched at configure
time, so the first `cmake -B` needs network access. RNNoise additionally
downloads a 56 MB model archive, verified against a SHA-256 recorded in the
pinned source. The weights are compiled in, which is most of the executable's
size.

Options: `-DRV_ENABLE_VST3=OFF`, `-DRV_ENABLE_RNNOISE=OFF`, `-DRV_ENABLE_ASIO=OFF`.

ASIO builds out of the box — the host subset of the SDK is in
`third_party/asiosdk`. `tools\fetch-asio-sdk.cmd` updates it to a newer release.

The application icon is committed as `res/RadioVoice.ico` and compiled in
through `res/app.rc`; a normal build does not regenerate it. `tools/make-icon.py`
(needs Pillow) redraws it, sizes 16 to 256 px individually.

---

## Virtual audio driver

[`driver/`](driver/) contains a kernel driver that creates a
`RadioVoice Output` / `RadioVoice Microphone` endpoint pair and loops one into
the other — a functional equivalent of VB-CABLE with no external dependency.
48 kHz, 16- or 24-bit, stereo.

Installing it requires Secure Boot off and test signing on. There is no way
around that short of a Microsoft signature — see
[below](#can-it-be-installed-without-test-signing).

Each of the two things you might want is one command. Both need the WDK, both
elevate themselves, and both will ask before changing anything:

```bash
install-driver.cmd
```

Builds, signs and installs the driver **on this machine** — the whole procedure,
including creating the signing certificate the first time. `uninstall-driver.cmd`
reverses it.

```bash
make-installer.cmd with-driver
```

Builds, signs and folds the driver **into the installer**, so the resulting
`dist\RadioVoice-<version>-setup.exe` can offer it to someone else. Without
`with-driver` the installer is built with whatever is already in `driver\build`,
and if that is nothing, it simply does not offer the component.

The full procedure step by step, and the diagnostic tooling, are in
[`driver/README.md`](driver/README.md).

### What it costs

Both routes end in the same two changes to the machine, and it is worth being
plain about them:

- **Test signing on**, which means the machine accepts any kernel driver signed
  by a certificate its own store trusts. One of the layers protecting against
  rootkits stops applying.
- **A certificate in `LocalMachine\Root`**, which means everything signed with
  that key is trusted from then on — not only this driver.

Both are reversible, and the installer refuses to proceed until you have said
you understand them. If you would rather not,
[VB-CABLE](https://vb-audio.com/Cable/) does the same job and RadioVoice works
with it just as well.

### Can it be installed without test signing?

No — and knowing what you are doing does not change it. The check lives in the
kernel, and being deliberate about it is not one of the inputs.

**Trusting the certificate is not enough, and fails in the worst way.** Putting
the `.cer` into `LocalMachine\Root` satisfies the user-mode check, so the
package installs and reports success. The kernel then applies a separate,
stricter policy that accepts only Microsoft-issued signatures, refuses the
image, and the device sits in Device Manager with code 52. Nothing says which
of the two checks failed.

The only route to a driver that loads on an ordinary machine is a signature from
Microsoft, via
[attestation signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation):
an EV code-signing certificate, a Partner Center account, and the driver
submitted to the Hardware Dev Center. No HLK test run is needed for attestation,
but the EV certificate is
[not optional](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
— it is what the submission is authenticated with. Cross-signing, the old way
round this, stopped working in Windows 10 1809.

So: test signing, or money. If neither appeals, VB-CABLE is WHQL-signed and
installs on any machine without touching a boot setting.

---

## Status

Verified on real hardware: WASAPI capture and playback, the DSP chain, VST3
hosting including native editor windows, the monitor output, and the bundled
driver end to end — built, signed, installed and passing audio.

Known gaps:

- **DirectSound capture** under-delivers on some interfaces. The log reports how
  its polls divided up when a stream stops. WASAPI is the recommended backend.
- **ASIO** compiles and enumerates drivers; an ASIO stream has not been opened
  on this machine.
- **WASAPI exclusive mode** is implemented but untested.
- **MSVC** builds do not currently work: rnnoise's SSE4.1 sources and the VST3
  SDK's `char8_t` usage both fail under `cl`. MinGW is the supported toolchain,
  and the one the release workflow uses.
- Moving the window between displays of different DPI is handled but has not
  been exercised on a multi-monitor setup.

---

## Licence

**GPL-3.0-or-later**. Per-dependency breakdown in [NOTICE.md](NOTICE.md).
