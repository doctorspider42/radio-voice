# Changelog

Notable changes to RadioVoice.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
version numbers follow [Semantic Versioning](https://semver.org/).

The section whose heading matches the `VERSION` file becomes the GitHub release
description word for word, so it is written for whoever is deciding whether to
download the installer.

## [0.3.0] — 2026-07-28

### Added

- **RadioVoice now tells you when there is a newer version.** The button at the
  right of the top bar shows which build you are running; once a day it asks
  GitHub whether a newer release exists, and lights up when one does. The release
  notes for it are shown in place, so there is something to decide on.
- **One click downloads it, and one more installs it.** The installer is fetched
  into `%APPDATA%\RadioVoice\updates` and checked against the size and the
  SHA-256 that GitHub publishes for it; anything that disagrees is thrown away.
  Installing closes RadioVoice, runs the installer without a wizard to click
  through, and starts it again — devices, chain, plugin state and settings all
  where you left them.
- Nothing is downloaded or installed on its own. The daily check is one request
  for the release list, and the checkbox behind the same button stops even that,
  after which RadioVoice does not touch the network at all.
- With the window hidden — which is where a tray application spends most of its
  life — a downloaded update is announced once by the notification icon, and its
  menu grows an **Install update** item.

Worth knowing before you use it: Windows asks for permission when the installer
runs, and names an unknown publisher. RadioVoice lives under Program Files
because the driver component has to, and the installer is unsigned for the same
reason the driver is. An update is TLS to GitHub plus a published checksum, not a
signature — installing by hand from the releases page remains exactly as good.

## [0.2.2] — 2026-07-28

### Added

- **`make-installer.cmd with-driver`** — one command that builds the driver,
  signs it, folds it into the installer and compiles the result. Getting a
  driver into an installer previously meant four steps in the right order, each
  of which fails in a way that does not name the step before it.
- The README now says plainly which single command does which of the two things
  people actually want: `install-driver.cmd` to install the driver here,
  `make-installer.cmd with-driver` to ship it to someone else. Along with what
  both cost — test signing, and a certificate in the machine's trusted root
  store.
- A README answer to the question everyone asks: no, the driver cannot be
  installed without test signing, and trusting the certificate by hand is not a
  way round it — the package installs, the kernel still refuses the image, and
  the device stops at code 52 without saying which check failed.

### Fixed

- `sign.cmd` claimed "no elevation needed". It needs elevation whenever the
  certificate is in `LocalMachine\My`, which is where it has to be; without it
  `signtool` reports "No certificates were found that met all the given
  criteria", which sounds like a missing certificate rather than an unreadable
  one.

## [0.2.1] — 2026-07-28

### Fixed

- **The installed application would not start** — `0xc0000142` on launch, with
  no indication of why. `-static-libgcc -static-libstdc++` left
  `libwinpthread-1.dll` behind as a runtime dependency, which sits on `PATH`
  beside the compiler and nowhere else. Linking with `-static` makes the
  executable self-contained, which is what an installed one has to be.
- **The component page offered a driver that was not there.** An installer
  built without a driver payload still listed "Full installation (with the
  driver)" and then showed only the application. There is now no component page
  at all unless a driver is actually bundled.
- **Uninstall is reachable from the Start menu**, not only from
  Settings → Apps.
- **"Start with Windows" now sets itself for the right user.** The installer
  runs elevated, so a registry entry written from it landed in the hive of
  whoever answered the UAC prompt rather than the person signing in. The
  application writes its own entry instead, launched as the original user.

### Changed

- The installer is built with **Inno Setup 7** and is now a 64-bit program.
  A 32-bit installer is subject to WOW64 file system redirection, whose view of
  `System32` contains no `pnputil.exe` — the one tool the driver component
  cannot work without.

## [0.2.0] — 2026-07-28

### Added

- **An installer** — `dist/RadioVoice-<version>-setup.exe`. Installs the
  application, creates the shortcuts, and can start RadioVoice with Windows.
- **It runs in the notification area.** Hide the window and the microphone
  carries on being processed. The tray icon's menu offers the window, mute,
  start/stop and exit; minimising and closing are configurable separately,
  behind the **Tray** button on the top bar.
- **Optional driver installation** from the installer, behind a page that has
  to be read and acknowledged: the virtual audio cable is not signed by
  Microsoft, so it needs Windows test signing turned on and its certificate
  added to the machine's trusted store. Both are spelled out there.
- **Start with Windows** — an entry under `HKCU\...\Run` with `--minimized`.
  The installer's option and the application's own checkbox write the same one.
- **One instance at a time.** A second launch shows the window of the copy
  already running instead of fighting it for the audio devices.
- **Versioning.** The `VERSION` file is now the single source; this changelog
  is new; and pushing to `main` builds the installer and publishes a release.

### Changed

- Installing the driver no longer needs `devcon.exe` from the WDK. Where devcon
  is absent, the device node is created through SetupAPI directly — which is
  what lets the installer work on a machine that has never seen a WDK.

## [0.1.0]

First release: real-time microphone processing (gate, neural noise suppression,
equaliser, compressor, limiter), a reorderable VST3 plugin chain, WASAPI, ASIO
and DirectSound backends, and a locally built virtual audio cable driver.
