# On-device translation: an NRO without game code

Branch `on-device-translation`. Status: **design, nothing implemented yet.**

## Goal
Today the NRO is built from C++ that the recompiler makes out of the user's `default.xbe` (33,172 functions, ~220 MB
of C++), so a built NRO holds the game's code and cannot be shared. The goal is an NRO that holds **no game code** and
can be published: it reads `default.xbe` from `sdmc:/switch/scda/game/` at run time and turns it into AArch64 code on
the console itself. The translated code is kept on the SD card, like the shader cache, and made again only when the
XBE or the translator changes.

This is what the original Windows release does with clang and lld at its first start, done without a compiler: in
effect an emulator for this one game, whose kernel, Direct3D and DirectSound are already native (`src/kernel`,
`src/xdk`, `src/port`).

## Why it is hard
- **Speed.** The game's own code is the limit on the Switch: the console's logs show "the title works 94-100%" at
  25-33 ms a frame. Today GCC `-O2` optimizes every function. Code made on the console without an optimizing compiler
  is slower, and every percent of it costs frame rate.
- **No compiler on the console.** Shipping GCC or LLVM to Horizon is not realistic. The translator has to emit
  AArch64 machine code itself.
- **Executable memory.** Horizon gives homebrew no plain RWX pages. Code goes in through the code alias mechanism the
  port already uses for guest memory (`switch_memory.cpp`: `svcMapProcessCodeMemory`, then RX and RW views).

## Approach
Reuse what the recompiler already knows, and change only where its output goes:

1. **Analysis on the console.** `tools/recomp/analysis.cpp` finds functions, blocks, jump tables and the XDK
   symbols (`config/scda_v2.toml`, `config/xdk_symbols.toml`). It is game independent code: it runs on the console
   at the first start against the user's XBE.
2. **An AArch64 back end** next to the C++ one (`codegen_*.cpp` emits C++ per x86 instruction today): the same
   per-instruction semantics, emitted as machine code. First naive (guest registers in `X86Context`, flags computed
   as now), then:
   - guest registers held in host registers within a block,
   - flags computed only where a later instruction reads them (the C++ back end leaves this to GCC),
   - direct branches between translated functions, `x86_icall` through a table as today.
3. **A translation cache on the SD card** (`code.bin`): the translated functions with a header naming the XBE's
   SHA-1 and the translator's version. The first start translates everything with a progress screen; later starts
   map the file.
4. **The same calling convention** the generated C++ has (`X86_FUNC(ctx, base)`), so hand-written replacements
   (`src/game/`), HLE entry points and the dispatcher keep working unchanged.

## Milestones, each with a gate
1. **Interpreter on Linux** (correctness, not speed): run the x86 instructions from the recompiler's decoder through
   one interpreter loop on the PC build; boot to the main menu. *Gate:* menu and one mission start, logs equal to
   the recompiled build.
2. **Naive AArch64 translator on Linux/AArch64 or under qemu-user:** function by function, no optimizations.
   *Gate:* boots; first speed figure against the recompiled build on the same machine.
3. **On the Switch:** code alias mapping, the SD card cache, the first-start progress screen. *Gate:* frame time in
   the first mission against today's NRO. **If it is more than ~2x slower, stop or rethink** (see below).
4. **Optimizations** in order of measured gain: register caching, flag elimination, block linking, hot function
   profiling.
5. **Release:** a published NRO with no game code, the translator's version in the cache header, the docs updated.

## Fallbacks if the translator stays too slow
- **Hybrid:** publish the translator NRO for everyone, keep the Docker builder for the full-speed build.
- **Hot functions by hand:** the game's busiest functions as hand-written native code (as sa2-nx does), the rest
  translated. Hand-written replacements of game functions are original work, not game code.

## Legal note
The translator and everything it needs are original code. The game's code exists only on the user's console, made
from the user's own copy, as with the Windows release's first-start compile. The published NRO must never contain
translated code, not even a cache for testing, and test caches never go into the repository.
