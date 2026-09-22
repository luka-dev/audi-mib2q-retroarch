---
title: GLES2 micro-benchmark and session runner
tags: [perf, tools, gpu]
status: verified-hardware
sources:
  - tools/qnx-bench/{qnx_gles2_bench.c,ra_bench.c,build-gles2.sh,run-gles2.sh,run-session.sh,README.md}
  - docs/legacy/mu1316-adreno-benchmark-20260902.md
  - build/gles2-bench/**/results.csv
reconciles:
  - tools/qnx-bench/README.md
  - docs/legacy/mu1316-adreno-benchmark-20260902.md
---

# GLES2 micro-benchmark and session runner

## `qnx_gles2_bench`

Same EGL / Screen / displayable 43 / context 90 / 3-buffer path as RetroArch, but deterministic
micro-scenarios instead of an emulator. Each sample reports `call_*` (time inside GLES calls incl.
driver blocking), `finish_*` (extra wait in `glFinish`), `total_*`. High `call` = synchronous driver
cost or GPU backpressure; high `finish` = fast submit, async GPU work.

```sh
tools/qnx-bench/build-gles2.sh
tools/qnx-bench/run-gles2.sh --scenario all --frames 30            # hidden
tools/qnx-bench/run-gles2.sh --route --scenario ppsspp_like --frames 60   # through context 90; dmdt sb 0 restores
tools/qnx-bench/run-gles2.sh --list-driver-controls
tools/qnx-bench/run-gles2.sh --scenario ppsspp_like --driver-control POWERFLAGS_OVERRIDE
tools/qnx-bench/run-gles2.sh --scenario ppsspp_like --binning direct
tools/qnx-bench/run-gles2.sh --scenario fill_32x --fbo-format rgb565
```

The wrapper refuses to run while RetroArch is active. Artefacts: `build/gles2-bench/<run>/`
(`results.csv`, `run.log`, `remote-command.txt`).

`ppsspp_like` = 1000 small draws, 4000 uniform updates, 125 texture binds, 125 state changes per
sample (the density of the God of War capture). Paired scenarios isolate one workaround each:
`uniform_cached8`, `uniform_vec4fv`, `texture_cached8`, `state_cached8`, `program_1000`,
`draw_batched_10/1`, `ppsspp_optimized`, `fill_32x_discard`.

## Results (2026-09-02, 480x272 RGBA8888 unless noted)

**Driver/GPU split** (CPU0 pinned): normal 22.99 ms; `INFINITE_FAST_HARDWARE` 18.53;
`INFINITE_FAST_DRIVER` 2.82 -> API 2.8 / driver 15.7 / GPU 4.5 ms.

| Runtime control | Baseline | Variant | Verdict |
|---|---:|---:|---|
| `POWERFLAGS_OVERRIDE` | 22.82 | 22.58 | noise |
| binning `cpu` / `gpu` | 23.21 | 24.05 / 22.90 | worse / noise |
| binning `direct` | 23.21 | 21.11 | -9 % command-heavy, **+78 % on 32x fill** - never global |
| write-only rendering, auto texture compression | - | - | no gain |
| 3 Screen buffers vs 2 (lightweight swap) | 16.95 | 8.48 | keep 3; 4 = `EGL_BAD_ALLOC` + unit hang |
| CPU affinity (CPU0..3) | 22.7-23.1 | 23.3 / 24.1 / 24.9 / 24.9 | scheduler is best |

| API-traffic workaround | Before | After | Change |
|---|---:|---:|---:|
| 1000 draws -> 10 / 1 batched | 12.14 | 3.63 / 3.17 | -70 % / -74 % |
| 4 uniform calls/draw -> one array call | 22.53 | 18.67 | -17 % |
| uniform update each draw -> each 8 | 22.53 | 10.90 | -52 % |
| texture bind each draw -> each 8 | 20.31 | 10.27 | -49 % |
| blend state each draw -> each 8 | 12.70 | 9.11 | -28 % |
| `ppsspp_like` -> combined | 24.02 | 14.31 | -40 % |

FBO format: RGBA4444 -11 %/-24 % (precision loss), RGB565 -14 %/-22 % (no alpha). 960x544 raises
`ppsspp_like` only 15 % but fill-heavy 146 % - resolution is not the lever for command-heavy scenes.
Most aggressive synthetic combination: 10.75 ms vs 23.21 (upper bound, not drop-in).

## `ra_bench`

Minimal headless libretro host (no EGL/Screen): repeatable core/JIT/HLE comparisons only.

## `run-session.sh`

`tools/qnx-bench/run-session.sh --core <name> --content <path> --seconds N [--profile-secs M]`:
one ssh session that launches `ra.sh` with content, optionally runs [[profiler]] mid-session, stops
RetroArch, harvests `ra_run/ra_hook/ra_audio`, the newest `retroarch__*.log`, `qsa_perf`, and stores
everything under `build/bench/<tag>/`. It exports `PATH=/armle/usr/bin:/armle/bin:$PATH` first
because the ssh login PATH lacks `date`.
