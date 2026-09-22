---
title: RetroArch on MHI2Q - Knowledge Index
tags: [moc]
status: complete
---

# RetroArch on MHI2Q - Knowledge Index

Map of Content for the RetroArch / QNX 6.5 / Audi MHI2Q (MU1316) port. One topic per note; every
claim is checked against source, firmware or a device log, and each note's `status` says how.
`reconciles:` lists the legacy documents folded into it.

> 34 notes. Start with [[architecture]], then [[hardware-validation-matrix]] for what is proven.

## [architecture](architecture.md) - the two halves, process topology, data flow, quickref

## Build
- [toolchain](build/toolchain.md) - GCC 8.5 SDP image, gas 2.19 VFP bug, libstdc++ math-stub recursion, exec ceiling
- [build-pipeline](build/build-pipeline.md) - `build.sh` stages, staging trees, SD content preservation
- [java-jar-build](build/java-jar-build.md) - `lsd_patch/build.sh` gates: forbidden shadows, class major 48, call contracts
- [vendored-sources](build/vendored-sources.md) - upstream commits and local change summary per tree

## Deploy
- [filesystem-layout](deploy/filesystem-layout.md) - `/mnt/app` (ro) vs `/fs/sda0` (rw), trees, path resolution, factory reset
- [launcher-ra-sh](deploy/launcher-ra-sh.md) - `ra.sh` step by step, env contract, io-hid ownership, log rotation
- [install-procedure](deploy/install-procedure.md) - ssh, transfer idioms, app/SD install, first-run checks, diagnostics harvest
- [configuration](deploy/configuration.md) - factory `retroarch.cfg`, core options, versioned migrations

## HMI (Java, `ra_mhi2q.jar`)
- [games-menu-injection](hmi/games-menu-injection.md) - the one shadow, Games row, fail-closed SystemSMM table swap, RaScreen 250
- [session-lifecycle](hmi/session-lifecycle.md) - connect/disconnect, generations, `/tmp` markers, exit watcher, re-entry latch
- [signals-and-lock](hmi/signals-and-lock.md) - SIGTERM/41/42, `sigwaitinfo` thread, per-frame reconcile, lock-file reality
- [display-context-90](hmi/display-context-90.md) - displayable 43, context `{16,43}`, `dmdt`, why not patch the display manager
- [audio-session](hmi/audio-session.md) - focus 2 / connection 20 / route MPL1 / fade; `MediaSessionBridge`; loss, recovery, release

## Native (C, `retroarch`)
- [video-context](native/video-context.md) - libdisplayinit, 3 buffers, interval 0, bounded Screen vsync, tracer
- [audio-qsa](native/audio-qsa.md) - `mpl1_int_ent`, 6 voices, prefill + ready marker, concealment, DRC, telemetry
- [input-hid-xusb](native/input-hid-xusb.md) - HIDDI + descriptor parser, XUSB/GIP, autoconfig data, rumble, debugging
- [content-discovery](native/content-discovery.md) - `content-rules.cfg`, playlist scanner behaviour

## Cores
- [cores-overview](cores/cores-overview.md) - gpSP / PCSX-ReARMed / Mupen64Plus-Next: flags, options, dynarec state
- [jit-icache-qnx](cores/jit-icache-qnx.md) - `__clear_cache` is a no-op; `msync(MS_INVALIDATE_ICACHE)` recipe and risk boundary
- [qnx-sync-cost](cores/qnx-sync-cost.md) - condvar signal = kernel call (59 % CPU); PCSX GTE `r0`; GLideN64 vertex span
- [ppsspp-status](cores/ppsspp-status.md) - ported, profiled, removed; what the port fixed; why the GPU driver blocks it

## Performance
- [frame-and-audio-pacing](perf/frame-and-audio-pacing.md) - vsync vs blocking audio, measurements, the open `video_vsync` question
- [profiler](perf/profiler.md) - `ra_prof`, decoding libc kernel stubs, core dumps, `tracelogger` warning
- [gles2-benchmark](perf/gles2-benchmark.md) - `qnx_gles2_bench` scenarios and the full 2026-09-02 result matrix; `run-session.sh`

## Reverse engineering (MU1316 firmware)
- [adreno-driver-hotpath](re/adreno-driver-hotpath.md) - `OpenGLES20.so`: validator, uniform compare, mutex, SSAA, QXProfiler
- [adreno-driver-controls](re/adreno-driver-controls.md) - `GL_QCOM_driver_control` ids and extensions
- [egl-swap-path](re/egl-swap-path.md) - libdisplayinit buffer count, eglsub-screen condvar wait
- [gsl-port-boundary](re/gsl-port-boundary.md) - `libGSLUser` ABI, `_IO_MSG` subtypes, memdesc layouts

## Research
- [freedreno-qnx](research/freedreno-qnx.md) - Mesa Freedreno A3xx over stock GSL: architecture, QEMU evidence, HU gates

## Testing
- [qemu-harness](testing/qemu-harness.md) - MIB2Q QEMU runtime, C++/EHABI smoke, libretro-common + PPSSPP suites
- [hardware-validation-matrix](testing/hardware-validation-matrix.md) - proven on the unit vs pending

## History
- [changelog](history/changelog.md) - commit timeline and the decisions not to re-litigate

---

## Verification

| status | meaning | notes |
|---|---|---|
| `verified-hardware` | measured or observed on the MU1316 unit | toolchain, signals-and-lock, video-context, audio-qsa, input-hid-xusb, content-discovery, jit-icache-qnx, qnx-sync-cost, ppsspp-status, frame-and-audio-pacing, profiler, gles2-benchmark, games-menu-injection |
| `verified-decompile` | read from the firmware binaries | adreno-driver-hotpath, adreno-driver-controls, egl-swap-path, gsl-port-boundary |
| `verified-trace` | confirmed against device logs / scripts | install-procedure, hardware-validation-matrix |
| `verified-source` | confirmed against this repo's source | architecture, build-pipeline, java-jar-build, vendored-sources, filesystem-layout, launcher-ra-sh, configuration, session-lifecycle, cores-overview, qemu-harness, changelog |
| `partially-verified` | older part proven on HU, newest change built only | audio-session (MediaSessionBridge), display-context-90 (`{16,43}`) |
| `research` | offline/QEMU evidence only | freedreno-qnx |

**Corrections caught while reconciling the legacy docs:**
- README: "gpSP dynarec ENABLED" - compiled in, but the factory option `gpsp_drc = "disabled"` runs the interpreter.
- README: `flock` serialization on `/fs/sda0/retroarch/ra.lock` - `flock` is unsupported on FAT32 and tmpfs; the file is a PID file at `/tmp/retroarch.lock`; serialization is done in Java.
- README / RetroArchHook comment: "launch only at route" - native is launched **before** focus/connection so QSA exists when connection 20 starts.
- README: HOLD-BACK SIGKILL escalation and SIGUSR1/2 pause from the HMI - designed, not implemented (no sender).
- README "Build": GCC 4.9.4 / `qnx-gcc49` - retired; everything is GCC 8.5 + gas 2.38.
- README milestones: "four supported cores" - three; PPSSPP is out of the image.
