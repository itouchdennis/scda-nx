# scda-nx: Splinter Cell: Double Agent for the Nintendo Switch
An experimental, unofficial Nintendo Switch (homebrew, libnx) port of [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp),
the static recompilation of the original Xbox version of Splinter Cell: Double Agent. It renders through Vulkan on
mesa-switch's NVK and runs on Linux as well, which serves as its test bed.

> [!NOTE]
> **AI-written, human-tested.** All of the code this fork adds (the Linux and Switch port, the Vulkan renderer, the
> build scripts and [PORTING_GUIDE.md](PORTING_GUIDE.md)) was written entirely by Claude, Anthropic's AI model. A
> human tested it on a real Nintendo Switch and directed the work. The original recompilation it builds on is the work
> of the [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) authors (see [Credits](#credits)).

> [!WARNING]
> **What has been tested:** only the single player campaign, on a first-model Nintendo Switch (V1) in handheld mode
> with a light overclock. Co-op, Versus, docked mode, other Switch models and stock clocks have not been tested.
> The Windows builder (`build-nro-windows.bat`) has not been tested on Windows yet; only the Linux builders were run
> from start to finish.

> [!IMPORTANT]
> **You must own a legally acquired copy of the game.** This repository contains no game data, no assets and no code
> generated from the game: everything game specific is produced at build time from the files of your own copy. This
> project does not support piracy in any form. Do not ask for game files, ISOs or prebuilt game code, and do not
> share them in issues or pull requests; such requests will be closed.

This project is not affiliated with or endorsed by Ubisoft, Microsoft or Nintendo. All trademarks belong to their
respective owners.

## No prebuilt NROs
There are no prebuilt `.nro` files, and there will not be any. A built NRO holds the game's own code, recompiled
from the XBE, so sharing one would mean sharing the game. Everyone builds their own from their own copy, as below.
Please do not upload or ask for built NROs.

## How to build it: Windows and Linux in a few clicks
Both ways run the build inside a ready-made builder container, so your computer needs nothing but Docker: no
toolchain, no packages and no changes to your system. The container image (`ghcr.io/itouchdennis/scda-nx-builder`,
made from [docker/Dockerfile](docker/Dockerfile) by [GitHub Actions](.github/workflows/builder-image.yml)) holds the
Switch toolchain, the Vulkan driver and this repository's sources, and nothing of the game. Your game is mounted into
it read only, and the NRO is made on your computer from your own copy. The scripts are attached to every
[release](https://github.com/itouchdennis/scda-nx/releases), so you do not need git or the whole repository.

You need your game as an `.iso` or the extracted disc (the folder with `default.xbe`), about 20 GB of free disk space,
and patience for the first build (about 20 minutes to over an hour). Later builds are much quicker. Step by step, with
everything that can come up: [docs/HOW_TO_BUILD.md](docs/HOW_TO_BUILD.md).

### Windows (10 or 11)
**Windows has not been tested yet.** `build-nro-windows.bat` was written for Windows 10 and 11 but has not been
run on a real Windows PC: in the test VM, Docker Desktop could not start (nested Hyper-V). Only the Linux builders
were run from start to finish. If you try it on Windows, please report how it went in an issue.

1. Install [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/) and start it once.
2. Download [build-nro-windows.bat](build-nro-windows.bat) from the latest release and put it in a folder of its own.
3. Double-click it and choose your game's `.iso` (or drag the `.iso` or the extracted disc's folder onto it). If
   Windows warns about the file, click **More info**, then **Run anyway**.
4. When it is done, a window opens with the folder `switch`: copy it onto the root of your SD card.

### Linux
1. Install [Docker](https://docs.docker.com/engine/install/), run `sudo usermod -aG docker $USER`, then log out and in.
2. Download [build-nro-docker.sh](build-nro-docker.sh) from the latest release (or clone this repository) and run it:
   ```sh
   chmod +x build-nro-docker.sh
   ./build-nro-docker.sh "/path/to/your Splinter Cell Double Agent.iso"
   ```
3. When it is done, copy `out/switch` (beside the script) onto the root of your SD card.

### Then on the Switch
Start the Homebrew Menu with title takeover (hold **R** while you start any game) and start Splinter Cell: Double
Agent from it. The first time, things can be missing for a moment while the shaders are built.

For an icon in the Homebrew Menu, put a 256x256 JPEG named `icon.jpg` beside the script before building. To run
again for updates, just run the script again; the NRO it replaces is kept in `out/BAK/`, named after its version.
The build cache stays in the Docker volume `scda-nx-cache` (`docker volume rm scda-nx-cache` removes it). macOS may
work with Docker Desktop too, but it has not been tested.

> [!CAUTION]
> The same goes here as for every script: read [build-nro-windows.bat](build-nro-windows.bat) or
> [build-nro-docker.sh](build-nro-docker.sh), and [docker/entrypoint.sh](docker/entrypoint.sh) and
> [docker/Dockerfile](docker/Dockerfile), before you run them if you are the least bit skeptical. If you would rather
> not trust a prebuilt image either, build it yourself from the Dockerfile on Linux:
> `SCDA_LOCAL=1 ./build-nro-docker.sh ...` (this takes a while, it compiles the Vulkan driver).

## The easy way without Docker: one command (Arch Linux)
`build-nro.sh` does every step below by itself: it installs what is missing (asking for your password), sets up
devkitPro, builds the Switch's Vulkan driver once, extracts your disc image, recompiles the game from it and leaves a
folder ready for the SD card. Nothing of the game is downloaded or uploaded; everything is made on your computer from
your own copy, and kept in `~/.cache/scda-nx` for the next run.

> [!CAUTION]
> **Only use this script if you are feeling lazy.** It runs `sudo`, edits `/etc/pacman.conf` and downloads and builds
> code from the internet. Never blindly trust a script from the internet, this one included. If you are even a little
> skeptical (and being skeptical is a good thing these days), read [build-nro.sh](build-nro.sh) first, or skip it
> and run the steps from [Building for the Nintendo Switch by hand](#building-for-the-nintendo-switch-by-hand-arch-linux)
> one by one, so you see and approve everything that happens.
```sh
git clone https://github.com/itouchdennis/scda-nx.git
cd scda-nx
./build-nro.sh "/path/to/your Splinter Cell Double Agent.iso"
```
The extracted disc's folder (with `default.xbe`) works as well as an `.iso`. The first run can take a while, depending on your
PC (the Vulkan driver and the whole game are compiled); later runs only rebuild what changed. When it is done, copy
`out/switch` onto the root of your SD card and start the game from the Homebrew Menu with title takeover (hold R while
starting any game). For an icon in the Homebrew Menu, put a 256x256 JPEG at `switch/icon.jpg` before building.

## Building for the Nintendo Switch by hand (Arch Linux)
What `build-nro.sh` does, step by step. Each block can be pasted into a terminal as it is. Expect the first build to
take a while: NVK, the recompiler and the whole game are compiled.

### 1. Host packages
Compilers and build tools, plus what Mesa's NVK needs to build (LLVM, libclc and the SPIR-V tools for its shader
compiler, Rust with the AArch64 target, bindgen and cbindgen):
```sh
sudo pacman -S --needed base-devel git cmake ninja meson python python-mako python-yaml python-packaging python-ply \
    flex bison clang llvm libclc spirv-llvm-translator spirv-tools glslang \
    rust rust-aarch64-gnu rust-bindgen cbindgen sdl2-compat rsync
```

### 2. devkitPro
devkitPro's pacman repositories, as [devkitPro describes them](https://devkitpro.org/wiki/devkitPro_pacman):
```sh
sudo pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
grep -q '^\[dkp-libs\]' /etc/pacman.conf || printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' | sudo tee -a /etc/pacman.conf >/dev/null
sudo pacman -Syu
```
Then the Switch toolchain and the libraries the port and NVK link:
```sh
sudo pacman -S --needed switch-dev switch-sdl2 switch-libexpat switch-zlib switch-libzstd
source /etc/profile.d/devkit-env.sh
```

### 3. Sources
This repository and mesa-switch (NVK for the Switch), side by side. The port was tested with mesa-switch at commit
`1a8c1a66d6f`:
```sh
mkdir -p ~/switch && cd ~/switch
git clone https://github.com/itouchdennis/scda-nx.git
git clone https://github.com/danfromtico/mesa-switch.git
git -C mesa-switch checkout 1a8c1a66d6f
```

### 4. NVK
Builds mesa-switch's Vulkan driver once, into `scda-nx/build/nvk-sdk`, where the NRO build finds it:
```sh
cd ~/switch/scda-nx
MESA=~/switch/mesa-switch switch/nvk/build-nvk.sh
```

### 5. Your game files
Copy the **extracted** disc of your own copy of the game into `assets/`, so that `assets/default.xbe` sits beside
`Default/` and `Media/` (an ISO is not enough here, extract it first). This should list the file:
```sh
ls ~/switch/scda-nx/assets/default.xbe
```

### 6. The NRO
Recompiles the game from `assets/default.xbe` on the first run (`tools/regen.sh`), then builds and stages the NRO:
```sh
cd ~/switch/scda-nx
switch/build.sh
```
The result is `switch_sd/switch/scda/scda.nro`. To give it an icon in hbmenu, put a 256x256 JPEG at
`switch/icon.jpg` before building; git ignores it, so it stays out of the repository.

### 7. On the Switch
Copy `scda.nro` to `sdmc:/switch/scda/` and the extracted disc to `sdmc:/switch/scda/game/`, then start it from
hbmenu with title takeover (hold R while starting a game); applet mode leaves too little memory.

On the first start the shaders are built while you play: objects can be missing or see-through for a moment. They
are kept on the SD card (`shaders.vkb`) and load at once from then on.

Logging is off by default. To write the full log (and `boot.txt`) to `sdmc:/switch/scda/logs/`, for a bug report
for example, add `logging = true` to `sdmc:/switch/scda/scda.toml`. A crash still writes `crash.txt` either way.

What it took and the traps found on the way are in [PORTING_GUIDE.md](PORTING_GUIDE.md).

## Changes
What changed in each release: [CHANGELOG.md](CHANGELOG.md). How to contribute, and the commit message format
(Conventional Commits): [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits
The recompilation, the Xbox kernel and XDK reimplementation, the renderer and all of the PC features are the work of
the original [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) project by TGP482 and Joshhhuaaa, made with
the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool, under the MIT license (see [LICENSE](LICENSE)).
This fork adds the Linux and Nintendo Switch port. Please report Switch issues here, not upstream.

For the game on Windows (downloads, features, online play, system requirements and how to build it), see the original
project: **https://github.com/TGP482/SCDA-XBERecomp**
