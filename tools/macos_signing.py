#!/usr/bin/env python3
"""Sign with a pinned Developer ID, matching Zitter's local packaging policy."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / '.radiovoice-signing.json'


def resolve_identity(explicit=None, allow_adhoc=False):
    identity = explicit or os.environ.get('RADIOVOICE_CODESIGN_IDENTITY')
    if not identity and CONFIG.exists():
        identity = json.loads(CONFIG.read_text()).get('identity')
    if not identity:
        sibling = ROOT.parent / 'zitter/.zitter-signing.json'
        if sibling.exists():
            identity = json.loads(sibling.read_text()).get('identity')
    if identity == '-' and not allow_adhoc:
        raise ValueError('A permanent signing identity is required; use --unsigned for a development DMG.')
    if not identity:
        if allow_adhoc:
            return '-'
        raise ValueError('Set RADIOVOICE_CODESIGN_IDENTITY or .radiovoice-signing.json; no certificate is configured.')
    return identity


def sign(path, identity, identifier=None, runtime=False):
    command = ['/usr/bin/codesign', '--force', '--sign', identity]
    if identity != '-':
        command += ['--timestamp']
        if runtime:
            command += ['--options', 'runtime', '--entitlements', str(ROOT / 'res/macos-entitlements.plist')]
    if identifier:
        command += ['--identifier', identifier]
    subprocess.run(command + [str(path)], check=True)


def sign_app(app, identity):
    app = Path(app)
    with (app / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    if info['CFBundleIdentifier'] != 'com.radiovoice.app':
        raise ValueError('Refusing to sign a bundle other than RadioVoice.')
    # This app has a statically linked executable and no nested code. Refuse
    # unexpected code rather than using --deep and sealing it accidentally.
    if (app / 'Contents/Frameworks').exists() or (app / 'Contents/PlugIns').exists():
        raise ValueError('Nested code needs explicit inside-out signing before distribution.')
    sign(app, identity, 'com.radiovoice.app', runtime=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--identity')
    parser.add_argument('--allow-adhoc', action='store_true')
    args = parser.parse_args()
    identity = resolve_identity(args.identity, args.allow_adhoc)
    print('Signing RadioVoice with ' + ('ad-hoc development signature' if identity == '-' else identity), flush=True)
    sign_app(args.app, identity)


if __name__ == '__main__':
    main()
