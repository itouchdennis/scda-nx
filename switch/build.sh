#!/usr/bin/env bash
# Build the Switch homebrew NRO and stage an SD card layout:
#
#   <SD>/switch/scda/scda.nro
#   <SD>/switch/scda/game/       the extracted disc: default.xbe, Default/, Media/, ...
#   <SD>/switch/scda/scda.toml   settings, written by the game
#   <SD>/switch/scda/logs/       the log
#   <SD>/switch/scda/hdd/        saves
#
# Start it from hbmenu with title takeover (hold R while starting a game): applet mode leaves too little memory.
#
#   DEVKITPRO      default /opt/devkitpro (switch-dev, switch-sdl2)
#   SCDA_GEN_DIR   generated code from tools/regen.sh (default generated/game; made here when missing)
#   STAGE_GAME     1: also copy the extracted disc (SCDA_GAME_SRC, default assets/) into the staging folder; by
#                  default only the NRO is staged and the disc is copied to the card once by hand
#   SD_ROOT        staging SD root (default switch_sd/)
#   BUILD_DIR      default build/switch-vk, build/switch for SCDA_GPU=gl
#   SCDA_ICON      the NRO's hbmenu icon, a 256x256 JPEG (default switch/icon.jpg when it exists)
#   SCDA_GPU       vk (default, Vulkan on mesa-switch's NVK), gl (OpenGL on switch-mesa) or null (draws nothing)
#   SCDA_NVK_SDK   vk: the Mesa Horizon SDK's portlibs/switch (default build/nvk-sdk/opt/devkitpro/portlibs/switch,
#                  where switch/nvk/build-nvk.sh installs it)
#   VERBOSE        1 (default while the port is tested): the full log, written to the card twice a second
#   JOBS           parallel compile jobs (default: by free memory, the generated files need ~1.5 GB each)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
GEN="${SCDA_GEN_DIR:-$REPO/generated/game}"
GAME="${SCDA_GAME_SRC:-$REPO/assets}"
SD="${SD_ROOT:-$REPO/switch_sd}"
GPU="${SCDA_GPU:-vk}"
BUILD="${BUILD_DIR:-$REPO/build/switch$([ "$GPU" = vk ] && echo -vk)}"
NVK="${SCDA_NVK_SDK:-$REPO/build/nvk-sdk/opt/devkitpro/portlibs/switch}"
VERBOSE="${VERBOSE:-1}"

fail() { echo "error: $*" >&2; exit 1; }
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || fail "devkitPro not found at $DEVKITPRO"
[ -f "$DEVKITPRO/portlibs/switch/include/SDL2/SDL.h" ] || fail "switch-sdl2 missing"
[ -f "$GEN/image_symbols.h" ] || SCDA_GEN_DIR="$GEN" "$REPO/tools/regen.sh"
[ "$GPU" != vk ] || [ -f "$NVK/lib/libvulkan.a" ] || fail "no NVK at $NVK (build it with switch/nvk/build-nvk.sh, or set SCDA_NVK_SDK)"

if [ -z "${JOBS:-}" ]; then
    mem_mb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))
    JOBS=$(( mem_mb / 1500 )); [ "$JOBS" -lt 1 ] && JOBS=1
    [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
fi

cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSCDA_GEN_DIR="$GEN" -DSCDA_GPU="$GPU" ${SCDA_ICON:+-DSCDA_ICON="$SCDA_ICON"} -DSCDA_NVK_SDK="$NVK" -DSCDA_VERBOSE_LOG="$([ "$VERBOSE" = 1 ] && echo ON || echo OFF)" >/dev/null
ninja -C "$BUILD" -j"$JOBS"

DEST="$SD/switch/scda"
mkdir -p "$DEST/game"
cp "$BUILD/scda.nro" "$DEST/scda.nro"
if [ "${STAGE_GAME:-0}" = 1 ] && [ -f "$GAME/default.xbe" ]; then
    # several GB: copy only what changed
    rsync -a --size-only --exclude '*.iso' "$GAME/" "$DEST/game/"
fi
echo
echo "staged $DEST"
echo "  scda.nro $(stat -c %s "$DEST/scda.nro") bytes, game/ $(du -sh "$DEST/game" | cut -f1)"
echo "  copy scda.nro to <SD>/switch/scda/ and the extracted disc (default.xbe, Default/, Media/, ...) to"
echo "  <SD>/switch/scda/game/, then start it from hbmenu with title takeover (hold R on a game)"
