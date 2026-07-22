#!/usr/bin/env bash
# Build entry for desktop Linux. Run from anywhere:
#   scripts/linux/build.sh [release|debug]     (default: release)
#
# release -> build/linux/         (the shipping build; no dev tooling)
# debug   -> build/linux-debug/   (adds whisper_ui_capture — headless UI PNGs)
set -euo pipefail
cd "$(dirname "$0")/../.."

MODE="${1:-release}"
case "$MODE" in
    release) BUILD_TYPE=Release; DIR=build/linux ;;
    debug)   BUILD_TYPE=Debug;   DIR=build/linux-debug ;;
    *) echo "usage: $0 [release|debug]"; exit 2 ;;
esac

git submodule update --init --recursive

cmake -S . -B "$DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$DIR"

echo
echo "Done ($MODE). Binaries in $DIR/:"
echo "  whisper_destilado       (GUI, Wayland)"
echo "  whisper_destilado_cli   (headless CLI)"
echo "  whisper_tests           (unit tests)"
if [ "$MODE" = debug ]; then
    echo "  whisper_ui_capture      (headless UI snapshots — debug only)"
fi
echo "Place a whisper model (.bin/.gguf) in $DIR/models/"
