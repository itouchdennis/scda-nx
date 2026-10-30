# How to build scda-nx (Windows and Linux)
scda-nx is Splinter Cell: Double Agent (Xbox) for the Nintendo Switch. There is no ready-made download of the game
for the Switch, and there never will be: you make it yourself, on your own computer, from **your own, legally
acquired copy** of the Xbox game. The scripts below do that for you in a few clicks. Nothing of the game is
downloaded or uploaded.

**What you need**
- Your copy of Splinter Cell: Double Agent for the Xbox, as a disc image (`.iso`) or as the extracted disc (the folder
  with `default.xbe`, `Default/` and `Media/`). Tested with the USA/Europe release.
- A Nintendo Switch that runs homebrew (the Homebrew Menu).
- About 20 GB of free disk space and an internet connection (the first run downloads the builder, several GB).
- [Docker](https://www.docker.com/): the builder runs inside it, so nothing else gets installed on your computer.

The first build takes a while (from about 20 minutes to over an hour, depending on your PC). Later builds are much
quicker.

## Windows (10 or 11)
**Windows has not been tested yet.** `build-nro-windows.bat` was written for Windows 10 and 11 but has not been
run on a real Windows PC: in the test VM, Docker Desktop could not start (nested Hyper-V). Only the Linux builders
were run from start to finish. If you try it on Windows, please report how it went in an issue.

1. Install [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/) and start it once. Let it
   set up WSL 2 if it asks, and restart your PC if it asks.
2. Download `build-nro-windows.bat` and put it in a folder of its own, for example `C:\scda-nx\`.
3. Double-click `build-nro-windows.bat` and choose your game's `.iso` (or the `default.xbe` in your extracted disc).
   You can also drag the `.iso` or the folder onto the `.bat`.
   - If Windows warns about the file ("Windows protected your PC"): click **More info**, then **Run anyway**. It is a
     plain text file, so you can open it in Notepad and read what it does first.
4. Wait until it says **Your SD card folder**. A window opens with the folder `switch`.
5. Go on with [On the Switch](#on-the-switch).

## Linux
1. Install Docker ([how](https://docs.docker.com/engine/install/)) and allow your user to use it:
   ```sh
   sudo usermod -aG docker $USER
   ```
   Then log out and back in.
2. Download `build-nro-docker.sh`, make it runnable and start it with your game:
   ```sh
   chmod +x build-nro-docker.sh
   ./build-nro-docker.sh "/path/to/your Splinter Cell Double Agent.iso"
   ```
   The extracted disc's folder works as well as the `.iso`.
3. Wait until it says **Your SD card folder**: it is `out/switch` beside the script.
4. Go on with [On the Switch](#on-the-switch).

## On the Switch
1. Copy the folder `switch` (from `out\` beside the script) onto the root of your SD card. It holds
   `switch/scda/scda.nro` and the game files in `switch/scda/game/`.
2. Start the Homebrew Menu with **title takeover**: hold **R** while you start any game, not from the album. The
   album (applet mode) leaves too little memory.
3. Start **Splinter Cell: Double Agent** from the list.

On the first start the shaders are built while you play, so things can be missing or see-through for a moment. They
are saved on the SD card and load at once from then on.

## Good to know
- **Please do not share the `.nro`.** It holds code made from your copy of the game, so it is for you only.
- **Read before you run.** Both scripts are plain text. If you are skeptical (that is a good thing), open them and read
  what they do first, or build by hand as the [README](https://github.com/itouchdennis/scda-nx#readme) describes.
- **An icon in the Homebrew Menu:** put a 256x256 JPEG named `icon.jpg` beside the script before building.
- **Updates:** run the script again. It fetches the newest builder and rebuilds only what changed. The NRO it
  replaces is kept in `out/BAK/`, named after its version (`scda_v0.4.1.nro`).
- **Disk space:** the builder and the build cache live in Docker. To remove them:
  `docker volume rm scda-nx-cache` and `docker image rm ghcr.io/itouchdennis/scda-nx-builder:latest`.
- **Buttons and gyro:** by default A confirms and B goes back, as on the Switch (Recomp Settings > General). Gyro
  aiming is off until you turn it on under Recomp Settings > Controller > Gyro.
- **Logs for a bug report:** add `logging = true` to `sdmc:/switch/scda/scda.toml`; the log is written to
  `sdmc:/switch/scda/logs/`.
- **Tested** only the single player campaign, on a first-model Switch (V1), handheld, with a light overclock. The
  NRO was built with the Linux builders; the Windows builder has not been tested yet.
- **macOS** may work with Docker Desktop too, but it has not been tested (and on Apple Silicon it runs emulated and slowly).
