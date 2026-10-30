#!/usr/bin/env bash
# The Switch NRO from your own copy of the game, built inside the scda-nx builder container (docker/Dockerfile). Needs
# only Docker: no toolchain, no packages and no sudo on this computer.
#
#   ./build-nro-docker.sh "/path/to/your game.iso"         (an Xbox disc image: .iso or .xiso)
#   ./build-nro-docker.sh "/path/to/your extracted disc"    (a folder with default.xbe, Default/, Media/)
#
# Your game is mounted read only into the container and never leaves this computer; the SD card folder is made in
# out/ beside this script (out/switch/scda/). The recompiled code and the build are kept in the Docker volume
# scda-nx-cache, so later runs are quicker; `docker volume rm scda-nx-cache` removes them.
#
#   OUT            the output folder (default out/ beside this script)
#   SCDA_IMAGE     the builder image (default ghcr.io/itouchdennis/scda-nx-builder:latest)
#   SCDA_LOCAL=1   build the builder image here from docker/Dockerfile instead of pulling it (takes long: NVK)
#   SCDA_ICON      your own 256x256 JPEG for the Homebrew Menu (default switch/icon.jpg when it exists)
#   SCDA_ANY_RELEASE=1  build from a release other than the tested one
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$REPO/out}"
IMAGE="${SCDA_IMAGE:-ghcr.io/itouchdennis/scda-nx-builder:latest}"
ICON="${SCDA_ICON:-$REPO/switch/icon.jpg}"

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ $# -eq 1 ] || { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
GAME="$(realpath -e -- "$1" 2>/dev/null)" || fail "no such file or folder: $1"
command -v docker >/dev/null || fail "Docker is not installed (https://docs.docker.com/engine/install/)"
docker info >/dev/null 2>&1 || fail "Docker does not answer: is it running, and may you use it? (add yourself to the docker group: sudo usermod -aG docker \$USER, then log in again)"

if [ "${SCDA_LOCAL:-0}" = 1 ]; then
    IMAGE=scda-nx-builder:local
    echo "== building the builder image from docker/Dockerfile (the first time takes long: it compiles NVK)"
    docker build -t "$IMAGE" -f "$REPO/docker/Dockerfile" "$REPO"
elif ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== downloading the builder image $IMAGE"
    docker pull "$IMAGE" || fail "could not download $IMAGE; build it here instead with SCDA_LOCAL=1"
fi

mkdir -p "$OUT"
mounts=(-v "$GAME:/game:ro" -v "$OUT:/out" -v scda-nx-cache:/cache)
[ -f "$ICON" ] && mounts+=(-v "$ICON:/icon.jpg:ro")

# the volume belongs to root when Docker makes it: hand it to this user once, so the build may run as you
docker run --rm -v scda-nx-cache:/cache --entrypoint chown "$IMAGE" "$(id -u):$(id -g)" /cache

docker run --rm -t --user "$(id -u):$(id -g)" -e SCDA_ANY_RELEASE="${SCDA_ANY_RELEASE:-0}" "${mounts[@]}" "$IMAGE"
echo
echo "Your SD card folder: $OUT/switch"
