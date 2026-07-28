# RadioVoice

Real-time microphone processor for Windows. Neural noise suppression, a noise
gate, a graphic equaliser, a compressor, a brickwall limiter and a **reorderable
VST3 plugin chain** — routed to a virtual device that other applications see as
a microphone.

C++20, Dear ImGui and Direct3D 11. WASAPI, ASIO and DirectSound backends.

---

## Quickstart

```bash
build.cmd
```

Then run `build\bin\RadioVoice.exe`.

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
signing. *(Currently in Polish.)*

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

Installing it requires Secure Boot off and test signing on, or a commercial EV
certificate. `install-driver.cmd` and `uninstall-driver.cmd` in the repository
root handle build, signing and installation; the full procedure and the
diagnostic tooling are in [`driver/README.md`](driver/README.md).

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
- **MSVC** builds of the application are configured but only MinGW has been run.
- Moving the window between displays of different DPI is handled but has not
  been exercised on a multi-monitor setup.

---

## Licence

**GPL-3.0-or-later**. Per-dependency breakdown in [NOTICE.md](NOTICE.md).
