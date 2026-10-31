# Unified macOS installer and signing

The branded, read-only DMG contains **RadioVoice.pkg**. Double-click it to install
both `/Applications/RadioVoice.app` and
`/Library/Audio/Plug-Ins/HAL/RadioVoice.driver`. The welcome and conclusion are
localized in English and Polish. Administrator authorization is required;
CoreAudio restarts once on the running startup volume, briefly interrupting audio.
The driver exposes a visible **RadioVoice** microphone and a hidden **RadioVoice
Send** output. No separate BlackHole installation is needed. Existing BlackHole
bundles and user settings are preserved. See [driver details](../../driver/macos/README.md).

## Build

Requires macOS 13+, Xcode Command Line Tools, CMake, Ninja and Python 3.10+.

```sh
./build-macos.sh '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
python3 -m venv build-dmg-env
build-dmg-env/bin/pip install -r installer/macos/requirements.txt
build-dmg-env/bin/python tools/package_macos.py --notarize
```

CMake builds the app and HAL bundle with the same architecture list. Packaging
rejects stale versions and mismatched architectures. It stages physical copies,
signs the app and driver, builds two component packages and a single flat product
package, and places the package in the DMG. The original builds are unchanged.
The app is non-relocatable: updates replace /Applications/RadioVoice.app rather
than finding a copy in a build tree or mounted backup. Driver updates retain its
UIDs and replace only our HAL bundle. The uninstall script included in the app
checks the installed driver's bundle ID before removing only that driver.

The packager expands the finished PKG and verifies both executable bundles,
checks the product signature, verifies the DMG, then mounts it read-only and
compares the contained PKG byte-for-byte with the verified package. SHA-256
sidecars are produced. Existing distribution files survive failed builds.

For a local preview while waiting for an Installer certificate:

```sh
build-dmg-env/bin/python tools/package_macos.py --unsigned-installer
```

This still signs app and driver with the configured Application certificate,
but the PKG is unsigned and the image is explicitly named `*-unsigned.dmg`.
It cannot be notarized or published as a production installer. `--unsigned`
uses ad-hoc signatures for both binaries as well. There is no silent fallback.

## Application and driver signatures

Both use **Developer ID Application**, matching Zitter. Identity precedence:

1. `--identity` (packager), or `RV_CODESIGN_IDENTITY` (CMake).
2. `RADIOVOICE_CODESIGN_IDENTITY` environment variable.
3. Local, Git-ignored `.radiovoice-signing.json`: `{"identity":"SHA1"}`.
4. `../zitter/.zitter-signing.json`.

The app uses Hardened Runtime, microphone entitlement and the VST3 library
validation exception. The HAL bundle has Hardened Runtime without application
entitlements. This is a userspace CoreAudio plug-in, not a kernel extension or a
DriverKit extension; no reduced security or DriverKit approval is required.

## Developer ID Installer certificate

The outer flat PKG needs a separate **Developer ID Installer** certificate.
Developer ID Application and the App Store Connect `.p8` key cannot replace it.
Use the same Apple developer team as the existing Application identity.

1. As the team's **Account Holder**, open Apple Developer → Certificates,
   Identifiers & Profiles → Certificates → **+** → **Developer ID Installer**.
   This is different from Mac Installer Distribution (Mac App Store).
2. In Keychain Access, use Certificate Assistant → Request a Certificate from
   a Certificate Authority. Save the CSR to disk and upload it in the portal.
3. Download the `.cer` and open it on the same Mac that generated the CSR.
   In My Certificates, verify that the certificate expands to show its private key.
4. Export the certificate **with its private key** as a password-protected `.p12`.
   Do not commit the certificate, private key or password to the repository.
5. Select the installed identity with `--installer-identity SHA1` or environment
   variable `RADIOVOICE_INSTALLER_IDENTITY`. With exactly one usable Installer
   identity in Keychain, it is selected automatically.

[Apple's certificate instructions](https://developer.apple.com/help/account/certificates/create-developer-id-certificates/).

For CI, add these **new** Actions secrets:

- `MACOS_INSTALLER_CERTIFICATE_P12_BASE64`: Base64-encoded Installer `.p12`.
- `MACOS_INSTALLER_CERTIFICATE_PASSWORD`: its password.

Optionally pin its SHA-1 in repository variable `MACOS_INSTALLER_IDENTITY`.
The existing `MACOS_CERTIFICATE_P12_BASE64`, `MACOS_CERTIFICATE_PASSWORD` and
`MACOS_CODESIGN_IDENTITY` continue to identify Developer ID Application.
Set a password interactively without putting it in chat or shell history:

```sh
gh secret set MACOS_INSTALLER_CERTIFICATE_PASSWORD --repo doctorspider42/radio-voice
```

## Notarization and CI

Existing notary credentials remain valid: `MACOS_NOTARY_KEY_PATH`,
`MACOS_NOTARY_KEY_ID`, and `MACOS_NOTARY_ISSUER_ID` (omit issuer for individual keys).
`--notarize` submits/staples the app, the signed PKG containing the signed driver,
and the signed DMG. Rejections and timeouts fail the build. Rejection logs stay in
`dist/notarization-<submission-id>.json`.

CI uses `MACOS_NOTARY_KEY_P8` and `MACOS_NOTARY_ISSUER_ID` secrets plus
`MACOS_NOTARY_KEY_ID` variable. The Application and Installer identities are
imported into a temporary Keychain, which is deleted afterwards. Signing secrets
are available only on main and explicitly signed manual builds. Those builds
require both certificates and notarization; missing credentials fail the job.
Ordinary branch/PR builds produce explicitly unsigned development images.

CI builds both architectures and tests portability, actual HAL loopback, hidden
output properties, and packaging logic. Physical microphone tests remain local.
Main publishes a release only after both Windows and Mac builds succeed. Do not
merge this installer migration until Installer signing secrets are configured.

The original ICNS and Retina background are committed. Regenerate them with
`build-dmg-env/bin/pip install Pillow==13.0.0` and
`build-dmg-env/bin/python tools/make-macos-assets.py` (Python 3.11+).
