---
title: Cores - what ships and how each is configured
tags: [cores]
status: verified-source
sources:
  - build.sh
  - cores-src/gpsp/Makefile (qnx block), cores-src/pcsx_rearmed/Makefile.libretro, cores-src/mupen64plus_next/Makefile
  - pkg/retroarch-core-options.cfg
  - build/mnt_app/root/retroarch/cores/ (sizes)
reconciles:
  - README.md milestone 3, "Strategy"
  - docs/legacy/qnx-arm-jit-icache-recipe.md "Status in this repo"
---

# Cores - what ships and how each is configured

Three cores, each a separate `dlopen`ed `.so` (keeps every ELF far under the ~15 MB exec ceiling,
[[toolchain]]). All three carry the QNX `msync` I-cache path ([[jit-icache-qnx]]) - `U msync` in each
stripped binary.

| Core | System | Stripped size | CPU | Video | Notes |
|---|---|---|---|---|---|
| gpSP `8b5812e5` | GBA | 672 KB | compiled with `HAVE_DYNAREC=1` + `MMAP_JIT_CACHE`, **runs the interpreter** (`gpsp_drc = "disabled"`) | software | the standalone gpSP that worked on this unit also ran the interpreter; the dynarec can be switched on per card from Core Options |
| PCSX-ReARMed `8fc35f30` | PS1 | 1.42 MB | ARM dynarec ON + NEON (`pcsx_rearmed_drc = "enabled"`) | software GPU (NEON) | SPU on its own thread; CD read-ahead 256; XA + CD-DA kept (inverted `no*` keys) |
| Mupen64Plus-Next `f275caf4`+QNX | N64 | 3.23 MB | ARM new_dynarec (`mupen64plus-cpucore = "dynamic_recompiler"`) | GLideN64 GLES2, HLE RSP, 640x480 4:3 | threaded GL renderer with waiter-gated queue ([[qnx-sync-cost]]); BSS cut 145 -> 44 MB |
| ~~PPSSPP v1.20.4~~ | PSP | 14 MB | ARM JIT + NEON | GLES2 | **not shipped** ([[ppsspp-status]]) |

## Build flags per core (`build.sh`)

- gpSP: `CODE_DEFINES="-mfpu=neon -fno-strict-aliasing -fwrapv"` (gpSP is the one core whose
  makefile does not set no-strict-aliasing; it type-puns the GBA memory map).
- PCSX: upstream `platform=qnx` block (NEON dynarec); `Makefile.libretro`.
- Mupen: `platform=qnx` with `CC/CXX/AR/STRINGS` from the SDP; `-B` gas shim repointed to gas 2.38.

## What is upstream-QNX vs ours

- **PCSX-ReARMed** already had `new_dyna_clear_cache` with the QNX `msync` branch - the reference
  implementation. Our fixes: GTE `r0` clobber ([[qnx-sync-cost]] §2), QNX libretro build.
- **gpSP**: `platform_cache_sync` patched; QNX makefile block; RWX JIT buffer via `mmap`.
- **Mupen64Plus-Next**: `cache_flush` patched; GLSM 80 MB uniform cache removed; dynarec cache
  halved; `BlockingQueue` / `RingBufferPool` waiter gating; exact vertex span; gas/GCC 8.5 compat.

## Threaded-JIT caveat

`msync(MS_INVALIDATE_ICACHE)` invalidates the **calling core's** I-cache. Safe when the same thread
compiles and executes (gpSP, Mupen dynarec, PCSX with `drc_thread` off). PCSX's optional
`pcsx_rearmed_drc_thread` compiles on another core and is **not** validated; keep it off or pin
compile+execute with `_NTO_TCTL_RUNMASK`.

## Runtime knobs that matter on this unit

`pcsx_rearmed_cd_readahead = 256` absorbs SD stalls (random 64 KB read ~6.6 ms on the card).
Frameskip is disabled everywhere: with blocking-audio pacing it removes the rate limiter and
pitches audio up ([[frame-and-audio-pacing]]).

Content rules map extensions to cores: [[content-discovery]]. Save/load-state validation per core:
[[hardware-validation-matrix]].
