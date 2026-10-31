#!/usr/bin/env python3
"""Build a branded drag-to-Applications DMG without Finder UI automation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

from macos_signing import ROOT, resolve_identity, sign, sign_app


def architectures(app):
    result = subprocess.check_output(['/usr/bin/lipo', '-archs', str(app / 'Contents/MacOS/RadioVoice')], text=True)
    archs = set(result.split())
    if archs == {'arm64', 'x86_64'}:
        return 'universal'
    if archs in ({'arm64'}, {'x86_64'}):
        return next(iter(archs))
    raise ValueError('Unsupported application architectures: ' + result.strip())


def notarize(archive):
    key = os.environ.get('MACOS_NOTARY_KEY_PATH')
    key_id = os.environ.get('MACOS_NOTARY_KEY_ID')
    issuer = os.environ.get('MACOS_NOTARY_ISSUER_ID')
    if not key or not key_id:
        raise ValueError('Notarization requires MACOS_NOTARY_KEY_PATH and MACOS_NOTARY_KEY_ID.')
    auth = ['--key', key, '--key-id', key_id]
    if issuer:
        auth += ['--issuer', issuer]
    result = subprocess.run(['xcrun', 'notarytool', 'submit', str(archive), *auth,
        '--wait', '--timeout', '30m', '--output-format', 'json'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('Apple notarization failed: ' + result.stderr.strip())
    response = json.loads(result.stdout)
    if response.get('status') != 'Accepted':
        log = subprocess.run(['xcrun', 'notarytool', 'log', response['id'], *auth], capture_output=True, text=True)
        log_path = ROOT / 'dist' / ('notarization-' + response['id'] + '.json')
        log_path.parent.mkdir(exist_ok=True)
        log_path.write_text(log.stdout or log.stderr)
        raise RuntimeError('Apple rejected notarization; see ' + str(log_path))
    print('Apple notarization accepted: ' + response['id'], flush=True)


def package(app, output, identity, notarization=False):
    import dmgbuild
    if notarization and identity == '-':
        raise ValueError('Notarization requires a permanent Developer ID signature.')
    app = app.resolve()
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('VERSION must contain a semantic version.')
    with (app / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    if info.get('CFBundleShortVersionString') != version or info.get('CFBundleIdentifier') != 'com.radiovoice.app':
        raise ValueError('Build the current RadioVoice version before packaging.')
    arch = architectures(app)
    filename = f'RadioVoice-{version}-macos-{arch}' + ('-unsigned' if identity == '-' else '') + '.dmg'
    output.mkdir(parents=True, exist_ok=True)
    target = output / filename
    # A physical copy prevents signing from mutating the running build's files.
    # Complete the temporary DMG before replacing the previous distribution.
    with tempfile.TemporaryDirectory(prefix='.radiovoice-dmg-', dir=output) as temp:
        stage = Path(temp)
        staged_app = stage / 'RadioVoice.app'
        shutil.copytree(app, staged_app, symlinks=True)
        resources = staged_app / 'Contents/Resources'
        resources.mkdir(exist_ok=True)
        for name in ('LICENSE', 'NOTICE.md'):
            shutil.copy2(ROOT / name, resources / name)
        sign_app(staged_app, identity)
        if notarization:
            archive = stage / 'RadioVoice.zip'
            subprocess.run(['/usr/bin/ditto', '-c', '-k', '--keepParent', str(staged_app), str(archive)], check=True)
            notarize(archive)
            subprocess.run(['xcrun', 'stapler', 'staple', str(staged_app)], check=True)
            subprocess.run(['xcrun', 'stapler', 'validate', str(staged_app)], check=True)
        dmg = stage / filename
        dmgbuild.build_dmg(str(dmg), 'RadioVoice', settings={
            'format': 'UDZO', 'filesystem': 'HFS+',
            'files': [str(staged_app)], 'symlinks': {'Applications': '/Applications'},
            'background': str(ROOT / 'res/dmg-background.tiff'),
            'icon': str(ROOT / 'res/RadioVoice.icns'),
            'window_rect': ((160, 160), (720, 532)),
            'default_view': 'icon-view', 'include_icon_view_settings': True,
            'include_list_view_settings': False, 'icon_size': 104, 'text_size': 14,
            'icon_locations': {'RadioVoice.app': (190, 270), 'Applications': (530, 270)},
            'show_icon_preview': False,
            'show_status_bar': False, 'show_tab_view': False, 'show_toolbar': False,
            'show_pathbar': False, 'show_sidebar': False,
        })
        if identity != '-':
            sign(dmg, identity, 'com.radiovoice.diskimage')
            subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(dmg)], check=True)
        if notarization:
            notarize(dmg)
            subprocess.run(['xcrun', 'stapler', 'staple', str(dmg)], check=True)
            subprocess.run(['xcrun', 'stapler', 'validate', str(dmg)], check=True)
        subprocess.run(['/usr/bin/hdiutil', 'verify', str(dmg)], check=True)
        # Finder metadata written while assembling a DMG can invalidate a valid
        # app signature. Verify the actual read-only payload, not only staging.
        mount = stage / 'verify'
        mount.mkdir()
        subprocess.run(['/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', str(mount), str(dmg)], check=True)
        try:
            subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(mount / 'RadioVoice.app')], check=True)
            if not (mount / 'Applications').is_symlink() or os.readlink(mount / 'Applications') != '/Applications':
                raise ValueError('The Applications drop target is missing or incorrect.')
            if architectures(mount / 'RadioVoice.app') != arch:
                raise ValueError('The packaged architecture differs from the build.')
        finally:
            subprocess.run(['/usr/bin/hdiutil', 'detach', str(mount)], check=True)
        os.replace(dmg, target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    (output / (filename + '.sha256')).write_text(f'{digest}  {filename}\n')
    print(json.dumps({'dmg': str(target), 'architecture': arch, 'signed': identity != '-', 'notarized': notarization, 'sha256': digest}, indent=2))
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=ROOT / 'build-macos/bin/RadioVoice.app')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    signing = parser.add_mutually_exclusive_group()
    signing.add_argument('--identity')
    signing.add_argument('--unsigned', action='store_true', help='Explicit ad-hoc development image, marked unsigned in its filename')
    parser.add_argument('--notarize', action='store_true', help='Submit to Apple and staple tickets to both app and DMG')
    args = parser.parse_args()
    identity = '-' if args.unsigned else resolve_identity(args.identity)
    package(args.app, args.output.resolve(), identity, notarization=args.notarize)


if __name__ == '__main__':
    main()
