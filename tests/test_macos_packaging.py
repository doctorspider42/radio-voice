"""Check signing choices and release selection without using a private key."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import macos_signing
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

    def test_notes_stop_at_the_next_version_and_reject_missing_version(self):
        notes = '# Changelog\n## [1.2.3] — Today\n\nNew installer.\n\n## [1.2.2]\nOld notes.'
        self.assertEqual('New installer.\n', release_notes('1.2.3', notes))
        with self.assertRaises(ValueError):
            release_notes('9.0.0', notes)
        with self.assertRaises(ValueError):
            release_notes('invalid', notes)


if __name__ == '__main__':
    unittest.main()
