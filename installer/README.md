# RadioVoice installer

Inno Setup 7 script producing `dist/RadioVoice-<version>-setup.exe`, a 64-bit
installer.

The installer is the one part of the project that is translated: its
`[CustomMessages]` carry English and Polish, and Inno Setup picks by the
system's language. Everything else — this file included — is English.

```
winget install --id JRSoftware.InnoSetup.7 --exact
make-installer.cmd
```

Note the `.7`. The plain `JRSoftware.InnoSetup` package is still 6.x, and
Chocolatey's `innosetup` tops out at 6.7.1; neither knows `SetupArchitecture`.
Seven installs alongside six, so there is nothing to remove first.

`make-installer.cmd` builds the application, stages the driver if there is one
to stage, and compiles the script. `make-installer.cmd no-build` skips straight
to packaging whatever is already in `build\bin`.

---

## What it installs

| Component | Default | Contents |
|---|---|---|
| `app` | always | `RadioVoice.exe`, the licence, the documentation |
| `driver` | off | the virtual audio cable — **only when a payload was staged** |

Two optional tasks: a desktop shortcut, and starting with Windows.

"Start with Windows" writes `RadioVoice` under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, pointing at the installed
executable with `--minimized`. That is the same value the application's own
**Tray → Start with Windows** checkbox writes, deliberately: one entry, so the
two cannot disagree about whether autostart is on.

The installer does not write that value itself. It runs elevated, so `HKCU`
inside it is the hive of whoever answered the UAC prompt — not necessarily the
person signing in. Instead the task runs `RadioVoice.exe --enable-autostart`
with Inno's `runasoriginaluser` flag, and the application writes its own entry
as the real user. `[UninstallRun]` does the reverse with `--disable-autostart`,
though without `runasoriginaluser`, which that section does not accept.

---

## The version

Read from `VERSION` at the repository root, by the script itself:

```
#define VersionHandle FileOpen(SourcePath + "\..\VERSION")
#define AppVersion    Trim(FileRead(VersionHandle))
```

There is no version written down in the `.iss`, and there should never be one.
See the *Versioning* section of `CLAUDE.md`.

`AppId` is a different matter: it is fixed forever. It is what tells Windows
that a new build upgrades the old one instead of installing beside it.

---

## The driver payload

The driver component exists in the compiled installer only if
`installer\payload\driver` held a complete, signed package at compile time.

```
make-installer.cmd with-driver
```

is the one-command route: `installer\build-driver-payload.ps1` builds the
driver, reuses or creates the certificate, signs, and stages — then the
installer is compiled around it. It elevates itself, because the signing key
lives in `Cert:\LocalMachine\My` and `signtool` cannot open it otherwise. Note
that on a machine with no certificate yet, that step creates one **and trusts it
machine-wide**, exactly as `install-driver.cmd` does.

It uses `-PassThru` and `WaitForExit()` rather than `Start-Process -Wait`.
`-Wait` waits for the whole process tree, and this one shells out to `cl`,
`link`, `signtool` and `Inf2Cat`; the elevated run would finish and the caller
would sit there indefinitely.

`installer\stage-driver.ps1` is the staging half on its own, and is what a plain
`make-installer.cmd` runs. It copies out of `driver\build`:

```
RadioVoiceAudio.sys      built    driver\build-driver.cmd
RadioVoiceAudio.inf      built    driver\build-driver.cmd
RadioVoiceAudio.cat      signed   driver\tools\sign.ps1
RadioVoiceTest.cer       created  driver\tools\make-test-cert.ps1
tools\*.ps1              copied   driver\tools
```

Miss any of them and staging prints what is missing and exits **0** — an
application-only installer is a legitimate thing to produce, and it is what a
machine without the WDK will always produce. The `#if FileExists(...)` in the
`.iss` is what turns the component off.

The payload is a build product and is not committed.

### What the installer does with it

In order, from `CurStepChanged(ssPostInstall)`:

1. `bcdedit /set testsigning on`, **only** if the user ticked that box.
2. `tools\trust-cert.ps1` — puts the `.cer` into `LocalMachine\Root` and
   `LocalMachine\TrustedPublisher`.
3. `tools\install.ps1` — `pnputil /add-driver`, then creates the
   root-enumerated device.

Because the installer is 64-bit there is no WOW64 redirection to work around;
a 32-bit one would resolve `powershell.exe` to the SysWOW64 copy, whose
`System32` has no `pnputil.exe`. `PowerShellPath` still names the path outright
so that this stays true if the architecture is ever changed back.

Step 3 does not need `devcon.exe`. devcon ships only with the WDK, so where it
is absent `tools\RootDevice.ps1` makes the same SetupAPI calls directly
(`SetupDiCreateDeviceInfo` → `SPDRP_HARDWAREID` → `DIF_REGISTERDEVICE` →
`UpdateDriverForPlugAndPlayDevices`). Uninstalling likewise falls back from
`devcon remove` to `Remove-PnpDevice`.

### The warning page

Shown between the task page and the ready page, and only when the driver
component is selected. It cannot be clicked past: `NextButtonClick` refuses to
advance until "I understand" is ticked.

That is deliberate, and it should stay that way. The page is asking for two
things that genuinely weaken the machine — test signing, and a certificate in
the machine's trusted root store — and it says so in those words rather than in
reassuring ones. If the wording ever gets softened, the page has stopped doing
its job.

With one exception: a silent Setup. `NextButtonClick` is called on silent
installs too — Inno Setup simulates the clicks — and there the checkbox is one
nobody was in a position to tick, so refusing to advance only kills the
install. That is what the application's own updater ran into: it starts Setup
with `/SILENT`, and every update of an installation that includes the driver
stopped dead at a message box, on a wizard that was not on screen.

So `NextButtonClick` returns early when `WizardSilent`. Consent is not being
inferred from silence: a silent run touches the driver only where the driver
component is selected, which on an upgrade is the choice made on this page the
first time. It also never turns test signing on — the second checkbox is False
in silent mode and stays that way, so an unattended update cannot rewrite a
boot configuration or ask for a restart.

For the same reason every message box in the driver install path is a
`SuppressibleMsgBox`, not a `MsgBox`. `/SUPPRESSMSGBOXES` has no effect on a
plain `MsgBox`, and a dialog waiting behind a wizard nobody can see is
indistinguishable from a hang.

---

## CI

`.github/workflows/release.yml` builds this on every push to `main`.

Inno Setup comes from winget rather than Chocolatey, because Chocolatey has no
7.x package.

The application is built with MinGW, by calling `build.cmd` — the same command
and the same compiler used locally. MSVC would need no toolchain install, but
the `msvc` preset does not currently build the dependencies (rnnoise's SSE4.1
flags, and `char8_t` in the VST3 SDK's `module_win32.cpp`), and a release is
the wrong place to discover that.

The driver is built by a separate job that installs the WDK, signs with a
certificate generated in the runner and thrown away with it, and is marked
`continue-on-error`. When it fails, the release still goes out — without the
driver component.
