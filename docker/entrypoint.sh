#!/usr/bin/env bash
# The builder container's job (docker/Dockerfile), started by build-nro-docker.sh. Mounted:
#   /game    your copy of the game, read only: an Xbox disc image (.iso, .xiso) or the extracted disc's folder
#   /out     where the SD card folder is made: /out/switch/scda/scda.nro and /out/switch/scda/game/
#   /cache   kept between runs: the recompiled code and the build (a Docker volume)
#   /icon.jpg  optional, your own 256x256 JPEG for the Homebrew Menu
# SCDA_ANY_RELEASE=1 builds from a default.xbe other than the release the port was tested with.
set -euo pipefail

TESTED_XBE_SHA1=91290baf93c2f6ae735a3106c48734665c826d84  # Double Agent (USA, Europe), the release tested
NVK=/opt/nvk-sdk/opt/devkitpro/portlibs/switch
DEST=/out/switch/scda
GAME="$DEST/game"

step() { printf '\n== %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[ -e /game ] || fail "no game mounted at /game (build-nro-docker.sh does that for you)"
[ -d /out ] && [ -w /out ] || fail "no writable folder mounted at /out"
[ -d /cache ] && [ -w /cache ] || fail "no writable volume mounted at /cache"

step "Game files -> $GAME"
mkdir -p "$GAME"
if [ -d /game ]; then
    [ -f /game/default.xbe ] || fail "the folder you chose has no default.xbe: choose the extracted disc's folder"
    # sizes only, no owners or permissions: /out can be a Windows folder (Docker Desktop), which takes neither
    rsync -r --info=progress2 --size-only --exclude '*.iso' /game/ "$GAME/"
elif [ -f "$GAME/default.xbe" ] && [ "$GAME/default.xbe" -nt /game ]; then
    echo "already extracted"
else
    extract-xiso -s -d "$GAME" -x /game || fail "the file you chose could not be read as an Xbox disc image"
fi
[ -f "$GAME/default.xbe" ] || fail "no default.xbe in what you chose: is it the Splinter Cell: Double Agent disc?"

XBE_SHA1="$(sha1sum "$GAME/default.xbe" | cut -d' ' -f1)"
if [ "$XBE_SHA1" = "$TESTED_XBE_SHA1" ]; then
    echo "default.xbe: the tested release (USA, Europe)"
elif [ "${SCDA_ANY_RELEASE:-0}" = 1 ]; then
    echo "default.xbe: sha1 $XBE_SHA1, not the tested release; building it anyway (SCDA_ANY_RELEASE=1)"
else
    fail "default.xbe has sha1 $XBE_SHA1, not the release this port was tested with ($TESTED_XBE_SHA1).
Another release may not work. To build it anyway, run again with SCDA_ANY_RELEASE=1."
fi

GEN="/cache/generated/$XBE_SHA1"
step "Recompiling default.xbe"
if [ -f "$GEN/image_symbols.h" ] && [ "$GEN/codegen.stamp" -nt /opt/scda/config/scda_v2.toml ]; then
    echo "already recompiled"
else
    mkdir -p "$GEN"
    (cd /opt/scda && xbe_recomp config/scda_v2.toml "$GAME/default.xbe" "$GEN")
    touch "$GEN/codegen.stamp"
fi

step "Building the NRO"
icon=()
[ -f /icon.jpg ] && icon=(SCDA_ICON=/icon.jpg)
env "${icon[@]}" SCDA_GEN_DIR="$GEN" SCDA_NVK_SDK="$NVK" SD_ROOT=/out BUILD_DIR=/cache/build-switch-vk \
    /opt/scda/switch/build.sh
[ -f "$DEST/scda.nro" ] || fail "no NRO was made"

step "Done"
echo "Copy the folder switch/ from your output folder onto the root of your SD card, so that it holds"
echo "  sdmc:/switch/scda/scda.nro and sdmc:/switch/scda/game/default.xbe"
echo "and start it from the Homebrew Menu with title takeover: hold R while starting any game."
echo
echo "The NRO holds code made from your copy of the game: it is for you only, please do not share it."
