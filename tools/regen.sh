#!/usr/bin/env bash
# default.xbe to C++ on this machine, for a cross build (switch/build.sh) that cannot run the recompiler itself.
# A native Linux build (cmake/portable.cmake) recompiles on its own and needs none of this.
#
#   SCDA_XBE       default.xbe (default assets/default.xbe)
#   SCDA_GEN_DIR   where the code goes (default generated/game)
#   HOST_BUILD     build folder for the recompiler (default build/host-tools)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XBE="${SCDA_XBE:-$REPO/assets/default.xbe}"
GEN="${SCDA_GEN_DIR:-$REPO/generated/game}"
HOST="${HOST_BUILD:-$REPO/build/host-tools}"

[ -f "$XBE" ] || { echo "error: no $XBE (put the extracted disc in assets/)" >&2; exit 1; }
cmake -S "$REPO" -B "$HOST" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCDA_GPU=null >/dev/null
ninja -C "$HOST" xbe_recomp
mkdir -p "$GEN"
(cd "$REPO" && "$HOST/xbe_recomp" config/scda_v2.toml "$XBE" "$GEN")
touch "$GEN/codegen.stamp"
echo "generated $GEN"
