---
title: ARM JIT on QNX 6.5 - the I-cache recipe
tags: [cores, jit, qnx]
status: verified-hardware
sources:
  - docs/legacy/qnx-arm-jit-icache-recipe.md
  - cores-src/gpsp/cpu_threaded.c (platform_cache_sync), cores-src/mupen64plus_next/.../new_dynarec/arm/assem_arm.c (cache_flush), cores-src/pcsx_rearmed/libpcsxcore/new_dynarec/new_dynarec.c, cores-src/ppsspp/Common/ArmEmitter.cpp
reconciles:
  - docs/legacy/qnx-arm-jit-icache-recipe.md
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md §3
---

# ARM JIT on QNX 6.5 - the I-cache recipe

Applies to every ARMv7 dynarec on this target. Same root cause, same one-function fix.

## Symptom

Intermittent crashes/glitches in generated code (invalid branches, jumps to `0xfffffffe`), or a
black screen a few seconds into a game then `SIGSEGV ip=0x048000d8` with **no module name**
(execution inside anonymous memory = JIT code). Or the dynarec was simply disabled "for stability".

## Root cause

ARM I-cache is not coherent with D-cache. Cores call `__builtin___clear_cache()` after emitting;
**on QNX 6.5 with this toolchain it lowers to a no-op** (proven: the old standalone gpSP's
`platform_cache_sync` disassembled to a bare `bx lr`). Everything else - the RWX translation cache
via `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANON|MAP_PRIVATE)`, +-32 MB branch reach - already
worked; only coherency was left to luck.

## Fix

```c
#include <sys/mman.h>
#if defined(__QNXNTO__)   /* compiler-predefined; __BLACKBERRY_QNX__ depends on build flags */
    msync(start, (size_t)((char*)end - (char*)start),
          MS_SYNC | MS_CACHE_ONLY | MS_INVALIDATE_ICACHE);
#else
    __clear_cache(start, end);
#endif
```

| Core | Function | File | State |
|---|---|---|---|
| PCSX-ReARMed | `new_dyna_clear_cache` | `libpcsxcore/new_dynarec/new_dynarec.c` | upstream had it; reference |
| gpSP | `platform_cache_sync` | `cpu_threaded.c` | patched; `HAVE_DYNAREC := 1` in the qnx Makefile block |
| Mupen64Plus-Next | `cache_flush` | `mupen64plus-core/.../new_dynarec/arm/assem_arm.c` | patched; N64 runs on HU |
| PPSSPP | `ArmEmitter.cpp` | `Common/ArmEmitter.cpp` | patched (core not shipped) |

Also fixed while there: `libretro-common/memmap/memmap.c` had a missing `|` before `MS_CACHE_ONLY`
under `#ifdef __QNX__` (that TU is not compiled into the cores, which is why it never surfaced).

## Risk boundary

`MS_INVALIDATE_ICACHE` acts on the **calling CPU**. Sufficient only when compile and execute
happen on the same thread. Cross-core cases (PCSX `drc_thread`, any helper-thread compiler) need
either validation on hardware or pinning with `_NTO_TCTL_RUNMASK`.

## Verification checklist

1. `arm-...-nm -D core.so | grep msync` shows `U msync`.
2. JIT-heavy title for 10+ minutes (GBA Mode-7 racer / Golden Sun; any 3D N64 game), watch slog
   for invalid-branch faults.
3. Only then evaluate threaded options.

Memory note kept in the user's global memory as `qnx-arm-jit-icache-recipe`.
