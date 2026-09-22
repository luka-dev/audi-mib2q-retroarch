# MU1316 Adreno workaround benchmark — 2026-09-02

Device: Audi MHI2Q, APQ8064/Krait, Adreno 320, GLES 2.0 build `3929146`.
Benchmark version 2 uses the same EGL, Screen window, displayable 43, routed
context 90, and three-buffer path as RetroArch. Unless noted, the render target
is 480x272 RGBA8888 and each result is `GLES calls + glFinish` mean time.

## Driver/GPU split

The comparison below is hidden, pinned to CPU0, and uses the same synthetic
1000-draw `ppsspp_like` command stream:

| Mode | Total ms | Meaning |
|---|---:|---|
| normal | 22.989 | real driver and GPU |
| `INFINITE_FAST_HARDWARE` | 18.528 | full validation/command generation, no GPU submission |
| `INFINITE_FAST_DRIVER` | 2.816 | minimal driver work, no real GL objects |

Approximate decomposition for this synthetic stream: 2.816 ms application/API
dispatch, 15.712 ms driver validation and command generation, and 4.461 ms GPU
submission/execution/backpressure. Thus the dominant limit here is driver CPU
overhead, not pixel rasterization. This decomposition is not claimed for every
real PPSSPP frame: its shaders, vertices, and framebuffer dependencies are more
complex than the synthetic stream.

Artifacts:

- `build/gles2-bench/auto-compression/baseline/run-20260902-032357/`
- `build/gles2-bench/driver-split-cpu0/hardware/run-20260902-032741/`
- `build/gles2-bench/driver-split-cpu0/driver/run-20260902-032756/`

## Runtime controls

| Test | Baseline ms | Variant ms | Result |
|---|---:|---:|---|
| `POWERFLAGS_OVERRIDE`, command-heavy A/B mean | 22.820 | 22.583 | -1.0%, noise-sized |
| `POWERFLAGS_OVERRIDE`, 960x544 fill-heavy | 13.082 | 13.234 | no gain |
| binning `cpu` | 23.205 | 24.054 | +3.7%, worse |
| binning `gpu` | 23.205 | 22.902 | -1.3%, noise-sized |
| binning `direct` | 23.205 | 21.114 | -9.0% for command-heavy work |
| binning `direct`, 32x fill | 5.309 | 9.478 | +78.5%, much worse |
| write-only rendering | 23.205 | 23.430 | no gain |
| auto texture compression | 22.989 | 23.153 | no gain and changes texture quality |

Direct-to-framebuffer avoids tiler/binning overhead when draw-call overhead
dominates, but destroys fill-heavy performance. It is not safe as a global
switch; at most it is a per-render-pass policy after workload classification.

Three Screen buffers reduce producer backpressure: the lightweight swap test
was 16.947 ms with two buffers and 8.478 ms with three. This does not double the
physical display refresh; it only decouples submission. Four buffers are not
supported: Screen created them, but `eglCreateWindowSurface` returned
`EGL_BAD_ALLOC (0x3003)`; immediately afterward, the unit temporarily stopped
answering over SSH.

CPU affinity did not help. CPU0 was the least bad pinned core at 23.274 ms;
CPU1/2/3 measured 24.092/24.943/24.856 ms, versus roughly 22.7-23.1 ms with the
normal scheduler.

## API-traffic workarounds

Same-process routed CPU0 matrix (60 measured samples):

| Pair | Before ms | After ms | Change |
|---|---:|---:|---:|
| 1000 draw calls -> 10 batched calls | 12.143 | 3.634 | -70.1% |
| 1000 draw calls -> 1 batched call | 12.143 | 3.170 | -73.9% |
| 4 uniform calls/draw -> one array call/draw | 22.526 | 18.670 | -17.1% |
| uniform update each draw -> each 8 draws | 22.526 | 10.895 | -51.6% |
| texture bind each draw -> each 8 draws | 20.307 | 10.271 | -49.4% |
| blend state each draw -> each 8 draws | 12.702 | 9.114 | -28.2% |
| `ppsspp_like` -> combined synthetic cache/batch | 24.016 | 14.308 | -40.4% |

Artifact: `build/gles2-bench/final-cpu0-matrix/run-20260902-032937/`.

PPSSPP already eliminates unchanged program and texture bindings in
`GLQueueRunner.cpp`; the archived heavy-scene counters are actual calls after
that cache. The remaining high-value experiments are therefore:

1. count byte-identical uniform updates per `(program, location)` and skip only
   verified duplicates;
2. coalesce compatible uniforms into arrays where shader layout permits;
3. cache unchanged vertex-attrib pointer descriptions inside the draw path;
4. batch only order-independent draws with identical program, textures, blend,
   framebuffer, and vertex layout.

The archived real frame is about 974 draws, 4160 actual uniform calls, 379
actual program binds, and 92 actual texture binds per host frame. Its GLES list
was about 59-60 ms: draw 33-36 ms, uniforms 8-10 ms, program 1-2 ms, texture
5-7 ms. Source: `/fs/sda0/retroarch/logs/ppsspp-era/ppsspp_perf_glqueue_1970_01_01_00_33.log`.

## Render-target bandwidth

| FBO | `ppsspp_like` ms | 32x fill ms | Tradeoff |
|---|---:|---:|---|
| RGBA8888 | 23.205 | 5.309 | reference |
| RGBA4444 | 20.580 | 4.044 | -11.3% / -23.8%; visible precision loss |
| RGB565 | 19.981 | 4.155 | -13.9% / -21.7%; no alpha |

At 960x544 (four times the pixels), `ppsspp_like` rose only 14.6%, from 23.205
to 26.604 ms, while fill-heavy work rose from 5.309 to 13.082 ms. This further
shows that native resolution alone cannot fix command-heavy scenes.

The most aggressive synthetic combination (cached/coalesced commands,
direct-to-framebuffer, RGBA4444) measured 10.753 ms versus the 23.205 ms
reference. It is an upper bound, not a drop-in PPSSPP result: direct mode is bad
for fill-heavy passes, RGBA4444 loses precision, and most real draw/uniform
changes cannot be removed without proving equivalence.
