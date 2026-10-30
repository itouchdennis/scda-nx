# Changelog
There are no prebuilt NROs in any release: each release holds the source and the build scripts. You build the NRO
yourself, from your own copy of the game ([docs/HOW_TO_BUILD.md](docs/HOW_TO_BUILD.md)).

## v0.4.0
- **Quitting the game no longer crashes the Switch.** Exit in the main menu now leaves the way a relaunch does (the
  game's threads paused, the GPU, audio and network services closed in order) and returns to the HOME menu instead
  of crashing (tested on a Switch V1). The same goes for the game's own shutdown calls.
- **CI without the game:** every push and pull request compiles the Linux port (Vulkan, OpenGL and null renderers)
  and the Switch port, checks the shell scripts, and runs `build-nro-windows.bat` on a real Windows runner up to
  the point where it needs Docker. Nothing is linked and no NRO is made: the one header from the game the sources
  include, `image_symbols.h`, is made from `config/scda_v2.toml` (`tools/ci/`, `SCDA_CHECK_ONLY`).
- `SCDA_NO_PAUSE=1` makes `build-nro-windows.bat` close at once instead of waiting for Enter.
- **Windows is still only partly tested:** CI starts the script on Windows, but a full build with Docker Desktop on
  a real Windows PC has not been done yet.

## v0.3.0
**Note: Windows has not been tested yet.** `build-nro-windows.bat` was written for Windows 10 and 11 but has not been
run on a real Windows PC: in the test VM, Docker Desktop could not start (nested Hyper-V). Only the Linux builders
were run from start to finish. If you try it on Windows, please report how it went in an issue.

- **Windows builder:** `build-nro-windows.bat`. Double-click it and choose your game's `.iso`, or drag the `.iso` or
  the extracted disc onto it. It needs only Docker Desktop, starts it if needed, builds the NRO and opens the finished
  SD card folder.
- **The build scripts come with each release.** `build-nro-windows.bat`, `build-nro-docker.sh` and a short
  `HOW_TO_BUILD.md` are attached to the release, so you do not need git or the whole repository to build.
- **The builder stays up to date:** both scripts fetch the newest builder image each run, and keep using the one
  already downloaded when you are offline.
- **An icon beside the script:** an `icon.jpg` next to the script is used for the Homebrew Menu.
- **Copying an extracted disc** into the build no longer copies owners or permissions, which Windows folders do not take.
- **A step-by-step guide for Windows and Linux** in the README and in [docs/HOW_TO_BUILD.md](docs/HOW_TO_BUILD.md).

## v0.2.1
The first public release of the Switch port.
- **Nintendo Switch port** (libnx homebrew) of the SCDA-XBERecomp static recompilation, rendering through Vulkan on
  mesa-switch's NVK. It runs on Linux as well, which serves as its test bed.
- **Shader cache on the SD card** (`shaders.vkb`): shaders are built once while you play and load at once from then on.
- **Audio crackling fixed:** a larger audio buffer on the Switch.
- **Logging is off by default:** `logging = true` in `sdmc:/switch/scda/scda.toml` writes the full log to `logs/`.
  A crash still writes `crash.txt`.
- **Builders:** `build-nro-docker.sh` builds with Docker on any Linux. `build-nro.sh` builds natively on Arch Linux in
  one command. The README also has the manual steps.
- **Homebrew Menu icon:** a JPEG you provide yourself. It is never part of the repository.
- **[PORTING_GUIDE.md](PORTING_GUIDE.md):** what porting it took, and the traps found on the way.
- **Tested** only the single player campaign, on a first-model Switch (V1), handheld, with a light overclock.
