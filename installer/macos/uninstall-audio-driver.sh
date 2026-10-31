#!/bin/sh
# Remove only RadioVoice's own HAL bundle. Never touch another virtual cable.
set -eu
if [ "$(/usr/bin/id -u)" != 0 ]; then
    echo 'Run this script with sudo after quitting audio applications.' >&2
    exit 1
fi
driver=/Library/Audio/Plug-Ins/HAL/RadioVoice.driver
if [ ! -e "$driver" ]; then exit 0; fi
if [ -L "$driver" ] || [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$driver/Contents/Info.plist")" != com.radiovoice.audio.driver ]; then
    echo 'Unexpected driver identity; refusing to remove it.' >&2
    exit 1
fi
/bin/rm -rf "$driver"
/usr/sbin/pkgutil --forget com.radiovoice.pkg.driver >/dev/null 2>&1 || true
/usr/bin/killall -TERM coreaudiod || true
echo 'RadioVoice audio driver removed. User settings were preserved.'
