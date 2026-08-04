# Changelog

Notable changes to RadioVoice.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
version numbers follow [Semantic Versioning](https://semver.org/).

The section whose heading matches the `VERSION` file becomes the GitHub release
description word for word, so it is written for whoever is deciding whether to
download the installer.

## [0.4.3] — 2026-08-04

### Changed

- **The README shows the window now.** Nothing about the program has changed —
  it is the same 0.4.2 underneath — but anyone deciding whether to download it
  can see what they would be getting first.

## [0.4.2] — 2026-08-04

### Fixed

- **Updates could not install themselves if you had the driver.** The update
  ran, and then stopped at a message box reading *Tick "I understand", or go
  back and clear the driver component* — a wizard page that was not on screen,
  asking for a tick nobody could give. Clicking OK left the old version exactly
  where it was. The page that warns about the unsigned driver is there for
  someone installing it for the first time, and it still refuses to be clicked
  past; it no longer stands in front of an update that is running by itself.

  If you have been stuck on an older version, this is why, and updating from
  here on should simply work. The update still leaves your boot configuration
  alone: test signing is only ever turned on from the wizard, by hand.

## [0.4.1] — 2026-08-03

### Fixed

- **Noise suppression at anything below 100% sounded like a small room.** The
  cleaned signal leaves the model 10 ms behind the original, and the two were
  being mixed as they arrived — so every setting short of full suppression was
  adding your voice to itself, 10 ms late. That is not a blend, it is a comb
  filter, and it is heard as reverb. The original is now held back by exactly
  that much, so the slider does what it says: less suppression, more of the room
  you are actually in, and nothing else. Full suppression was never affected.
- **A built-in module had two switches that did not agree.** The gate, the
  equaliser, the compressor and the suppressor can each be switched off from
  their own panel and from the chain list, and the two were separate pieces of
  state — so one could say off while the other said on, and which one the sound
  followed depended on which you had reached for last. They are one switch now,
  wherever you touch it.

  If you had switched a built-in module off from the chain list, it comes back
  on once: its panel switch is the one that survives the upgrade.

## [0.4.0] — 2026-07-28

### Added

- **The version is on screen now**, right beside the RadioVoice name at the top
  of the window. Until now it lived in the cog menu, which is a fine place to
  keep it and a poor place to find it — and the first thing anyone is asked when
  they report a problem is which version they are running.

## [0.3.2] — 2026-07-28

### Fixed

- **The switch that turns update checking off was hard to find.** It sits in the
  cog menu, but under a heading that said *VERSION* — which is not the word
  anyone scans for when they are looking for it. The section is called *UPDATES*
  now, and the version number is a line inside it rather than the name of it.
- **The options menu could run off the bottom of the screen.** It has grown a log
  line, four notification-area settings, a paragraph and an update section, and a
  popup sizes itself to its contents without being clamped to the display — so on
  a small screen, or a high display scale, the last section was not cut off so
  much as absent. It scrolls now.

## [0.3.1] — 2026-07-28

### Fixed

- The installer's driver warning — the page explaining what test signing costs
  you — was cut off at the bottom of the wizard with no way to scroll, so the
  half that matters most went unread and the two checkboxes below it were off
  the page entirely. The text now sits in a scrolling box, with the checkboxes
  under it where they can be reached.

## [0.3.0] — 2026-07-28

### Added

- **RadioVoice now tells you when there is a newer version.** Once a day it asks
  GitHub which release is the newest, and the cog turns blue when one has
  appeared. Everything about it is under that cog: which build you are running,
  what the new one changes — the release notes are shown in place — and the two
  buttons that act on it.
- **One click downloads it, and one more installs it.** The installer is fetched
  into `%APPDATA%\RadioVoice\updates` and checked against the size and the
  SHA-256 that GitHub publishes for it; anything that disagrees is thrown away.
  Installing closes RadioVoice, runs the installer without a wizard to click
  through, and starts it again — devices, chain, plugin state and settings all
  where you left them.
- Nothing is downloaded or installed on its own. The daily check is one request
  for the release list, and a checkbox in the same place stops even that, after
  which RadioVoice does not touch the network at all.
- With the window hidden — which is where a tray application spends most of its
  life — a downloaded update is announced once by the notification icon, and its
  menu grows an **Install update** item.

Worth knowing before you use it: Windows asks for permission when the installer
runs, and names an unknown publisher. RadioVoice lives under Program Files
because the driver component has to, and the installer is unsigned for the same
reason the driver is. An update is TLS to GitHub plus a published checksum, not a
signature — installing by hand from the releases page remains exactly as good.

## [0.2.3] — 2026-07-28

### Changed

- The **Log** and **Tray** buttons have left the top bar for a cog beside
  Restart. Both were settings, not transport, and the bar you glance at while
  talking is the wrong place for them. The cog turns amber when something has
  gone into the log, so the one thing those buttons were doing at a distance is
  still done.

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
