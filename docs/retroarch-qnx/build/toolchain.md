---
title: Toolchain - GCC 8.5 for QNX 6.5 and its traps
tags: [build, toolchain]
status: verified-hardware
sources:
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md (sections 1, 2, 9)
  - ../qnx-65-sdp-docker/ (external repo: Dockerfile, binutils/build.sh, gcc/build.sh)
  - pkg/runtime-libs/SOURCE.txt
  - git 7f975612, 6b44a9bf
reconciles:
  - README.md "Build" / "Strategy"
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md
---

# Toolchain - GCC 8.5 for QNX 6.5 and its traps

Everything cross-compiles inside one Docker image driven by
`../qnx-65-sdp-docker/host-scripts/qnx-run.sh` (mounts the repo as `/src`). Compiler:
`arm-unknown-nto-qnx6.5.0eabi-gcc` **8.5.0** (the 4.9.4 image is retired; git `6b44a9bf` rebuilt all
artifacts with 8.5). Target ABI: ELF32 ARM, Version5 EABI, VFPv3, `wchar_t=4`, dynamic against the
unit's `libc.so.3` / `libm.so.2`.

## Flags that are not optional

| Flag | Why |
|---|---|
| `-include stddef.h` (C only, never on `.S`) | QNX Dinkum headers do not pull `<stddef.h>` transitively; `array/rhmap.h` etc. need `size_t`/`ptrdiff_t`. Feeding it to the assembler is invalid. |
| `-fno-strict-aliasing` | emulators type-pun guest memory. pcsx/mupen set it themselves; gpSP gets it via `CODE_DEFINES` in `build.sh` (also `-fwrapv`). |
| `-mfpu=neon` (cores) / `-mfpu=vfpv3-d16` (frontend) | cores want NEON; the frontend was never validated with NEON on and keeps d16. |
| `-mtune=cortex-a15` | closest GCC 8.5 model to Krait (no `-mtune=krait`). |
| `-O2`, never `-O3` on cores | an `-O3` PPSSPP build crashed inside `ldqnx.so.2::__gnu_Unwind_Find_exidx`; `-O3` was cleared of blame later but stays untested. |
| `LINK = $(CC)` for the frontend | see libstdc++ ordering below. |

## Trap 1 - gas 2.19 mis-encodes VFP multiply-accumulate (fixed)

The SDP ships `gas 2.19.1` (2007). It encodes `vmls -> vnmls`, `vnmla -> vmls`, `vnmls -> vnmla`
(only `vmla` is right). Upstream fixed this in binutils 2.20 (2009). GCC 4.9 rarely emitted these
patterns; GCC 8.5 emits them freely, so the first 8.5 frontend rendered no text (glyph rasteriser
computed `-(ox + tx*sx)` instead of `tx*sx - ox`) and faulted in `sqrtf`.

No `-f` flag suppresses the patterns (`-ffp-contract=off`, `-frounding-math`, `-fsignaling-nans`
were all tried). **Fix:** the Docker image builds **binutils 2.38 `as`** for the target and makes it
the default assembler; `ld` stays 2.19. A full census of the frontend's assembly shows zero
mismatches with 2.38 (67 with 2.19). The vendored pcsx/mupen makefiles hardcode
`-B/opt/tools/gas-compat/bin`; that shim is repointed to the new `as`.

## Trap 2 - libstdc++ math stubs recurse into themselves (fixed at the toolchain)

`libstdc++-v3/src/c++98/math_stubs_float.cc` is compiled because libstdc++'s `crossconfig.m4`
hardcodes which libm functions QNX has (6 of 23) instead of probing. Its `powf` calls `pow(x,y)`
on floats, which C++ overload resolution sends back to `powf` -> unbounded recursion -> SIGSEGV on a
stack guard page (`libstdc++.so.6@sqrtf+0x1c`, `ref=0005fff8`). Triggers only for NaN/negative
arguments, so it looked intermittent. The old `Makefile.griffin` note blaming NEON for a `powf` hang
was this bug.

Two consequences:

1. **Link the C frontend with `$(CC)`, not `$(CXX)`** so libstdc++ is not ahead of libm in
   `DT_NEEDED` (the griffin blob is pure C: `HAVE_GRIFFIN_CPP := 0`).
2. **The toolchain build now strips every `math_stubs_*` member** from `libstdc++.a` (old QNX `ar d`
   removes one copy per call; there were two) and fails if the archive still defines
   `ceilf expf floorf powf sqrtf`. `tools/qnx-qemu/prepare-static-cxx-runtime.sh` re-validates that
   before any static C++ link. The shipped `pkg/runtime-libs/libstdc++.so.6` (rebuilt 2026-08-24,
   git `7f975612`) defines none of the 15 float entry points. `src/ra_math.c` (a `dlsym` shim) is gone.

## Trap 3 - `std::mutex` lifetime on QNX (fixed for PPSSPP)

GCC 8's default gthread path gives `std::mutex` a static initializer and a no-op destructor. QNX
sync objects are bound to their address, so reusing the heap address later yields `EINVAL` in
`condition_variable`. Force-including `QnxCompat.h` (`_GTHREAD_USE_MUTEX_INIT_FUNC`) pairs explicit
`pthread_mutex_init/destroy`. Only PPSSPP needed it; see [[ppsspp-status]].

## Trap 4 - `__clear_cache` is a no-op

Every ARM dynarec must call `msync(..., MS_INVALIDATE_ICACHE)`. Recipe: [[jit-icache-qnx]].

## Exec-size ceiling

QNX 6.5 on this unit refuses executables above ~15 MB (measured in the gcc49 port). The frontend is
2.33 MB stripped; cores are separate `.so` (`dlopen`). Big buffers go through runtime `mmap`
(unlimited, 240 MB measured), never static BSS (procnto commits it at exec). The Mupen build drops
GLSM's 80 MB uniform-cache array and halves the dynarec cache (BSS 145 MB -> 44 MB).

W^X is **not** enforced on QNX 6.5 (measured): RWX JIT mappings work.

## Verifying a build offline

`tools/qnx-qemu/test-cxx-runtime.sh` boots the real MIB2Q loader/libc under QEMU and runs
throw/catch, the affected libm calls, 32 mutex/condvar recreate cycles and promise/future:
[[qemu-harness]].
