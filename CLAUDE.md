# RadioVoice

Real-time microphone processor for Windows: gate, EQ, compressor, limiter and a
VST3 chain, routed to a virtual device that other applications see as a
microphone. C++20, no audio framework — the WASAPI, ASIO and DirectSound
backends are ours, the interface is Dear ImGui on Direct3D 11.

## Language

**English** for everything that ships as code: source comments, identifiers,
log messages, commit messages, and pull request titles and descriptions.

**Polish** for the documentation written for whoever runs the application:
`README.md`, `QUICKSTART.md`, `NOTICE.md`.

The split is deliberate. The code is read by whoever maintains it, and the
surrounding material — Win32, VST3, ASIO, every specification this thing
implements — is in English; translating half of it produces sentences that name
`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` in Polish grammar. The README is read by
whoever wants a processed microphone in Discord, and that is a different person.

Strings shown in the interface are English, matching the panel names the code
already uses.

## Layout

- `src/audio` — WASAPI, ASIO and DirectSound streams, device enumeration, engine,
  drift resampler
- `src/dsp` — gate, suppressor, equaliser, compressor, limiter, chain, analysis
- `src/host` — VST3 scanning and hosting
- `src/gui` — palette, fonts, custom widgets, equaliser editor
- `src/core` — logging, paths, ring buffer, parameter and string helpers
- `src/app` — application state, panels, configuration
- `src/main.cpp` — the only file that knows about HWNDs and device contexts
- `res` — manifest, version info, icon (see below)
- `tools` — scripts that are not part of the build

## Build

```
build.cmd
```

Produces `build/bin/RadioVoice.exe`. Variants: `reldbg`, `debug`, `msvc`,
`no-vst3`. CMake fetches Dear ImGui, nlohmann/json and the VST3 SDK during
configuration, so the first `cmake -B` needs network access.

## Icon

`res/RadioVoice.ico` is committed and pulled in by `res/app.rc`; a normal build
does nothing with it. It is drawn by `tools/make-icon.py` (needs Pillow), every
size separately. After changing the artwork, run the script and commit the
resulting `.ico`.
