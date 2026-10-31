#!/usr/bin/env python3
"""Build the independently branded GPL BlackHole fork as a universal HAL bundle."""
import argparse
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
from macos_signing import ROOT


def identity():
    return dict(re.findall(r'^#define RV_AUDIO_(\w+) "([^"]+)"',
        (ROOT / 'driver/macos/Identity.h').read_text(), re.M))


def build(output, archs):
    ids = identity()
    version = (ROOT / 'VERSION').read_text().strip()
    contents = output / 'Contents'
    (contents / 'MacOS').mkdir(parents=True, exist_ok=True)
    resources = contents / 'Resources'
    resources.mkdir(exist_ok=True)
    command = ['xcrun', 'clang', '-bundle', '-fblocks', '-Wno-format-extra-args', '-O2', '-mmacosx-version-min=13.0']
    for arch in archs:
        if arch not in ('arm64', 'x86_64'):
            raise ValueError('Unsupported driver architecture: ' + arch)
        command += ['-arch', arch]
    subprocess.run([*command, '-include', str(ROOT / 'driver/macos/RadioVoiceConfig.h'),
        str(ROOT / 'driver/macos/upstream/BlackHole.c'), '-framework', 'CoreAudio',
        '-framework', 'CoreFoundation', '-framework', 'Accelerate',
        '-o', str(contents / 'MacOS/RadioVoiceDriver')], check=True)
    info = {'CFBundleIdentifier': ids['BUNDLE_ID'], 'CFBundleExecutable': 'RadioVoiceDriver',
        'CFBundleName': ids['NAME'], 'CFBundlePackageType': 'BNDL',
        'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleVersion': version,
        'CFBundleShortVersionString': version, 'CFBundleIconFile': 'RadioVoice.icns',
        'LSMinimumSystemVersion': '13.0',
        'CFPlugInFactories': {ids['FACTORY_UUID']: 'RadioVoice_Create'},
        'CFPlugInTypes': {'443ABAB8-E7B3-491A-B985-BEB9187030DB': [ids['FACTORY_UUID']]}}
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    shutil.copy2(ROOT / 'res/RadioVoice.icns', resources)
    for source, name in [('driver/macos/upstream/LICENSE', 'BlackHole-LICENSE'),
                         ('driver/macos/README.md', 'Driver-README.md')]:
        shutil.copy2(ROOT / source, resources / name)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(output)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archs', default='arm64;x86_64')
    args = parser.parse_args()
    build(args.output.resolve(), args.archs.split(';'))
