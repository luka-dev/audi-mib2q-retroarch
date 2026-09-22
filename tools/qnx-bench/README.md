# QNX performance benchmarks

This directory contains two complementary tools.

## Standalone GLES2 benchmark

`qnx_gles2_bench` creates the same EGL/GLES2/Screen display path as the QNX
RetroArch port, but runs deterministic micro-scenarios instead of an emulator.
It is intended to answer whether a workload is limited by synchronous GLES
driver overhead, asynchronously queued GPU work, fill rate, resource upload,
readback, or presentation.

Build it:

```sh
tools/qnx-bench/build-gles2.sh
```

Run it hidden on the connected head unit:

```sh
tools/qnx-bench/run-gles2.sh --scenario all --frames 30
```

Run through RetroArch's real displayable/context route:

```sh
tools/qnx-bench/run-gles2.sh --route --scenario ppsspp_like --frames 60
```

The routed form temporarily switches display 0 to context 90. The remote
supervisor restores the display manager's buffered previous context with
`dmdt sb 0`, including when the benchmark child exits with an error. Do not run
it concurrently with RetroArch; the wrapper refuses to do so.

Each measured sample reports three components:

- `call_*`: time spent inside the GLES calls, including any driver-side block;
- `finish_*`: additional wait in `glFinish()` after those calls return;
- `total_*`: their sum.

A high `call` value and low `finish` value means the cost is synchronous in the
driver calls or they are backpressured by the GPU. A low `call` value and high
`finish` value means work was submitted quickly and completed asynchronously
on the GPU. This distinction is stronger than measuring a whole emulator frame,
but it is not a replacement for unavailable Adreno hardware performance
counters.

`ppsspp_like` deliberately approximates the command density observed in the
heavy God of War capture: 1000 small draws, 4000 uniform updates, 125 texture
binds and 125 state changes per sample. Its shaders and geometry are synthetic,
so it diagnoses command overhead; it does not claim to reproduce the game.

The paired scenarios expose the maximum value of frontend workarounds:

- `uniform_cached8` and `uniform_vec4fv` reduce uniform API traffic;
- `texture_cached8`, `state_cached8`, and `program_1000` isolate redundant
  state/program overhead;
- `draw_batched_10` and `draw_batched_1` render the work of 1000 logical quads
  in ten calls or one call;
- `ppsspp_optimized` combines state caching with one uniform-array upload per
  group of eight draws;
- `fill_32x_discard` measures the upper bound from discarding an FBO whose
  color result is no longer needed.

The firmware advertises old Qualcomm driver controls and binning hints. They
can be enumerated and tested without modifying the firmware:

```sh
tools/qnx-bench/run-gles2.sh --list-driver-controls
tools/qnx-bench/run-gles2.sh --scenario ppsspp_like \
  --driver-control POWERFLAGS_OVERRIDE
tools/qnx-bench/run-gles2.sh --scenario ppsspp_like --binning direct
tools/qnx-bench/run-gles2.sh --scenario fill_32x --fbo-format rgb565
```

`POWERFLAGS_OVERRIDE` is described by this driver as forcing GPU power clocks
high. `INFINITE_FAST_HARDWARE` and `INFINITE_FAST_DRIVER` are diagnostic
controls only: they deliberately stop producing real GPU output and must never
be treated as an optimization. Each test gets a fresh process/context.

On the MU1316 stack, four native buffers are not usable: Screen creates them,
but `eglCreateWindowSurface` then fails with `EGL_BAD_ALLOC`; immediately after
that run, the tested unit temporarily stopped answering. Use only two or three
buffers.

Artifacts are stored under `build/gles2-bench/run-*/`:

- `results.csv`: machine-readable measurements and device metadata;
- `run.log`: initialization, renderer, and human-readable scenario summaries;
- `remote-command.txt`: exact remote environment and invocation.

## Headless libretro benchmark

`ra_bench.c` is a minimal libretro host for CPU/core experiments. It has no EGL
or QNX Screen context, so it cannot measure the Adreno, the GLES driver, or
presentation. Use it for repeatable core/JIT/HLE comparisons only; use the
standalone GLES2 benchmark for graphics-path questions.
