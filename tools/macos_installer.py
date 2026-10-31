#!/usr/bin/env python3
"""Build the unified, root-volume-only application and HAL installer."""
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
from xml.sax.saxutils import escape
from macos_signing import ROOT
from build_macos_driver import identity as driver_identity


def resolve_installer_identity(explicit=None):
    requested = explicit or os.environ.get('RADIOVOICE_INSTALLER_IDENTITY')
    # Installer identities do not pass the codesigning policy used for apps.
    listing = subprocess.check_output(['security', 'find-identity', '-v', '-p', 'basic'], text=True)
    candidates = re.findall(r'([A-Fa-f0-9]{40}) "(Developer ID Installer:[^"]+)"', listing)
    if requested:
        matches = [fingerprint for fingerprint, name in candidates if requested in (fingerprint, name)]
    else:
        matches = [fingerprint for fingerprint, _ in candidates]
    if len(matches) != 1:
        raise ValueError('Select a valid Developer ID Installer identity with --installer-identity or '
                         'RADIOVOICE_INSTALLER_IDENTITY. For a development preview only, use --unsigned-installer.')
    return matches[0]


def verify_driver(driver, arch):
    info = plistlib.loads((driver / 'Contents/Info.plist').read_bytes())
    ids = driver_identity()
    if (info.get('CFBundleIdentifier') != ids['BUNDLE_ID'] or
        info.get('CFBundleShortVersionString') != (ROOT / 'VERSION').read_text().strip() or
        info.get('CFPlugInFactories') != {ids['FACTORY_UUID']: 'RadioVoice_Create'}):
        raise ValueError('Build the current RadioVoice driver before packaging.')
    expected = {'arm64', 'x86_64'} if arch == 'universal' else {arch}
    actual = set(subprocess.check_output(['lipo', '-archs', str(driver / 'Contents/MacOS/RadioVoiceDriver')], text=True).split())
    if actual != expected:
        raise ValueError('App and driver architectures must match.')
    subprocess.run(['codesign', '--verify', '--strict', str(driver)], check=True)


def build_pkg(stage, app, driver, version, installer_identity):
    payload = stage / 'payload'
    app_root, driver_root = payload / 'app', payload / 'driver'
    (app_root / 'Applications').mkdir(parents=True)
    shutil.copytree(app, app_root / 'Applications/RadioVoice.app', symlinks=True)
    hal = driver_root / 'Library/Audio/Plug-Ins/HAL'
    hal.mkdir(parents=True)
    shutil.copytree(driver, hal / 'RadioVoice.driver', symlinks=True)
    # Clear inherited Finder/privacy metadata from the staged copies.
    subprocess.run(['xattr', '-cr', str(payload)], check=True)
    packages = stage / 'packages'
    packages.mkdir()
    # Never relocate installation to a build tree or mounted backup containing
    # another copy of RadioVoice.app; updates always replace the canonical path.
    components = stage / 'app-components.plist'
    components.write_bytes(plistlib.dumps([{'RootRelativeBundlePath': 'Applications/RadioVoice.app',
        'BundleIsRelocatable': False, 'BundleIsVersionChecked': True,
        'BundleOverwriteAction': 'upgrade', 'BundleHasStrictIdentifier': True}]))
    for name, root in [('app', app_root), ('driver', driver_root)]:
        command = ['pkgbuild', '--root', str(root), '--identifier', 'com.radiovoice.pkg.' + name,
            '--version', version, '--install-location', '/', '--ownership', 'recommended']
        if name == 'app':
            command += ['--component-plist', str(components)]
        else:
            command += ['--scripts', str(ROOT / 'installer/macos/scripts')]
        subprocess.run([*command, str(packages / (name + '.pkg'))], check=True)
    resources = stage / 'installer-resources'
    for language, title, welcome, conclusion in [
        ('en', 'Install RadioVoice',
         'Installs RadioVoice and its virtual microphone. Audio will briefly stop during installation. '
         'Quit RadioVoice and other audio applications first. Existing BlackHole installations and presets are preserved.',
         'Open RadioVoice from Applications and choose your microphone. In Discord, OBS or Teams, select RadioVoice as the input. '
         'The sending output is hidden automatically. Restart receiving applications if needed.'),
        ('pl', 'Zainstaluj RadioVoice',
         'Instaluje RadioVoice i jego wirtualny mikrofon. Podczas instalacji dźwięk na chwilę zostanie przerwany. '
         'Najpierw zamknij RadioVoice i inne aplikacje audio. Istniejący BlackHole i presety zostaną zachowane.',
         'Otwórz RadioVoice z Aplikacji i wybierz swój mikrofon. W Discordzie, OBS lub Teams wybierz wejście RadioVoice. '
         'Wyjście wysyłające jest automatycznie ukryte. W razie potrzeby uruchom ponownie aplikacje odbierające dźwięk.')]:
        localized = resources / (language + '.lproj')
        localized.mkdir(parents=True)
        for filename, text in [('welcome.html', welcome), ('conclusion.html', conclusion)]:
            (localized / filename).write_text('<!doctype html><html><meta charset="utf-8"><body '
                'style="font:14px -apple-system"><h1>' + escape(title) + '</h1><p>' + escape(text) + '</p></body></html>')
    shutil.copy2(ROOT / 'LICENSE', resources / 'license.txt')
    distribution = stage / 'Distribution.xml'
    distribution.write_text('''<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>RadioVoice</title>
  <welcome file="welcome.html"/>
  <conclusion file="conclusion.html"/>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
  <volume-check><allowed-os-versions><os-version min="13.0"/></allowed-os-versions></volume-check>
  <choices-outline><line choice="app"/><line choice="driver"/></choices-outline>
  <choice id="app" visible="false"><pkg-ref id="com.radiovoice.pkg.app"/></choice>
  <choice id="driver" visible="false"><pkg-ref id="com.radiovoice.pkg.driver"/></choice>
  <pkg-ref id="com.radiovoice.pkg.app" version="''' + escape(version) + '''">app.pkg</pkg-ref>
  <pkg-ref id="com.radiovoice.pkg.driver" version="''' + escape(version) + '''">driver.pkg</pkg-ref>
</installer-gui-script>
''')
    target = stage / 'RadioVoice.pkg'
    command = ['productbuild', '--distribution', str(distribution), '--resources', str(resources),
        '--package-path', str(packages)]
    if installer_identity:
        command += ['--sign', installer_identity, '--timestamp']
    subprocess.run([*command, str(target)], check=True)
    if installer_identity:
        subprocess.run(['pkgutil', '--check-signature', str(target)], check=True)
    # Expand the actual flat package and verify its signed executable payloads.
    expanded = stage / 'expanded'
    subprocess.run(['pkgutil', '--expand-full', str(target), str(expanded)], check=True)
    for bundle in (expanded / 'app.pkg/Payload/Applications/RadioVoice.app',
                   expanded / 'driver.pkg/Payload/Library/Audio/Plug-Ins/HAL/RadioVoice.driver'):
        subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
    return target
