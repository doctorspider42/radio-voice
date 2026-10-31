#!/usr/bin/env python3
"""Extract this version's release notes without platform-specific shell logic."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def release_notes(version, changelog):
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('VERSION must contain a semantic version.')
    heading = re.search(r'^##\s*\[' + re.escape(version) + r'\].*$', changelog, re.MULTILINE)
    if not heading:
        raise ValueError('CHANGELOG.md has no section for ' + version)
    remainder = changelog[heading.end():]
    next_heading = re.search(r'^##\s*\[', remainder, re.MULTILINE)
    notes = remainder[:next_heading.start()] if next_heading else remainder
    if not notes.strip():
        raise ValueError('The release notes are empty.')
    return notes.strip() + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(release_notes((ROOT / 'VERSION').read_text().strip(), (ROOT / 'CHANGELOG.md').read_text()), encoding='utf-8')
