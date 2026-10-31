"""Check signing choices and release selection without using a private key."""
import importlib.util
import json
import plistlib
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import macos_signing
import package_macos
from release_notes import release_notes


class SigningTests(unittest.TestCase):
    def test_identity_precedence_and_no_silent_unsigned_distribution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'radiovoice'
            root.mkdir()
            sibling = Path(temp) / 'zitter'
            sibling.mkdir()
            config = root / '.radiovoice-signing.json'
            with patch.object(macos_signing, 'ROOT', root), patch.object(macos_signing, 'CONFIG', config), patch.dict('os.environ', {}, clear=True):
                with self.assertRaises(ValueError):
                    macos_signing.resolve_identity()
                self.assertEqual('-', macos_signing.resolve_identity(allow_adhoc=True))
                (sibling / '.zitter-signing.json').write_text(json.dumps({'identity': 'zitter-certificate'}))
                self.assertEqual('zitter-certificate', macos_signing.resolve_identity())
                config.write_text(json.dumps({'identity': 'pinned-certificate'}))
                self.assertEqual('pinned-certificate', macos_signing.resolve_identity())
                with patch.dict('os.environ', {'RADIOVOICE_CODESIGN_IDENTITY': 'environment-certificate'}):
                    self.assertEqual('environment-certificate', macos_signing.resolve_identity())
                    self.assertEqual('explicit-certificate', macos_signing.resolve_identity('explicit-certificate'))
                with self.assertRaises(ValueError):
                    macos_signing.resolve_identity('-')

    def test_permanent_signature_requests_timestamp(self):
        with patch.object(macos_signing.subprocess, 'run') as run:
            macos_signing.sign(Path('/tmp/RadioVoice.app'), 'certificate', 'com.radiovoice.app')
            command = run.call_args.args[0]
            self.assertIn('--timestamp', command)
            self.assertNotIn('--deep', command)

    def test_signed_app_enables_hardened_runtime_with_audio_host_entitlements(self):
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / 'RadioVoice.app'
            (app / 'Contents').mkdir(parents=True)
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'com.radiovoice.app'}))
            with patch.object(macos_signing.subprocess, 'run') as run:
                macos_signing.sign_app(app, 'certificate')
                command = run.call_args_list[0].args[0]
                self.assertIn('runtime', command)
                self.assertIn('--entitlements', command)

    def test_notary_rejection_is_a_failure_and_keeps_diagnostics(self):
        with tempfile.TemporaryDirectory() as temp:
            response = subprocess.CompletedProcess([], 0, '{"status":"Invalid","id":"test-submission"}', '')
            log = subprocess.CompletedProcess([], 0, '{"issues":["missing timestamp"]}', '')
            with patch.object(package_macos, 'ROOT', Path(temp)), patch.dict('os.environ', {
                'MACOS_NOTARY_KEY_PATH': '/tmp/test.p8', 'MACOS_NOTARY_KEY_ID': 'TESTKEYID'
            }, clear=True), patch.object(package_macos.subprocess, 'run', side_effect=[response, log]):
                with self.assertRaises(RuntimeError):
                    package_macos.notarize(Path(temp) / 'RadioVoice.zip')
                self.assertTrue((Path(temp) / 'dist/notarization-test-submission.json').exists())

    def test_notes_stop_at_the_next_version_and_reject_missing_version(self):
        notes = '# Changelog\n## [1.2.3] — Today\n\nNew installer.\n\n## [1.2.2]\nOld notes.'
        self.assertEqual('New installer.\n', release_notes('1.2.3', notes))
        with self.assertRaises(ValueError):
            release_notes('9.0.0', notes)
        with self.assertRaises(ValueError):
            release_notes('invalid', notes)


if __name__ == '__main__':
    unittest.main()
