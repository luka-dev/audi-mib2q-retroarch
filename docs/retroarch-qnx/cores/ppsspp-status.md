---
title: PPSSPP - ported, measured, not shipped
tags: [cores, ppsspp, research]
status: verified-hardware
sources:
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md §3-§10, §12
  - cores-src/ppsspp/ (Common/ArmCPUDetect.cpp, Common/GPU/OpenGL/GLQueueRunner.cpp, libretro/libretro.cpp)
  - docs/legacy/mu1316-adreno-benchmark-20260902.md
  - build/excluded-ppsspp-20260829/
  - git 8c54b47e
reconciles:
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md (PPSSPP parts)
---

# PPSSPP - ported, measured, not shipped

PPSSPP v1.20.4 was ported (git `e1651bc4`, `44400c6a`), ran on the unit, and was removed from the
production image on 2026-09-22 (`8c54b47e`). The source and all QNX patches stay in
`cores-src/ppsspp/`; the last built artefacts are under `build/excluded-ppsspp-20260829/`.

## The source is still here

Nothing was deleted from the tree. `cores-src/ppsspp/` is the full vendored port - 19 781 files,
including the `platform=qnx` block in `libretro/Makefile`, `Common/QnxCompat.h`, the JIT I-cache
fix and the QNX-built FFmpeg static libraries under `ffmpeg/blackberry/armv7/`. What changed in
`8501f8b4` is that `build.sh` no longer builds it and the image no longer carries the `.so`, its
info file, the `psp` content directory or the `system/PPSSPP` assets; the last built artefacts sit
in `build/excluded-ppsspp-20260829/`.

To build it again, inside the toolchain container ([[toolchain]]):

```bash
. VENDORED_SOURCES.env                       # PPSSPP_SOURCE_COMMIT is still recorded
V=$(printf '%.7s' "$PPSSPP_SOURCE_COMMIT")
tools/qnx-qemu/prepare-static-cxx-runtime.sh /tmp/qnx-static-runtime   # sanitized libstdc++.a
cd cores-src/ppsspp/libretro
make clean platform=qnx GIT_VERSION="$V"
make -j4 platform=qnx GIT_VERSION="$V" QNX_STATIC_CXX_LIBDIR=/tmp/qnx-static-runtime
arm-unknown-nto-qnx6.5.0eabi-strip ppsspp_libretro_qnx.so -o /src/build/ppsspp_libretro.so
```

Then stage it by hand: the core into `cores/`, `pkg/info/ppsspp_libretro.info`, a `psp` rule in
`content-rules.cfg` ([[content-discovery]]) and the `flash0`, `lang`, `vfpu` asset subtrees into
`system/PPSSPP` on the card - all four are preserved in `build/excluded-ppsspp-20260829/`.

## Why it is out

The stock Adreno GLES2 driver (`OpenGLES20.so` build 3929146) spends most of a PSP frame in
**CPU-side validation and command generation**, not on the GPU:

| synthetic `ppsspp_like` stream (1000 draws, 4000 uniforms, 125 texture binds) | ms |
|---|---:|
| normal | 22.99 |
| `INFINITE_FAST_HARDWARE` (driver work, no GPU submit) | 18.53 |
| `INFINITE_FAST_DRIVER` (API envelope only) | 2.82 |

-> ~15.7 ms/frame is driver validation. A real God of War frame: 974 draws, 4160 uniforms, 379
program binds, 92 texture binds = 59-60 ms of GLES list time. No safe driver switch removes that
work ([[adreno-driver-hotpath]], [[adreno-driver-controls]]); only sending fewer calls helps
(batched draws -70 %, 8-draw uniform/texture caching -50 %) and that is an upper bound. The path
around it is a different GL stack: [[freedreno-qnx]].

## What the port fixed (keep - all still apply if it returns)

| Fix | Where |
|---|---|
| JIT I-cache `msync` on QNX | `Common/ArmEmitter.cpp` ([[jit-icache-qnx]]) |
| QNX CPU detection read from syspage: 4 Krait cores, NEON, VFPv4, IDIV (+ Krait MIDR workaround when `ARM_CPU_FLAG_IDIV` is absent) - previously reported 1 core / no NEON and disabled the fast paths | `Common/ArmCPUDetect.cpp` |
| `std::mutex` lifetime (`_GTHREAD_USE_MUTEX_INIT_FUNC` via force-included `QnxCompat.h`) | all C++ TUs ([[toolchain]]) |
| static repaired libstdc++ (no `libstdc++.so.6` dependency; `NEEDED: libGLESv2 libEGL libm libc`) | libretro Makefile + `prepare-static-cxx-runtime.sh` |
| `VertexJit` NEON loads of packed S8/S16 vectors -> exact byte/halfword loads (QNX strict alignment) | vertex decoder |
| SaveState buffer validation, balanced pause/resume, `retro_serialize_size` no longer leaves the emu thread paused | `libretro/libretro.cpp` |
| shader cache kept in RAM (FAT32 SD writes ~8.7 MB/s = 200 ms stalls); `PSP/SYSTEM/CACHE` created so `.glshadercache` can be written at all | `libretro/libretro.cpp` |
| compact perf telemetry core option (`Disabled / Log only / On-screen + log`) - the generic `fps_show` overlay alone cost frames | `GLQueueRunner`, `libretro.cpp` |
| `-mtune=cortex-a15`, `-O2`, `neon-vfpv4` | libretro Makefile |

## Measured profile (God of War: Ghost of Sparta, 3 x 30 s)

JIT code 31-34 %, `libc` memcpy 22-24 %, core 20-24 %, `OpenGLES20.so` 19-24 %, RetroArch 1.1 %.
Inside the core nothing dominates (`NotifyBlockTransferAfter` 11-14 %, `SasInstance::MixVoice`
5-10 %, `LoadClut` 7-8 %, `FastLoadBoneMatrix` 5-6 %). Audio crackle and frame drops were one
symptom: PSP audio mixing shares the CPU. Level loads (`idle_max` 593-827 ms) drained the audio
reserve; CSO was rejected (compression cuts bytes, not seeks: ~6.6 ms per 64 KB random read).
Options tried: `auto_frameskip` (broke pacing), skip buffer effects/readbacks (bookkeeping
remains), GPU skinning (no effect), `video_vsync=true` (**-22.6 ms/frame of idle spin**, kept).

## Open when it was parked

- Save/Load state on hardware after the static-runtime repair - never re-tested.
- `AsyncIOManager::WaitResult` at 22.5 % of core CPU during cutscenes (active wait).
- `DisplayProperties::GetDeviceOrientation` at 10.2 % (a getter in a hot loop).
- Only one (heavy) title was profiled; a light title would separate port cost from hardware limit.

QEMU unit suites still pass: 24/24 groups ([[qemu-harness]]).
