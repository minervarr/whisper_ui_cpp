#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux (Release) or build/linux-debug (Debug)
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, ALSA headers, jack2 headers
# (NOT pipewire-jack), wayland-client/wayland-cursor/xkbcommon dev headers,
# a Vulkan loader + headers, and the Slang shader compiler (slangc).
#
# Usage: scripts/linux/build.sh [release|debug|--share|--packages] [--clean] [cmake args...]
# Passing a mode explicitly (scripts, CI) always skips straight to the
# build — same for non-interactive stdin (defaults to Release, v3).
#
# Run with no mode flag on an interactive terminal and two prompts run in
# sequence — microarchitecture target, then build type — since they're
# orthogonal:
#   Scene 1 — microarch target:
#     1) v3 (default)  -- AVX2/BMI2/FMA baseline (Haswell 2013+, Zen 2+) --
#                          this is what the project has always shipped
#     2) Universal      -- true x86-64 baseline (SSE2 only), any 64-bit CPU
#     3) Native          -- tuned to this exact CPU (-march=native)
#     4) Custom          -- enter any -march value (v4/znver4/...)
#     5) All              -- build universal/v3/v4/zen4 in one pass
#     6) Packages          -- the same four as an Arch package, for upload
#   Scene 2 — build type (skipped for Packages, which is Release by definition):
#     1) Release (default)
#     2) Debug
#
# Release: only whisper_destilado, whisper_destilado_cli, whisper_tests --
#   whisper_ui_capture (headless UI snapshots) is Debug-only.
# Debug: everything, LTO off, debug info on, no optimization -- for stepping
#   through the app. GPU inference is unaffected by WHISPER_ARCH_LEVEL either
#   way -- it always runs through Vulkan; the level only changes ggml's CPU
#   kernels (mel spectrogram, resampling) and any op without a Vulkan path.
# All: builds four binaries -- Universal plus v3/v4 plus a Zen4-tuned build
#   (see WHISPER_ARCH_LEVEL in the root CMakeLists.txt) -- so you can hand out
#   whichever one matches the recipient's CPU instead of one binary. Each
#   variant gets its own build dir under build/linux_share/. Release variants
#   are packaged as tarballs under dist/linux/; Debug variants (an "All" +
#   Debug combo picked interactively) are left unpackaged in
#   build/linux_share_debug/ -- Debug output isn't the kind of thing you hand
#   someone.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_TYPE=Release
SHARE=0
PACKAGES=0
CLEAN=0
MODE_SET=0
ARCH_LEVEL=""
ARCH_SUFFIX=""
CMAKE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        debug|--debug)     BUILD_TYPE=Debug; MODE_SET=1 ;;
        release|--release) BUILD_TYPE=Release; MODE_SET=1 ;;
        --share)           SHARE=1; MODE_SET=1 ;;
        --packages)        PACKAGES=1; MODE_SET=1 ;;
        --clean)           CLEAN=1 ;;
        *)                 CMAKE_ARGS+=("$arg") ;;
    esac
done

# No mode flag given: ask, if there's actually someone at the keyboard to
# answer (stdin a tty) -- a non-interactive caller (CI, a pipe) falls through
# to the Release/v3 default above instead of hanging on `read`.
if [[ "$MODE_SET" -eq 0 && -t 0 ]]; then
    echo "Select microarchitecture target:"
    echo "  1) v3 (default) -- AVX2/BMI2/FMA baseline (Haswell 2013+, Zen 2+)"
    echo "  2) Universal -- true x86-64 baseline (SSE2 only), any 64-bit CPU"
    echo "  3) Native -- tuned to this exact CPU (-march=native)"
    echo "  4) Custom -- enter a specific -march value (v4/znver4/...)"
    echo "  5) All -- build universal/v3/v4/zen4 in one pass"
    echo "  6) Packages -- the same four as an Arch package, for upload"
    read -r -p "Enter choice [1-6, default 1]: " arch_choice
    case "$arch_choice" in
        ""|1) ;;
        2) ARCH_LEVEL="universal"; ARCH_SUFFIX="_universal" ;;
        3) ARCH_LEVEL="native"; ARCH_SUFFIX="_native" ;;
        4)
            read -r -p "Enter -march value (e.g. v4, znver4): " custom_level
            if [[ -z "$custom_level" ]]; then
                echo "error: no value entered" >&2
                exit 2
            fi
            ARCH_LEVEL="$custom_level"
            ARCH_SUFFIX="_custom-${custom_level}"
            ;;
        5) SHARE=1 ;;
        6) PACKAGES=1 ;;
        *) echo "error: invalid choice '$arch_choice'" >&2; exit 2 ;;
    esac

    # Packages are Release by definition -- the PKGBUILD hardcodes it, and a
    # Debug package (symbols, no LTO, smoke-test tools) is not something you
    # hand anyone. Skip the question rather than ask one whose answer is
    # ignored.
    if [[ "$PACKAGES" -eq 0 ]]; then
        echo "Select build type:"
        echo "  1) Release (default)"
        echo "  2) Debug"
        read -r -p "Enter choice [1-2, default 1]: " type_choice
        case "$type_choice" in
            ""|1) BUILD_TYPE=Release ;;
            2)     BUILD_TYPE=Debug ;;
            *)     echo "error: invalid choice '$type_choice'" >&2; exit 2 ;;
        esac
    fi
fi

git submodule update --init --recursive

# vk_canvas resolves slangc from $VULKAN_SDK/bin/slangc, falling back to a
# hardcoded Windows path if VULKAN_SDK is unset. Point at it explicitly
# unless the caller already passed -DVCE_SLANGC or has VULKAN_SDK set.
SLANGC_ARG=()
if [[ "${CMAKE_ARGS[*]:-}" != *"VCE_SLANGC"* && -z "${VULKAN_SDK:-}" ]]; then
    if command -v slangc >/dev/null 2>&1; then
        SLANGC_ARG=(-DVCE_SLANGC="$(command -v slangc)")
    elif [[ -x /opt/shader-slang/bin/slangc ]]; then
        SLANGC_ARG=(-DVCE_SLANGC=/opt/shader-slang/bin/slangc)
    fi
fi

if [[ "$PACKAGES" -eq 1 ]]; then
    # The Arch-package sibling of --share: same four microarch variants, but
    # built by makepkg into installable .pkg.tar.zst files instead of tarballs.
    #
    # This script does NOT rebuild anything itself here. Everything -- the
    # four configures and the four self-contained packages -- lives in
    # packaging/arch/PKGBUILD, because that is where a person reading the
    # package expects to find it, and because makepkg has to own $srcdir for
    # its checksums to mean anything.
    #
    # The PKGBUILD builds the last PUSHED commit, not this working tree.
    PKG_DIR=packaging/arch
    DIST_DIR=dist/linux

    # WHERE MAKEPKG PUTS ITS WORKING FILES -- this is load-bearing, not
    # tidiness. Left alone, all of these default to $startdir, i.e.
    # packaging/arch/ itself (makepkg: `${!var:-$startdir}`). For a git
    # source that means makepkg drops a BARE CLONE OF THIS ENTIRE REPOSITORY
    # at packaging/arch/whisper_destilado/ inside the working tree. Pointing
    # them at build/packaging/ (already gitignored wholesale, same as
    # build/ and dist/) keeps that clone out of the tree entirely, rather
    # than relying on a nested .gitignore pattern that could be missed.
    #
    # Absolute paths: makepkg runs with its own $startdir, so relative ones
    # would resolve against packaging/arch/.
    export SRCDEST="$PWD/build/packaging/src"      # VCS clones + source tarballs
    export BUILDDIR="$PWD/build/packaging/build"   # src/ and pkg/ extraction
    export PKGDEST="$PWD/$DIST_DIR"                # finished packages, straight to dist/
    mkdir -p "$SRCDEST" "$BUILDDIR" "$PKGDEST"

    if ! command -v makepkg >/dev/null 2>&1; then
        echo "error: makepkg not found -- this mode needs base-devel" >&2
        exit 2
    fi
    if [[ ! -f "$PKG_DIR/PKGBUILD" ]]; then
        echo "error: $PKG_DIR/PKGBUILD not found" >&2
        exit 2
    fi
    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        echo "note: --packages is always Release; ignoring debug." >&2
    fi
    if [[ ${#CMAKE_ARGS[@]} -gt 0 ]]; then
        echo "note: extra cmake args are not forwarded to makepkg;" >&2
        echo "      edit $PKG_DIR/PKGBUILD's build() instead: ${CMAKE_ARGS[*]}" >&2
    fi

    if [[ "$CLEAN" -eq 1 ]]; then
        echo "Cleaning build/packaging and previous packages..."
        rm -rf build/packaging
        rm -f "$DIST_DIR"/*.pkg.tar.zst
        mkdir -p "$SRCDEST" "$BUILDDIR"
    fi

    echo
    echo "==> makepkg: four variants, each a full build (this takes a while)"
    # -f so a re-run overwrites; no -i, since installing one of four on the
    # build machine is a separate decision from producing them.
    ( cd "$PKG_DIR" && makepkg -f )

    # PKGDEST already put them in $DIST_DIR, next to the --share tarballs, so
    # there is one place to upload from and nothing to move.
    shopt -s nullglob
    built=("$DIST_DIR"/*.pkg.tar.zst)
    shopt -u nullglob
    if [[ ${#built[@]} -eq 0 ]]; then
        echo "error: makepkg reported success but produced no packages" >&2
        exit 1
    fi

    echo
    echo "Packages in $DIST_DIR/:"
    for p in "${built[@]}"; do
        printf '  %6s  %s\n' "$(du -h "$p" | cut -f1)" "$p"
    done
    echo
    echo "Each is self-contained -- a recipient downloads ONE and runs:"
    echo "  sudo pacman -U <file>"
    echo "Then points it at a whisper model (.bin/.gguf), no sudo needed:"
    echo "  export WHISPER_MODEL_DIR=~/Models/whisper"
    echo "or drops one into the read-only install (needs sudo):"
    echo "  /opt/whisper_destilado/models/"
    echo "They can check which variant their CPU supports with:"
    echo "  /lib/ld-linux-x86-64.so.2 --help | grep -A4 'Subdirectories of glibc-hwcaps'"
    echo "universal works everywhere; v3/v4/zen4 only where that line says 'supported'."
    exit 0
fi

if [[ "$SHARE" -eq 1 ]]; then
    # variant name -> WHISPER_ARCH_LEVEL value
    declare -A SHARE_VARIANTS=(
        [universal]="universal"
        [v3]="v3"
        [v4]="v4"
        [zen4]="znver4"
    )

    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        SHARE_ROOT=build/linux_share_debug
    else
        SHARE_ROOT=build/linux_share
    fi
    DIST_DIR=dist/linux

    if [[ "$CLEAN" -eq 1 ]]; then
        echo "Cleaning $SHARE_ROOT and $DIST_DIR..."
        rm -rf "$SHARE_ROOT" "$DIST_DIR"
    fi
    [[ "$BUILD_TYPE" == "Release" ]] && mkdir -p "$DIST_DIR"

    for variant in "${!SHARE_VARIANTS[@]}"; do
        level="${SHARE_VARIANTS[$variant]}"
        variant_dir="$SHARE_ROOT/$variant"

        echo
        echo "==> Configuring '$variant' ($BUILD_TYPE) -> $variant_dir..."
        cmake -S . -B "$variant_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DWHISPER_ARCH_LEVEL="$level" \
            "${SLANGC_ARG[@]}" "${CMAKE_ARGS[@]}"
        cmake --build "$variant_dir"

        if [[ "$BUILD_TYPE" == "Release" ]]; then
            # Only the runtime files ship -- $variant_dir/ also holds
            # CMakeFiles/ and other build-tree clutter that cp -r'ing the
            # whole directory would otherwise drag into the tarball.
            pkg_name="whisper_destilado-linux-$variant"
            pkg_dir="$DIST_DIR/$pkg_name"
            rm -rf "$pkg_dir"
            mkdir -p "$pkg_dir/models"
            cp "$variant_dir/whisper_destilado" "$pkg_dir/"
            cp -r "$variant_dir/assets" "$pkg_dir/assets"
            tar -C "$DIST_DIR" -czf "$DIST_DIR/$pkg_name.tar.gz" "$pkg_name"
            rm -rf "$pkg_dir"
            echo "==> Packaged $DIST_DIR/$pkg_name.tar.gz"
        else
            echo "==> Built $variant_dir/whisper_destilado (Debug, not packaged)"
        fi
    done

    echo
    if [[ "$BUILD_TYPE" == "Release" ]]; then
        echo "All-variant build done. Tarballs in $DIST_DIR/:"
        for variant in "${!SHARE_VARIANTS[@]}"; do
            echo "  $DIST_DIR/whisper_destilado-linux-$variant.tar.gz"
        done
    else
        echo "All-variant build done (Debug, unpackaged). Binaries:"
        for variant in "${!SHARE_VARIANTS[@]}"; do
            echo "  $SHARE_ROOT/$variant/whisper_destilado"
        done
    fi
    exit 0
fi

if [[ "$BUILD_TYPE" == "Debug" ]]; then
    BUILD_DIR="build/linux${ARCH_SUFFIX}-debug"
else
    BUILD_DIR="build/linux${ARCH_SUFFIX}"
fi

if [[ "$CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

ARCH_ARG=()
[[ -n "$ARCH_LEVEL" ]] && ARCH_ARG=(-DWHISPER_ARCH_LEVEL="$ARCH_LEVEL")

echo "Configuring CMake (Ninja, $BUILD_TYPE${ARCH_LEVEL:+, arch=$ARCH_LEVEL}) -> $BUILD_DIR..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${ARCH_ARG[@]}" "${SLANGC_ARG[@]}" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR"

echo
echo "Done. Binaries in $BUILD_DIR/:"
echo "  whisper_destilado       (GUI, Wayland)"
echo "  whisper_destilado_cli   (headless CLI)"
echo "  whisper_tests           (unit tests)"
if [[ "$BUILD_TYPE" == "Debug" ]]; then
    echo "  whisper_ui_capture      (headless UI snapshots -- debug only)"
fi
echo "Place a whisper model (.bin/.gguf) in $BUILD_DIR/models/"
echo "  (or set WHISPER_MODEL_DIR=<folder> / WHISPER_MODEL_PATH=<file> to use one elsewhere)"
