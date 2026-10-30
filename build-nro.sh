#!/usr/bin/env bash
# The Switch NRO from your own copy of the game, in one command, on Arch Linux:
#
#   ./build-nro.sh "/path/to/your game.iso"        (an Xbox disc image: .iso or .xiso)
#   ./build-nro.sh "/path/to/your extracted disc"   (a folder with default.xbe, Default/, Media/)
#
# It installs what is missing (asking for your password through sudo), builds the Switch toolchain's Vulkan driver
# once, recompiles the game from your default.xbe and leaves a folder to copy onto the SD card: out/switch/scda/.
# Nothing of the game is downloaded or uploaded: everything is made here, from your files, and stays on this machine.
#
#   CACHE          where the driver, tools and recompiled code are kept between runs (default ~/.cache/scda-nx)
#   OUT            the SD card folder made (default out/ beside this script)
#   SCDA_NVK_SDK   a Vulkan driver already built (portlibs/switch with lib/libvulkan.a): skips building it
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="${CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/scda-nx}"
OUT="${OUT:-$REPO/out}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
MESA_URL=https://github.com/danfromtico/mesa-switch.git
MESA_COMMIT=1a8c1a66d6f  # the mesa-switch the port was tested with
XISO_URL=https://github.com/XboxDev/extract-xiso.git
TESTED_XBE_SHA1=91290baf93c2f6ae735a3106c48734665c826d84  # Double Agent (USA, Europe), the release tested

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
fail() { printf '\n\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ask() {
    local answer
    read -r -p "$1 [y/N] " answer
    [[ "$answer" == [yYjJ]* ]]
}

[ $# -eq 1 ] || { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
GAME_IN="$(realpath -e -- "$1" 2>/dev/null)" || fail "no such file or folder: $1"
[ "$(id -u)" != 0 ] || fail "run it as your own user, not as root (it asks for sudo itself where needed)"
command -v pacman >/dev/null || fail "this builder is for Arch Linux and its derivatives; elsewhere see the README's steps"

# --- 1. packages ------------------------------------------------------------------------------------------------------
HOST_PACKAGES=(base-devel git cmake ninja meson python python-mako python-yaml python-packaging python-ply flex bison
    clang llvm libclc spirv-llvm-translator spirv-tools glslang rust rust-aarch64-gnu rust-bindgen cbindgen
    sdl2-compat rsync)
SWITCH_PACKAGES=(switch-dev switch-sdl2 switch-libexpat switch-zlib switch-libzstd)

step "Packages"
missing=()
for p in "${HOST_PACKAGES[@]}"; do pacman -Q "$p" &>/dev/null || pacman -Qg "$p" &>/dev/null || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]}"
    sudo pacman -S --needed "${missing[@]}"
fi

if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
    echo "devkitPro's package repositories (the Switch toolchain, https://devkitpro.org) are not set up yet."
    ask "Add them to /etc/pacman.conf and import devkitPro's signing key?" || fail "the Switch toolchain is needed"
    sudo pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
    sudo pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
    keyring="$(mktemp -d)/devkitpro-keyring.pkg.tar.zst"
    curl -fL https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst -o "$keyring"
    sudo pacman -U --noconfirm "$keyring"
    printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' |
        sudo tee -a /etc/pacman.conf >/dev/null
    sudo pacman -Sy
fi
missing=()
for p in "${SWITCH_PACKAGES[@]}"; do pacman -Q "$p" &>/dev/null || pacman -Qg "$p" &>/dev/null || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]}"
    sudo pacman -S --needed "${missing[@]}"
fi
export DEVKITPRO
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || fail "devkitPro is not at $DEVKITPRO after installing it"
echo "all there"
mkdir -p "$CACHE"

# --- 2. the game ------------------------------------------------------------------------------------------------------
DEST="$OUT/switch/scda"
GAME="$DEST/game"
step "Game files -> $GAME"
mkdir -p "$GAME"
if [ -d "$GAME_IN" ]; then
    [ -f "$GAME_IN/default.xbe" ] || fail "$GAME_IN has no default.xbe: choose the extracted disc's folder"
    rsync -a --info=progress2 --size-only --exclude '*.iso' "$GAME_IN/" "$GAME/"
else
    if ! command -v extract-xiso >/dev/null && [ ! -x "$CACHE/tools/extract-xiso" ]; then
        echo "building extract-xiso (reads Xbox disc images)"
        rm -rf "$CACHE/src/extract-xiso"
        git clone --depth 1 "$XISO_URL" "$CACHE/src/extract-xiso"
        cmake -S "$CACHE/src/extract-xiso" -B "$CACHE/src/extract-xiso/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
        ninja -C "$CACHE/src/extract-xiso/build"
        mkdir -p "$CACHE/tools"
        cp "$CACHE/src/extract-xiso/build/extract-xiso" "$CACHE/tools/"
    fi
    XISO="$(command -v extract-xiso || echo "$CACHE/tools/extract-xiso")"
    if [ -f "$GAME/default.xbe" ] && [ "$GAME/default.xbe" -nt "$GAME_IN" ]; then
        echo "already extracted"
    else
        "$XISO" -s -d "$GAME" -x "$GAME_IN" || fail "$GAME_IN could not be read as an Xbox disc image"
    fi
    [ -f "$GAME/default.xbe" ] || fail "$GAME_IN holds no default.xbe: is it the Splinter Cell: Double Agent disc?"
fi
XBE_SHA1="$(sha1sum "$GAME/default.xbe" | cut -d' ' -f1)"
if [ "$XBE_SHA1" = "$TESTED_XBE_SHA1" ]; then
    echo "default.xbe: the tested release (USA, Europe)"
else
    echo "default.xbe: sha1 $XBE_SHA1, not the release this port was tested with ($TESTED_XBE_SHA1)."
    echo "Another release may not work."
    ask "Build anyway?" || exit 1
fi

# --- 3. the Vulkan driver ---------------------------------------------------------------------------------------------
NVK="${SCDA_NVK_SDK:-$CACHE/nvk-sdk/opt/devkitpro/portlibs/switch}"
step "Vulkan driver (NVK) -> $NVK"
if [ -f "$NVK/lib/libvulkan.a" ]; then
    echo "already built"
else
    MESA="$CACHE/src/mesa-switch"
    if [ ! -d "$MESA/.git" ]; then
        git clone "$MESA_URL" "$MESA"
    fi
    git -C "$MESA" fetch --quiet origin || true
    git -C "$MESA" checkout --quiet "$MESA_COMMIT"
    echo "building it, once (this takes a while)"
    MESA="$MESA" OUT="$CACHE/nvk-sdk" "$REPO/switch/nvk/build-nvk.sh"
    [ -f "$NVK/lib/libvulkan.a" ] || fail "the Vulkan driver did not build"
fi

# --- 4. the game's code, recompiled from your default.xbe --------------------------------------------------------------
GEN="$CACHE/generated/$XBE_SHA1"
step "Recompiling default.xbe -> $GEN"
if [ -f "$GEN/image_symbols.h" ] && [ "$GEN/codegen.stamp" -nt "$REPO/config/scda_v2.toml" ]; then
    echo "already recompiled"
else
    SCDA_XBE="$GAME/default.xbe" SCDA_GEN_DIR="$GEN" HOST_BUILD="$CACHE/host-tools" "$REPO/tools/regen.sh"
fi

# --- 5. the NRO -------------------------------------------------------------------------------------------------------
step "Building the NRO (the first time takes 30 to 90 minutes)"
SCDA_GEN_DIR="$GEN" SCDA_NVK_SDK="$NVK" SD_ROOT="$OUT" BUILD_DIR="$CACHE/build-switch-vk" "$REPO/switch/build.sh"
[ -f "$DEST/scda.nro" ] || fail "no NRO was made"

step "Done"
echo "Copy the folder $OUT/switch onto the root of your SD card, so that it holds"
echo "  sdmc:/switch/scda/scda.nro and sdmc:/switch/scda/game/default.xbe"
echo "and start it from the Homebrew Menu with title takeover: hold R while starting any game."
echo
echo "The NRO holds code made from your copy of the game: it is for you only, please do not share it."
