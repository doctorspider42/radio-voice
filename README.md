<h1 align="center">RadioVoice</h1>

<p align="center"><strong>Your mic. Your sound.</strong></p>
<p align="center">
  Real-time microphone processing for Windows, with a macOS source-build preview.<br>
  Clean up the noise, shape your voice, and bring your own VST3 plugins.
</p>

<p align="center">
  <a href="https://github.com/doctorspider42/radio-voice/releases/latest"><img src="https://img.shields.io/github/v/release/doctorspider42/radio-voice?style=flat-square&amp;color=63c5b8" alt="Latest release"></a>
  <a href="https://github.com/doctorspider42/radio-voice/actions/workflows/release.yml"><img src="https://github.com/doctorspider42/radio-voice/actions/workflows/release.yml/badge.svg" alt="Release build"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078d4?style=flat-square" alt="Windows 10 / 11, x64">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599c?style=flat-square" alt="C++20">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=flat-square" alt="GPL-3.0-or-later license"></a>
</p>

<p align="center">
  <a href="https://github.com/doctorspider42/radio-voice/releases/latest"><strong>Download for Windows</strong></a>
  &nbsp;·&nbsp; <a href="QUICKSTART.md">Setup guide</a>
  &nbsp;·&nbsp; <a href="CHANGELOG.md">What's new</a>
  &nbsp;·&nbsp; <a href="https://github.com/doctorspider42/radio-voice/issues">Report a bug</a>
</p>

![RadioVoice: the signal chain as a rack of modules beside the open compressor, with its level history, transfer curve and gain-reduction meter](docs/screenshot.png)

## A voice chain for every app

Process your microphone once, then send the result to **Discord, OBS, Teams**, or
any app that accepts a microphone through a virtual audio cable.

| | What you get |
| :--- | :--- |
| **Clean** | RNNoise neural noise suppression and a gate with lookahead. |
| **Shape** | Ten-band EQ, high- and low-pass filters, compressor, and output limiter. |
| **Make it yours** | Drag to reorder built-in effects and VST3 plugins in the rack. Plugin editors open docked in the window, in a window of their own, or as a parameter list. |
| **Keep it running** | Monitor through headphones, save your chain and plugin state, and hide the window to the tray while audio keeps flowing. |

## Get started

1. **Install RadioVoice** from the [latest release](https://github.com/doctorspider42/radio-voice/releases/latest).
2. **Route your audio.** With [VB-CABLE](https://vb-audio.com/Cable/) installed,
   click the route in RadioVoice's header and choose your microphone and `CABLE Input`.
3. **Pick `CABLE Output` as the microphone** in your receiving app. Adjust your
   chain; enable **Monitor** and select your headphones to hear the result.

> **Bundled driver:** The optional RadioVoice virtual cable is test-signed and
> requires Secure Boot off and Windows test signing on. VB-CABLE is the setup
> path above. Read the [driver guide](driver/README.md) before choosing the bundled driver.

Need the full walkthrough? See [QUICKSTART.md](QUICKSTART.md).

## Under the hood

**C++20 · Dear ImGui · Direct3D 11 · WASAPI / ASIO / DirectSound**

Native audio backends, a lock-free capture buffer, and clock-drift compensation
between input and output devices. No audio framework.

```text
Microphone → Your chain → Output limiter → Virtual cable → Your apps
```

The effects before the output limiter are reorderable. Settings and plugin state
live in `%APPDATA%\RadioVoice\config.json`; logs are in `radiovoice.log` alongside it.

### Build from source

Use **Windows x64, CMake 3.24+, Ninja, Git, and MinGW-w64 GCC 13+**.
The first build downloads dependencies.

```bat
build.cmd
```

Run `build\bin\RadioVoice.exe`. Build variants and packaging instructions are in
the [setup guide](QUICKSTART.md) and [installer guide](installer/README.md).

### macOS preview

Requires **macOS 13+, Xcode Command Line Tools, CMake 3.24+, Ninja and Git**.
Build on the Mac you want to run it on (Apple Silicon or Intel):

```sh
./build-macos.sh
open build-macos/bin/RadioVoice.app
```

Install [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole#installation-instructions),
select your microphone as **Input** and **BlackHole 2ch** as **Output**, then
select **BlackHole 2ch** as the microphone in the receiving application. Enable
**Monitor** and choose headphones for a separate listening output.

Set the output device to **48,000 Hz** in Audio MIDI Setup for RNNoise. The
CoreAudio backend uses each device's current native rate and buffer size without
changing shared device settings; the engine bridges different input/output
rates and clocks. Allow microphone access when macOS asks. If access was denied,
enable RadioVoice in **System Settings → Privacy & Security → Microphone**.

Settings and logs live in `~/Library/Application Support/RadioVoice/`. Native
VST3 bundles are scanned from `~/Library/Audio/Plug-Ins/VST3` and
`/Library/Audio/Plug-Ins/VST3`; Windows plugin binaries cannot be used. An arm64
build needs arm64 or universal plugins. Large fixed-size plugin editors can be
opened with **Pop out** when they do not fit inside the rack.

A branded DMG installer is available from CI/release assets: open it and drag
RadioVoice into Applications. Universal images contain Apple Silicon and Intel
binaries. Images built without a Developer ID certificate are explicitly named
`unsigned` and are development previews. With the signing and App Store Connect
credentials configured, release images are signed, notarized and stapled.

Local builds use the pinned signing identity when configured, and otherwise an
ad-hoc development signature. Automatic macOS updates and a bundled virtual
driver are not included. Intel runtime behavior has not been verified on Intel
hardware. See [macOS packaging](installer/macos/README.md) and
[development notes](docs/macos.md) for details.

### Current limits

- **WASAPI shared mode is the tested path.** ASIO and WASAPI exclusive mode are
  implemented but not verified on hardware; DirectSound capture has known issues
  on some interfaces.
- **Neural noise suppression requires 48 kHz** and adds 10 ms of latency.
- **MinGW is the supported build toolchain.** MSVC builds have known dependency
  failures. The installer and bundled driver are not Microsoft-signed.

## License

**GPL-3.0-or-later.** See [LICENSE](LICENSE) and [third-party notices](NOTICE.md).
