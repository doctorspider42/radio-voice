# RadioVoice virtual microphone for macOS

This is an independently branded fork of [BlackHole](https://github.com/ExistentialAudio/BlackHole),
Copyright Existential Audio Inc., licensed under GPLv3 (see upstream/LICENSE).
Vendored commit: `574029f0d3175ccf29a826d74be4da3967b1cb92`.
`upstream/BlackHole.c` changes the fallback box/manufacturer branding and fixes
inverted CFStringRef qualifier-size checks in UID-to-box/device translation.
It also assigns stream/volume/mute ownership to the device exposing that
scope and fixes mirror control enumeration. These adaptations are needed for
our split input-only/output-only layout. The HAL regression test checks the
published ownership and both valid and malformed device UID requests. RadioVoiceConfig.h contains our configuration;
Identity.h and tools/build_macos_driver.py supply our own factory, bundle and artwork.
No BlackHole branding or icon is shipped as our product branding.

The visible **RadioVoice** device has two input channels at 48 kHz. Select it as
Discord/OBS/Teams' microphone. **RadioVoice Send** has two output channels and
`kAudioDevicePropertyIsHidden=1`; RadioVoice resolves it by its stable UID, not
by ordinary device enumeration. Hiding is a user interface property, not access
control: other applications can still resolve the UID. There is no kernel
extension, DriverKit entitlement or reduced boot security requirement.

The unified installer places RadioVoice.app in /Applications and RadioVoice.driver
in /Library/Audio/Plug-Ins/HAL. It restarts CoreAudio once after installation on the
current startup volume; ongoing audio is briefly interrupted. Existing BlackHole
installations and user settings are preserved. Restart receiving applications if
the new microphone does not immediately appear. Upgrades keep the same bundle ID,
UIDs and installation paths. Keep a previous signed installer available for recovery.

Build with CMake on macOS, or run tools/build_macos_driver.py --output
build-macos/driver/RadioVoice.driver. The application and HAL bundle use Developer
ID Application signatures; the flat installer additionally needs Developer ID
Installer. Both architectures are built from the vendored source. The release
source archive contains the corresponding driver source and build scripts.

To remove only this driver, quit RadioVoice and receiving audio applications,
then run `sudo /Applications/RadioVoice.app/Contents/Resources/uninstall-audio-driver.sh`.
This checks the bundle ID before deleting only our HAL bundle and restarting
CoreAudio. User presets and other audio drivers are retained.
