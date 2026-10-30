#!/usr/bin/env bash
# Compiles the port's own sources without the game (SCDA_CHECK_ONLY): image_symbols.h is made from the config, the
# recompiled code and the link are left out. What CI runs to catch build breaks; it cannot make an NRO.
#   tools/ci/check-build.sh linux <vk|gl|null>     the Linux port, and the recompiler
#   tools/ci/check-build.sh switch [vk]            the Switch port (devkitPro, and NVK in SCDA_NVK_SDK)
#   BUILD_DIR   where to build (default build/ci-<target>-<gpu>)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${1:?linux or switch}"
GPU="${2:-vk}"
BUILD="${BUILD_DIR:-$REPO/build/ci-$TARGET-$GPU}"
GEN="$BUILD/gen"

python3 "$REPO/tools/ci/symbols_header.py" "$REPO/config/scda_v2.toml" "$GEN"
args=(-S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCDA_CHECK_ONLY=ON -DSCDA_GEN_DIR="$GEN" -DSCDA_GPU="$GPU")
targets=(scda)
case "$TARGET" in
    linux)
        targets+=(xbe_recomp) ;;
    switch)
        export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
        args+=(-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DSCDA_NVK_SDK="${SCDA_NVK_SDK:-}") ;;
    *)
        echo "error: linux or switch, not $TARGET" >&2; exit 2 ;;
esac
cmake "${args[@]}"
ninja -C "$BUILD" "${targets[@]}"
echo "ok: $TARGET $GPU compiles"
