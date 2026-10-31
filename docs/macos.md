# macOS preview

RadioVoice retains the shared C++ DSP and redesigned Dear ImGui interface.
The macOS implementation uses Cocoa + Metal for the window and AUHAL for audio.
Minimum deployment target: macOS 13. Native and universal (arm64 + x86_64)
builds are supported; runtime validation is on Apple Silicon.

## Build and run

Install Xcode Command Line Tools, CMake and Ninja, then run:

```sh
./build-macos.sh
open build-macos/bin/RadioVoice.app
```

The first configuration fetches the same dependencies as Windows. The app bundle
is signed after linking with the identity in `RADIOVOICE_CODESIGN_IDENTITY` or
`.radiovoice-signing.json`; the local Zitter signing preference is also supported.
Without a configured identity, development builds use an ad-hoc signature. DMG
packaging requires a permanent certificate unless `--unsigned` is explicit.
See [packaging instructions](../installer/macos/README.md). Do not enable the
Windows ASIO backend on macOS.

## Routing

1. Run RadioVoice.pkg from the DMG to install the app and virtual microphone.
2. Allow RadioVoice microphone access on its first launch.
3. In Devices, select a physical microphone. RadioVoice Send is selected automatically.
4. In Discord, OBS or Teams, select **RadioVoice** as the microphone.
5. For local listening, enable Monitor and select headphones.

The HAL bundle has a visible input-only RadioVoice device and a hidden output-only
RadioVoice Send device. Ordinary system enumeration omits the sending device;
RadioVoice resolves it by UID. Other applications can resolve that UID too:
this is presentation, not an access-control mechanism. Both use stereo float
samples at 48 kHz. Existing BlackHole routes migrate when our own driver is found;
other explicitly selected outputs are preserved.

A Multi-Output or Aggregate Device is not required for this arrangement.
Keep the system output and the receiving application's speakers on physical
headphones.

Each stream opens at the selected device's native rate and buffer size. Those
values can be changed in Audio MIDI Setup; device format changes trigger engine
recovery. The engine's existing resampler and drift controller bridge independent
clocks. Device UIDs, not transient numeric AudioDeviceIDs, are saved in settings.

## Verification

```sh
ctest --test-dir build-macos --output-on-failure
build-macos/bin/RadioVoice.app/Contents/MacOS/RadioVoice --list-devices
build-macos/bin/RadioVoice.app/Contents/MacOS/RadioVoice --smoke-test
```

The portability test covers non-ASCII filesystem/device strings, UTF-16 plugin
text, ARM floating-point state restoration, missing-device cleanup, and RNNoise
processing on the native architecture. The playback test opens the real default
physical output three times, feeds silence, checks callback delivery, and ensures
callbacks stop before their producer can be destroyed. It skips if no physical
default output is available. It does not record the microphone.

The UI smoke test opens the redesigned window and checks actual Metal drawable
presentation over 120 stationary timer ticks. It requires at least 75% of those
ticks to produce onscreen presentations at 30 FPS or more, then exits. A frozen
window fails even when the UI timer itself is running. It
uses a fresh temporary configuration, disables auto-start and does not request
microphone access. `RADIOVOICE_SMOKE_FRAMES` can extend the run for visual
inspection. `RADIOVOICE_DATA_DIR` overrides the normal settings directory for
isolated development runs.

The HAL interface test loads the actual driver bundle without installing it or
capturing a microphone. It verifies visible input-only / hidden output-only
devices, stable UIDs, stereo 48 kHz samples, bit-exact loopback across ring-buffer
wraparound and silence after the sender stops. This does not replace an installed
CoreAudio/Discord integration check.

## Preview boundaries

- The built-in microphone → full DSP chain → BlackHole → independent capture
  route was verified on Apple Silicon at 48 kHz on 2026-10-31. The 15-second
  normal/mute/normal check passed: signal returned after mute, the muted capture
  was exactly silent, all samples were finite and the receiver reported zero
  xruns. Individual receiving applications still need their own setup/check.
- Real third-party VST3 editor embedding, resizing, state restore and pop-out need
  plugin compatibility testing. A plugin must contain the architecture of the
  host; no Windows DLL loading or cross-architecture bridge is provided.
- Fixed-size editors are kept at their native size; use Pop out if they do not
  fit. Resizable editors negotiate a fitting size with the plugin.
- Automatic updates are manual on macOS; the Windows updater is excluded from
  the Mac target. Universal DMGs support Developer ID signing and Apple
  notarization; app, unified PKG and DMG receive stapled tickets when enabled.
- Login items use macOS ServiceManagement. Registration can depend on the local
  bundle's signing/installation status; failures are reported in the log. Test
  from a stable location such as Applications before enabling start at login.
- Long-running drift, unplug/replug, sleep/wake and Bluetooth profile changes
  still need hardware testing. Intel runtime behavior has not been verified.

## End-to-end virtual microphone check

With the RadioVoice driver (or BlackHole fallback) installed and microphone access granted, close RadioVoice and run:

```sh
open build-macos/bin/RadioVoice.app --args --verify-route
```

The normal application stays open. A separate capture stream reads the selected
RadioVoice input (or BlackHole input for the fallback route) for 15 seconds: six seconds of normal processing, three muted,
then six restored. Speak during both normal phases. The first second after each
transition is discarded so device buffers and effect lookahead can drain.

`~/Library/Application Support/RadioVoice/route-verification.json` contains only
aggregate sample counts, RMS/peak levels, non-finite checks and receiver xruns.
The result is `passed` only when both normal phases contain signal, the muted
phase is silent, every phase received samples, and no invalid samples or receiver
xruns occurred. Quiet input/suppression can yield `inconclusive`; silence alone
never counts as a successful route test. The original mute state is restored
when verification finishes or the application closes. No audio recording is saved.
