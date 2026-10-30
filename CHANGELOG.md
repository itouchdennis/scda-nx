# Changelog
There are no prebuilt NROs in any release: each release holds the source and the build scripts. You build the NRO
yourself, from your own copy of the game ([docs/HOW_TO_BUILD.md](docs/HOW_TO_BUILD.md)).

## v0.4.6
- **Sharper picture on the Switch:** *Render Resolution* (Recomp Settings > Display) now works in the Vulkan
  renderer. The game is drawn at that share of the output's height: at 100% (the default) 960x720 at 720p and
  1440x1080 at 1080p, instead of the Xbox's 640x480 stretched up. Text and edges are no longer blurry. About 67%
  at 720p gives the old 640x480. The shadow map stays at the console's 512x512.
- **SMAA on the Switch:** the *SMAA* setting smooths edges in the Vulkan renderer too. It costs frame rate on the
  Switch, as does anisotropic filtering; both are off by default.
- **Fewer stalls while a level starts:** the game reads a small surface back every frame then, and each read waited
  for the GPU (up to 170 ms at a time). A finished copy is now ready for it instead.
- **Gyro:** *Invert gyro X axis* (on by default on the Switch, where turning right turned the view left).
  *Calibrate gyro* no longer leaves the gyro dead: it now measures in the menu too, and gives up after 10 seconds
  if the console is not lying still.
- No more false "out of GPU memory" errors in the log.

## v0.4.5
- **Gyro aiming on the Switch:** the Joy-Con (handheld or as a pair) and the Pro Controller turn the view by their
  motion sensors. Off by default: Recomp Settings > Controller > Gyro > Gyro Aiming. *Gyro only while aiming* (on by
  default) lets it turn the view only with the weapon raised or through the sniper scope, the camera stays on the
  right stick otherwise. Sensitivity, axis and calibration as on Windows.
- **Switch button layout:** *Use Switch button layout for A/B* and *... for X/Y* (Recomp Settings > General, both on
  by default) make the face buttons work by their labels: A confirms and B goes back, in the game and every menu.
- **Changing between 720p and 1080p works:** it flickered wildly without changing size, the swap chain being rebuilt
  every frame at the window's old size.
- **Versus is gone from the main menu on the Switch:** it is another program, with no NRO on the Switch.
- **Settings that did nothing on the Switch are gone:** MSAA, shader model 3.0 with HDR rendering and tone mapping,
  the soft shadow filter, ambient occlusion, HDR output, display mode and monitor (the Switch is always full
  screen), the mouse and keyboard page, and the gyro button rows (they name buttons its controllers have not). They
  stay on Windows, where its renderer draws them.
- CI runs on `main` and on pull requests into it only; work goes on in `dev`.

## v0.4.1
- **The shadow calibration in the game's settings works.** The slider sets the game's gamma ramp, which the port
  dropped so far; it is now applied to every frame, as on the Xbox, before the port's own brightness and contrast
  (tested on a Switch V1).
- **Earlier NROs are kept:** a build moves the `scda.nro` it replaces to `BAK/scda_v<version>.nro` beside the
  `switch` folder (so it does not end up on the SD card), instead of overwriting it.
- **The NRO carries its release:** the Homebrew Menu shows the version (0.4.1) instead of 0.1.0, from the version in
  `CMakeLists.txt`; the release workflow checks that a new tag matches it.

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
