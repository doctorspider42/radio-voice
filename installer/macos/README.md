# macOS disk image and signing

The installer is a read-only, compressed DMG with a Retina background, RadioVoice
icon and `/Applications` shortcut. Drag the application to Applications to
install it. BlackHole remains a separate optional installation.

## Build a signed universal installer

Requires macOS, Xcode Command Line Tools, CMake, Ninja and Python 3.10 or newer.
The background and ICNS are committed; Pillow is only needed to regenerate them.

```sh
./build-macos.sh '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
python3 -m venv build-dmg-env
build-dmg-env/bin/pip install -r installer/macos/requirements.txt
build-dmg-env/bin/python tools/package_macos.py
```

The image and its SHA-256 sidecar are written under `dist/`. The filename contains
the version from `VERSION` and the architectures actually present in the app,
not a manually supplied architecture label. The packager rejects stale versions.

It signs a physical copy of the application, including LICENSE and NOTICE.md,
with a timestamp, then signs and verifies the disk image. The original build is
not modified. Before publishing the result, the packager mounts the final image
read-only and checks the app's strict signature, architectures and Applications
link. It does not script Finder or modify macOS security settings.

## Signing identity

The same permanent Developer ID policy as Zitter is used. Precedence:

1. `--identity` (packager), or the `RV_CODESIGN_IDENTITY` CMake cache variable.
2. `RADIOVOICE_CODESIGN_IDENTITY` environment variable.
3. `.radiovoice-signing.json` at the repository root, containing `{"identity":"SHA1"}`.
4. `../zitter/.zitter-signing.json`, when that local checkout exists.

The local preference is ignored by Git; it contains only the certificate
fingerprint, never a private key. The certificate and private key stay in Keychain.
An unavailable configured identity fails signing; it never silently falls back.
Unconfigured development builds may use ad-hoc signing. Distribution packaging
requires an identity unless `--unsigned` is explicitly supplied; those images are
named `*-unsigned.dmg` and are not represented as Developer ID-signed releases.

Signing uses the same permanent Developer ID as Zitter, with a secure timestamp.
The app also enables Hardened Runtime for notarization, with microphone access
and the library-validation exception required to load other vendors' VST3 effects.
No global Gatekeeper bypass is part of the installer.

To notarize, set `MACOS_NOTARY_KEY_PATH` to the App Store Connect `.p8` file,
`MACOS_NOTARY_KEY_ID` to its Key ID, and `MACOS_NOTARY_ISSUER_ID` to its team
Issuer ID (omit issuer for an individual API key). Add `--notarize` to the
packaging command. The packager submits a signed app ZIP, staples the accepted
app ticket, creates/signs the DMG, submits it and staples the image ticket.
Any rejection or timeout fails packaging; the previous installer is preserved.
Rejection logs are retained under `dist/notarization-<submission-id>.json`.

To regenerate artwork from the existing icon source and layout code:

```sh
build-dmg-env/bin/pip install Pillow==13.0.0
build-dmg-env/bin/python tools/make-macos-assets.py
```

## CI and releases

`.github/workflows/release.yml` builds the Windows installer and a universal Mac
DMG on main, `codex/**` pushes, pull requests to main and manual runs. The Mac job
runs on `macos-15`, tests native DSP/portability and packaging choices, and checks
that both architectures exist. Physical-device tests remain local.

Artifacts are uploaded for every successful build. A single publish job on main
waits for both application builds, collects the installers and creates the release
using the matching CHANGELOG section. An existing version is not overwritten.
The Windows driver job remains best effort.

For permanent CI signing configure these repository Actions secrets:

- `MACOS_CERTIFICATE_P12_BASE64`: the existing Developer ID Application identity,
  including its private key, exported as a password-protected PKCS#12 and Base64.
- `MACOS_CERTIFICATE_PASSWORD`: that PKCS#12 password.

For Apple notarization also configure:

- Secret `MACOS_NOTARY_KEY_P8`: the App Store Connect API private key contents.
- Variable `MACOS_NOTARY_KEY_ID`: the corresponding API Key ID.
- Secret `MACOS_NOTARY_ISSUER_ID`: the team Issuer ID (omit for individual keys).

When the notary key is present, signed CI builds require notarization and attach
both tickets before uploading artifacts. Supplying a bad key fails the build.

Optionally pin the expected certificate SHA-1 with the repository Actions variable
`MACOS_CODESIGN_IDENTITY`. It must match the imported identity. To match Zitter,
use the exact same certificate rather than generating a new identity per run.

The workflow imports the certificate into a temporary Keychain restricted to
codesign, signs the app and DMG, and removes the temporary Keychain afterwards.
Signing secrets are exposed only to main builds or a maintainer's manual run
with the `sign_macos` input explicitly enabled, never pull requests or ordinary
branch previews. Credentials are not printed. Supplying these secrets shares signing capability
with that repository's CI; configuring the workflow does not export or upload
local keys. Fork pull requests receive no secrets.

Until a certificate is configured, CI creates explicitly unsigned development
images. No certificate is generated or trusted automatically, and broken supplied
credentials fail the job instead of producing an unsigned substitute.
