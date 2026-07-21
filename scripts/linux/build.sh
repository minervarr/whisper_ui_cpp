#!/usr/bin/env bash
# Build entry for desktop Linux. Run from anywhere:  scripts/linux/build.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

git submodule update --init --recursive

cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux

echo
echo "Done. Binaries in build/linux/:"
echo "  whisper_destilado       (GUI, Wayland)"
echo "  whisper_destilado_cli   (headless CLI)"
echo "  whisper_tests           (unit tests)"
echo "Place a whisper model (.bin/.gguf) in build/linux/models/"
