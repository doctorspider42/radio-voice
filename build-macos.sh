#!/bin/sh
set -eu
cd "$(dirname "$0")"
cmake --preset macos "$@"
cmake --build --preset macos --parallel
printf '\nBuilt: %s/build-macos/bin/RadioVoice.app\n' "$PWD"
