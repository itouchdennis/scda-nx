# Porting an Xbox recomp game to the Nintendo Switch

This is what it took to bring Splinter Cell: Double Agent (SCDA-XBERecomp) to the Switch. It is written to make the
next port faster. Each item comes in the order it matters in a new port, with the failure you get without it and the
fix. The source files named here are in this repository.

Projects worth reading next to this one: `sa2-nx` (memory mapping), `burnout3-sw` (thread priorities), `nfsu2-sw`
(stack sizes), `nfsmw-nx` (NVK with a pipeline cache, no stutter) and `mesa-switch` (NVK for Horizon).

> This repository contains no game data and no code generated from it. Everything game specific comes from your own
> copy of the game at build time (`tools/regen.sh`).

---

## 0. Starting point and layout

A recomp game has four layers. Only the bottom two depend on the platform:

| Layer | Contents | Platform |
|---|---|---|
| `generated/` | the XBE's x86 code as C++ (recompiler `tools/recomp`), made from your own copy, never committed | shared |
| `src/kernel`, `src/xdk`, `src/audio`, `src/gpu` | Xbox kernel, D3D8, DirectSound, mixer | shared |
| `src/port/*.cpp` | window, input, files, threads, memory, audio output | **per platform** |
| renderer | `gpu_vk.cpp` (Vulkan), `gpu_gl.cpp`, D3D12 on Windows | **per API** |

Rule: everything platform specific goes into `src/port/`, behind `port/port.h`. Then the same port runs on Linux
**and** the Switch, and Linux is the quick test bed.

**Order that worked:**
1. Linux build with the null renderer (`SCDA_GPU=null`): does the game boot, do kernel, files and threads work?
2. Switch build with the null renderer: memory, threads, file system and logs on real hardware.
3. Renderer: **Vulkan on NVK right away**, not GL first (section 5).
4. Audio, input, relaunch (menu → mission → menu), performance.

---

## 1. Build setup

- **A cross build cannot run the recompiler.** `tools/regen.sh` generates the C++ on the PC (`generated/game`),
  `switch/build.sh` only compiles it for AArch64.
- `cmake/portable.cmake` covers Linux and the Switch:
  - **`-fsigned-char` on AArch64**: ARM's `char` is unsigned, x86's signed, and recompiled code and XDK
    reimplementations rely on the latter.
  - Renderer by `SCDA_GPU=vk|gl|null`, sources **filtered per backend**: a glob that pulls `vk_device.cpp` into the
    GL build fails to link.
  - Link-time wraps for libnx and SDL (section 3): `--wrap=SDL_SYS_SetThreadPriority`, `--wrap=threadCreate`,
    `--wrap=threadClose`, `--wrap=condvarWaitTimeout`.
- `switch/build.sh` builds **and stages** the NRO: `build/switch-vk/scda.nro` → `switch_sd/switch/scda/scda.nro`.
  A bare `ninja` does not update `switch_sd/`. Vulkan is the default; `SCDA_GPU=gl` builds the GL renderer.
- SD card layout (`sdmc:/switch/scda/`): `scda.nro`, `game/` (your extracted disc), `scda.toml`, `logs/`, `hdd/`
  (the Xbox hard disk: E:, T:, U:, Z:), `shaders.vkb`.
- **Always start with title takeover** (hold R while starting a game): applet mode leaves too little memory.

---

## 2. Memory: the guest's 4 GB (`switch_memory.cpp`)

The Xbox game expects a 32 bit address space of its own; guest address = `base + VA`.

- Reserve the address space in **libnx's ASLR region**, commit it in 2 MB granules.
- **No `svcCreateCodeMemory` per block**: each code-memory object keeps its page list, and after a few dozen the
  kernel runs out (`0xCE01`). Instead, as sa2-nx does: turn a heap block into module code memory with
  `svcMapProcessCodeMemory`, make it read-write, and map that alias into the window with `svcMapProcessMemory`.
- The **physical windows** (on the Xbox `0x80000000` and `0xF0000000` are the same pages) need the same alias;
  `svcMapProcessMemory` refuses heap memory as its source (`0xD401`).
- A decommitted range is zeroed in place, as a fresh commit reads.
- Check the log for `memory: syscall hints …` and `physical windows … alias`.

---

## 3. Threads, priorities, cores

| Problem | Fix |
|---|---|
| `std::thread::detach` throws "Function not implemented" | provide newlib's `pthread_detach` hook, which libnx leaves out |
| every thread lands on core 0 | deal threads round robin over cores 0 to 2; core 3 is the system's and does not time-slice the default priority |
| the process may hold ~96 kernel threads, ended detached threads keep theirs | close ended detached threads yourself (`threadCreate`/`threadClose` wrapped) |
| SDL's audio "TIME_CRITICAL" becomes 0x3B on the Switch (time-sliced), audio crackles | wrap `SDL_SYS_SetThreadPriority`: TIME_CRITICAL → **0x2B** (as burnout3-sw) |
| guest threads need big stacks | 16 MB per guest thread (as nfsu2-sw), `SCDA_STACK_MB` overrides it |
| the CPU is too slow | raise only the CPU clock: the FastLoad boost drops the GPU to 76.8 MHz |

---

## 4. File system

Each of these cost a crash or an empty file on the Switch's SD card:

- `std::filesystem` queries, and even `stat`/`fstat`, fail with **EIO** at times: get sizes in steps, `fstat` →
  `lseek` to the end → probing.
- **A file open for writing cannot be opened again**: ask its size through the open handle, or `profile.sav` reads
  as 0 bytes.
- New file sizes through `ftruncate`; log short reads and writes.
- Open read-only where only reading.
- **`fsync` the log on every flush**, or it is empty after a hang.

---

## 5. Renderer: Vulkan on NVK from the start

**Lesson from SCDA: GL on switch-mesa 20.1 is a dead end.** It keeps neither program binaries nor a disk cache, so
every start links every shader again and the game stutters at each new material. Program binaries, a shader list
replayed at start and linker threads all failed to fix it. nfsmw-nx runs smoothly on NVK with a pipeline cache, and
the same worked here.

### Building NVK (once)
- `switch/nvk/build-nvk.sh` builds mesa-switch's NVK on Linux without Docker. It needs devkitA64, meson, ninja,
  clang/llvm/libclc/spirv-llvm-translator, rust with `rust-aarch64-gnu`, bindgen, cbindgen, and
  `pacman -S switch-libexpat switch-zlib switch-libzstd` from devkitPro.
- `MESA=/path/to/mesa-switch switch/nvk/build-nvk.sh` installs to `build/nvk-sdk/opt/devkitpro/portlibs/switch`,
  where `switch/build.sh` looks for `lib/libvulkan.a` (or set `SCDA_NVK_SDK`).

### Linking it in
- **Static, no loader**: only `vkGetInstanceProcAddr` is linked, everything else is loaded through it
  (`gpu::vk::fn`). `find_package(Vulkan)` does not work: its config wants libelf, which devkitPro has not.
- Link list: `libvulkan.a expat zstd z nx` (nx twice) `stdc++ m`.
- **Stubs** (`switch_stubs.c`): libSDL2.a pulls in EGL and NVK pulls in libelf (its CUBIN parser, never run on the
  Switch); both are served by stubs that fail cleanly.
- **No SDL window** (`SCDA_NO_WINDOW`): the surface comes from `nwindowGetDefault` + `nwindowSetDimensions` +
  `vkCreateViSurfaceNN`. SDL only does input and audio.

### Backend design that worked
- **Vulkan 1.3 + `VK_EXT_shader_object`**: all state dynamic, no pipeline explosion over D3D8 state combinations;
  push descriptors.
- Images stay in the GENERAL layout with coarse barriers: simple, and fast enough on NVK.
- Negative viewport height for D3D conventions.
- **Persistent shader cache** (`shaders.vkb` on the card), keyed by HLSL hash + entry point, with the driver's
  identity in its header: after a Mesa update it is rebuilt by itself.
- **Async shaders**: 2 builder threads on the Switch (3 on a PC) with 8 MB stacks; a draw whose shader is not built
  yet is skipped. The first time this shows as briefly missing or see-through objects instead of stutter; from the
  second start on they come from the cache ("200 shaders kept … made in 9 ms").
- A binary the driver refuses goes back to "kept" and its key is dropped, so its HLSL is generated again.
- A ring buffer in 4 segments, submits with serial/fence (`kSubmits=8`), `kFramesQueued=2`.
- Vertex uploads **before** the render pass begins.
- NOOVERWRITE locks get the newest finished copy from a ring of mapped read-back buffers: a synchronous read-back
  each frame held the Switch at 8 fps.
- Shared parts in `gpu_common.cpp` (constants, shader keys, present pass), so GL and Vulkan do not drift apart.

### Traps found with GL that hold for any API
- Vertex uploads must reach the **highest vertex a draw reads**, zeros past it: Mesa reads beyond and crashes.
- Keep D3D's front face winding as it is, or objects show their insides.
- `invariant` positions (`gl_Position invariant`, or SPIR-V's) for EQUAL depth passes, or the lighting flickers.

### Testing on a PC
- `build/linux-vk` on RADV is the quick test bed for the same backend code.
- `SCDA_VK_VALIDATE=1` turns on the validation layers (where installed).
- `SCDA_SHOT_EVERY=150` takes screenshots, which can be checked without looking at a screen.

---

## 6. Audio

The mix (DirectSound emulation, ADPCM, HRTF, reverb) is the same on every platform. On the Switch only the output
matters (`sdl_audio.cpp`):

- 5.1 → stereo fold scaled by `1/(1+0.707)`, then a **limiter** with 2 ms look-ahead instead of a hard clamp.
- An own pump thread renders 10 ms buffers into `SDL_QueueAudio`, at least 50 ms deep, at TIME_CRITICAL (→ 0x2B by
  the wrap).
- **The trap that mattered: SDL's Switch driver.** devkitPro's `SDL_switchaudio.c` plays only **two** wave buffers
  and notices one finishing only every 5 ms (`audrenWaitFrame`). At `samples = 512` that left ~5 ms to refill; when
  that was missed the gap sounded like **hard clipping** on steady bass (ambience, menu sounds). The queue in front
  of SDL does not help, and nothing shows in the log. **Fix: `want.samples = 2048` on the Switch**, ~37 ms of
  slack. That ended the crackling.
- Finding it faster than we did:
  - "output starved" or "stream ran dry" in the log means the queue or the game; nothing there and still crackling
    means the driver.
  - `SCDA_AUDIO_DUMP=<file>` captures the mix before the limiter on a PC (raw interleaved float32). Check peak, RMS
    and sample jumps with numpy. Here the mix was clean (peak ~0.06 in the menu), so the problem was behind it.
  - Every 10 s the output logs "audio: the mix peaked at …" when the limiter acted; seen often, the mix is too hot.
- Should 2048 not be enough: bypass SDL and play through libnx's `audout` with more buffers.

---

## 7. Relaunch (menu ↔ mission)

Xbox games restart between menu and mission (`HalReturnToFirmware`). On the Switch hbloader starts the next NRO
through `envSetNextLoad` **in the same process, at the same address, on the same heap**: whatever the old run
leaves behind, the new one finds. This was the hardest part of the port:

1. **Pause** every other thread (Horizon cannot end another thread).
2. Give back their **stack mirrors**, with exactly the size `threadCreate` mapped (stack + reent + TLS, as libnx's
   `threadClose` computes it). `Thread::stack_sz` alone is refused by `svcUnmapMemory`, and every relaunch kept
   40 × 16 MB.
3. Release the guest mappings.
4. **Close the nvdrv and audren sessions yourself**, not with `nvExit`/`audrenExit`, whose cleanup waits forever;
   also **libnx's private clone of the nvdrv session** (`g_nvSrvClone`, its address read from `nvIoctl`'s adrp/add),
   the transfer memory handles and the sockets. Then wait (up to 3 s) for the services to let go of the heap, and
   mark the GPU's buffers cached again (otherwise ~600 MB stay uncached per mission).
5. **Wake every condvar** a paused thread still waits on (`condvarWaitTimeout` wrapped). The kernel keeps waits by
   address, and the next run puts its objects at the same addresses: its `notify_one` woke the old render thread
   instead, and the mission hung on its loading screen.
6. Leave without exit handlers a paused thread could hold up.
7. In the new run: the heap is the **largest plain read/write stretch** of what hbloader hands over; pages still
   uncached are marked cached first.
8. Launch data travels in `hdd/launch.bin`, the last image through `PersistDisplay`.

Diagnosis: `logs/boot.txt` keeps the whole relaunch chain, each step of the leave unbuffered, the blocked heap by
kind, and an "alive" every 10 s.

---

## 8. Input and the rest

- **No SDL text input**: it opens the system keyboard.
- 720p by default (native in handheld mode).
- Errors before the game starts go to the **error applet**, so the player sees why nothing starts.

---

## 9. Diagnostics (build them early, they paid off every time)

- **Crash handler**: host and guest state into the log, addresses relative to the NRO's start → `addr2line -e
  scda.elf`; `crash.txt` and `boot.txt` in `logs/`.
- **Watchdog**: each guest thread's last indirect call and a stack excerpt every 15 s.
- Debug builds trace the first 3000 kernel calls.
- **Frame time breakdown** every 10 s: the title's work against its waits for the GPU, read-backs and flushes, plus
  the renderer's ms per phase. It tells at once whether the CPU or the GPU is the limit.
- `nxlink -s`: stderr and the log go to the PC live.

---

## 10. Checklist for the next game

- [ ] `tools/regen.sh` with the new XBE and a recompiler config; the Linux build with `SCDA_GPU=null` boots
- [ ] paths, NRO name, title ID and the relaunch target per XBE in `switch_platform.cpp`
- [ ] Switch build with `null`: memory mapping, threads, files and logs work on hardware
- [ ] take over the Vulkan backend (`gpu_vk.cpp`, `vk_device.*`, `gpu_common.*`); first on a PC with RADV, then NVK
- [ ] name the shader cache file per game; async shaders on
- [ ] audio: `samples = 2048` on the Switch, the priority wrap in place; check a dump on a PC
- [ ] relaunch menu → mission → menu at least twice in a row (the leaks show from the third run on)
- [ ] saves: write → restart the game → load (the "second open" trap)
- [ ] game specific patches (skipped intros, HUD, split screen) go into `src/game/`, not the port

**What saves time next time:** skip GL and start with NVK, take the relaunch logic from here as it is, and set the
audio buffer size right from the start. Those were the three longest detours here.
