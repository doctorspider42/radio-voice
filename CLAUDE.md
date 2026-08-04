# RadioVoice

Real-time microphone processor for Windows: gate, EQ, compressor, limiter and a
VST3 chain, routed to a virtual device that other applications see as a
microphone. C++20, no audio framework — the WASAPI, ASIO and DirectSound
backends are ours, the interface is Dear ImGui on Direct3D 11.

## Language

**English**, everywhere. Source comments, identifiers, log messages, commit
messages, pull request titles and descriptions, and the documentation:
`README.md`, `QUICKSTART.md`, `NOTICE.md`, `CHANGELOG.md`.

The surrounding material — Win32, VST3, ASIO, every specification this thing
implements — is in English, and translating half of it produces sentences that
name `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` in Polish grammar. Strings shown in
the interface are English too, matching the panel names the code already uses.

The one exception is the **installer**, which is translated because it is the
only part a user meets before deciding whether to trust the thing. Its script is
code, so its comments and identifiers are English like everything else; the
strings it displays are given in both English and Polish under
`[CustomMessages]`, and Inno Setup picks by the system's language. Add both
whenever a message is added — a half-translated wizard is worse than an
untranslated one.

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
- `docs` — images the documentation points at; nothing the build touches
- `tools` — scripts that are not part of the build
- `installer` — Inno Setup script, and the staging that folds the driver into it
- `driver` — the kernel driver and its own tooling; see `driver/README.md`

## Build

```
build.cmd
```

Produces `build/bin/RadioVoice.exe`. Variants: `reldbg`, `debug`, `msvc`,
`no-vst3`. CMake fetches Dear ImGui, nlohmann/json and the VST3 SDK during
configuration, so the first `cmake -B` needs network access.

```
make-installer.cmd
```

Produces `dist/RadioVoice-<version>-setup.exe`. Needs Inno Setup 6. The driver
is folded in only if a signed one is already sitting in `driver/build` — see
`installer/README.md`.

## Versioning

**Every change bumps `VERSION` and adds to `CHANGELOG.md`. Both, every time.**

`VERSION` at the repository root holds the whole version, and it is the only
place that does. CMake reads it, generates `rv_version.h` from it for the
resource block, and the installer reads it too. Nothing else hard-codes a
version — if you find yourself typing one, that is the bug.

Semantic versioning: patch for a fix, minor for a feature, major for a break.

The `CHANGELOG.md` section whose heading matches `VERSION` becomes the GitHub
release description verbatim, so write it for whoever is deciding whether to
download the installer — not as a list of commits.

Pushing to `main` builds the installer and publishes a release tagged
`v<VERSION>` (`.github/workflows/release.yml`). A push whose version is already
released still builds, and simply does not publish — so forgetting the bump
costs a release, not a red build.

The driver carries its own version in `DriverVer` inside
`driver/RadioVoiceAudio.inf`, on its own schedule. Bump it whenever the `.sys`
changes, or Windows will keep loading the binary it already has.

## Icon

`res/RadioVoice.ico` is committed and pulled in by `res/app.rc`; a normal build
does nothing with it. It is drawn by `tools/make-icon.py` (needs Pillow), every
size separately. After changing the artwork, run the script and commit the
resulting `.ico`.
