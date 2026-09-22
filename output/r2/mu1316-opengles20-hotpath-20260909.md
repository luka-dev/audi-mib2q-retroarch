# MU1316 `OpenGLES20.so` hot-path audit — 2026-09-09

## Scope and conclusion

This is a static ARM analysis of the stock QNX GLES2 userspace driver, correlated
with the live synthetic measurements recorded on 2026-09-02. No firmware or HU
file was modified.

Analyzed binary:

- `/ifs2/ifs_display/proc/boot/OpenGLES20.so`
- SHA-256: `cc89187b21c921f109e7802ac805879a52002883c0d001355aafc44fd93bbc4d`
- ELF32 ARM EABI5, stripped, image base `0`
- live version string: OpenGL ES 2.0 build `3929146`

The result is consistent with the benchmark split: command-heavy PPSSPP-style
work is limited primarily by CPU work inside this old driver, not by Adreno 320
rasterization. There is no discovered safe switch that removes the expensive
work while preserving correct rendering. The practical target is to send fewer
redundant state changes and fewer compatible draws into the driver.

## Measured split, now confirmed by implementation

The same pinned-CPU0 synthetic stream contains approximately 1000 draws, 4000
uniform calls and 125 texture binds per frame:

| Driver mode | Mean frame time | Work retained |
|---|---:|---|
| normal | 22.989 ms | application, driver and GPU |
| `INFINITE_FAST_HARDWARE` | 18.528 ms | validation and command generation, no GPU submission |
| `INFINITE_FAST_DRIVER` | 2.816 ms | application and public API envelope, stubbed driver |

The approximate decomposition is therefore:

- application/API envelope: 2.816 ms;
- driver validation and command generation: 15.712 ms;
- GPU submission, execution and backpressure: 4.461 ms.

`core_glEnableDriverControlQCOM` at `0x49b18` reaches the common control handler
at `0x499dc`. Control ID 0 sets bit `0x2` and invokes `0x2f0ac`; that function
copies `0x55c` bytes of `ifd_*` function pointers into the context dispatch table
at context offset `0x204`. Most `ifd_*` entries are four-byte immediate-return
stubs. It does not expose a fast but correct renderer.

Control ID 1 sets bit `0x1`. The centralized submit routine at `0x9c8f0` checks
this bit at `0x9d004` and jumps past all four `gsl_command_issueib_sync` call
sites (`0x9d0a4`, `0x9d194`, `0x9d23c`, `0x9d2f4`). This proves that the measured
`INFINITE_FAST_HARDWARE` time retains driver-side preparation while suppressing
GPU submission, exactly as the embedded control description claims.

## Public-call envelope and mutex

`gl2_GetContext` is at `0x45fb4` and has 382 analyzed cross-references. The
exported GLES entry points call it before dispatching indirectly through the
current context table. Examples are `glDrawElements` at `0x2e840`,
`glUniform4fv` at `0x2d780`, and `glBindTexture` at `0x2ef30`.

When the context field at offset `+0x8` is non-null, `gl2_GetContext` calls
`os_mutex_lock` at `0x4601c` and `os_mutex_unlock` at `0x46058`. The associated
backend object is created in `0x9ecb8`; its mutex is named `mem_pool_cleanup`,
created at `0x9fea4`, and stored at backend offset `+0x2e4c`. In
`libOSUser.so`, `os_mutex_lock` (`0x34dc`) and `os_mutex_unlock` (`0x3530`) are
thin wrappers around QNX `pthread_mutex_lock`/`pthread_mutex_unlock`; creation
uses a recursive mutex.

The lock is real per-call overhead, but it is not the 15.712 ms bottleneck:
`INFINITE_FAST_DRIVER` retains all public wrappers and `gl2_GetContext`, yet the
whole synthetic frame costs only 2.816 ms. Even a perfect, unsafe bypass of the
entire public API envelope could recover no more than part of that 2.816 ms for
this workload, about 12% of the normal frame.

Removing the lock or calling internal `core_gl*` functions directly is unsafe.
The same mutex coordinates memory-pool cleanup, and `gl2_GetContext` also handles
a driver-owned surface/supersampling transition described below.

## Forced supersampling branch

The conditional work inside `gl2_GetContext` is tied to panel settings. During
context creation, the driver calls `get_panel_settings` and reads fields at
offsets `+0x730` and `+0x734`. When enabled, it logs
`Forcing super sampling with scale factor: %1.2f`; the default factor is 2.0.
Helper `0x45b10` then creates a hidden scaled framebuffer/renderbuffers in the
tail of the GLES context and later rebinds that framebuffer through
`core_glBindFramebuffer`.

This would be a serious multiplier if applied to RetroArch, but the extracted
MU1316 application configuration contains only:

```text
forceSSAAEnable=gemib
```

at `extracted/app/gemib.factory/adreno_config.txt`. `libpanel.so` checks the
configured process name and uses `QC_GFX_CONF_DIR` (default
`/developer/Adreno-OS`) to find the Adreno configuration. The RetroArch and
benchmark launch commands set `GRAPHICS_ROOT=/proc/boot/` and do not set
`QC_GFX_CONF_DIR`; their process names also do not match `gemib`.

Therefore forced SSAA is not active for the measured RetroArch/benchmark path
under the captured configuration. On a future live run this should still be
confirmed once by checking that the SSAA log message is absent.

## What makes draws expensive

`core_glDrawElementsInstancedXXX` at `0x4920c` is 948 bytes. It unconditionally
calls helper `0x48528` at `0x492e4`. The helper is a 2348-byte validator with
587 ARM instructions, 137 basic blocks and cyclomatic complexity 83. Its paths
are labelled by embedded diagnostic names including:

- `gl_draw_error_checks` (`0xc4d40`);
- `validate_vertex_attrib_state` (`0xc4d58`);
- `validate_samplers` (`0xc4d78`);
- `validate_render_targets` (`0xc4d8c`);
- `validate_transform_feedback` (`0xc4da4`).

This function is not merely optional API error reporting. It validates enabled
attributes and buffer bounds/types, walks samplers and render targets, resolves
derived state, and writes outputs through the pointer at `sp+0x30`. The caller
consumes those outputs in the remaining draw path. NOPing the call or branching
past a single `gl_draw_error_checks` label would leave required state undefined.

The caller then reaches more state preparation, including helper `0x4397c`
(1720 bytes, 430 instructions, 85 basic blocks, complexity 50), before backend
command generation in helper `0xa3e40` (580 bytes, 145 instructions, 21 basic
blocks, complexity 17).

There is no `gsl_command_issueib_sync` directly in the per-draw path. Direct
calls to it are centralized in `0x9c8f0`, and `glFinish` reaches the central
flush/finish path through `core_glFinish` at `0x4f1f0` and helper `0xa99b8`.
Thus the driver does not synchronously submit once for every draw; the dominant
per-draw cost is fine-grained validation and command construction, followed by
batched submission/backpressure.

## Uniform and state-call costs

The common uniform loader at `0x6a7a4` is 1384 bytes, 346 instructions, 76 basic
blocks and complexity 44. It validates program, location, type and count; walks
uniform metadata; compares new data against cached values using VFP loops or
`os_memcmp`; copies changed values; and emits dirty state through `0xa53e8`.
The matrix loader at `0x6a018` is 880 bytes, 220 instructions, 37 basic blocks
and complexity 23 and follows the same validate/compare/update pattern.

The driver already avoids copying identical uniform payloads, but the API call,
lookup, validation and comparison are still paid. Skipping byte-identical
updates in PPSSPP before entering the driver remains useful.

Other representative state paths are also substantial:

- `core_glBindTexture` at `0x66b98`: 952 bytes, 238 instructions, 52 basic
  blocks, complexity 31, including namespace lookup and callbacks;
- `core_glVertexAttribPointer` at `0x6b60c`: 1192 bytes, 298 instructions, 89
  basic blocks, complexity 53.

This matches the archived real frame: about 974 draws, 4160 uniforms, 379
program binds and 92 texture binds, with roughly 59–60 ms in its GLES list
(draw 33–36 ms, uniforms 8–10 ms, texture 5–7 ms, program 1–2 ms).

## Built-in vendor profiler

The firmware already contains
`extracted/app/armle/graphics/QXProfiler.so`. The GLES driver watches
`/pps/services/graphics/profiler`; when its status is enabled, routine
`0x15444` loads `QXProfiler.so`, resolves its shim entry points and selects the
QX dispatch table through `qgl2ToolsJumpTableSelectTarget` at `0x152c8`.

`QXProfiler.so` exports per-API shims such as `shim_glDrawElements`,
`shim_glUniform4fv` and `shim_glBindTexture`, together with
`q3dToolsDriverProfileEnter/Exit`, `q3dToolsNewFrame`, GPU monitor/counter APIs
and `qgl2ToolsQXRegisterWithProfilerApp`. This is the best vendor-supported path
for a later live trace. It may add measurement overhead and appears to require
the matching profiler client/protocol, so it was not enabled while the HU is
offline.

## Ranked next actions

1. Add PPSSPP-side counting and elimination of byte-identical uniform updates,
   keyed by `(program, location, type, count)`, with invalidation on relink and
   context loss.
2. Cache unchanged vertex-attribute pointer/enabled-state descriptions before
   they reach GLES. Measure call counts before and after.
3. Batch only draws proven compatible in program, textures, framebuffer,
   vertex layout, blend/depth/stencil state and ordering. The synthetic test
   reduced 1000 draws to 10 by 70.1%, but that is an upper bound rather than a
   promise for arbitrary PSP frames.
4. Consider a driver-side validation-result cache keyed by explicit state
   generations only after application-side reductions are exhausted. It is
   plausible but invasive: the validator produces data required downstream,
   so a simple no-error patch is not valid.
5. When the HU is available, first verify that forced SSAA is absent, then run
   per-API and GPU-counter profiling through QXProfiler or the existing
   standalone harness. Keep a normal/IFH/IFD control split for every workload.

Do not globally remove `gl2_GetContext`, its mutex, or draw validation. Do not
use `INFINITE_FAST_DRIVER` or `INFINITE_FAST_HARDWARE` for real rendering. Do
not globally force direct binning: it helped the command-heavy synthetic case
by 9.0% but made the fill-heavy test 78.5% slower.

## Evidence and confidence

Proven statically: wrapper dispatch, mutex placement and purpose, SSAA branch,
driver-control table replacement, IFH submit bypass, draw-validation structure,
uniform compare/update structure, centralized GSL submission, and QXProfiler
loading/exports.

Proven by live measurement: the timing decomposition, API-traffic experiment
speedups, real-frame call counts, FBO-format tradeoffs and binning tradeoffs.

Still requiring live validation: exact savings from PPSSPP-side uniform and
vertex-state caches in a game, the percentage of compatible draws that can be
batched without changing rendering, and usable QXProfiler client setup.

Related artifacts:

- `output/r2/mu1316-adreno-benchmark-20260902.md`
- `output/r2/mu1316-adreno-controls.md`
- `output/r2/mu1316-gsl-port-boundary-20260908.md`
- `build/gles2-bench/driver-split-cpu0/`
- `build/gles2-bench/final-cpu0-matrix/run-20260902-032937/`
