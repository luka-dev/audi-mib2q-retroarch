---
title: OpenGLES20.so hot path - where the driver spends 16 ms
tags: [re, firmware, gpu]
status: verified-decompile
sources:
  - docs/legacy/mu1316-opengles20-hotpath-20260909.md
  - firmware /ifs2/ifs_display/proc/boot/OpenGLES20.so (SHA-256 cc89187b..., GLES 2.0 build 3929146)
  - build/gles2-bench/driver-split-cpu0/, final-cpu0-matrix/
reconciles:
  - docs/legacy/mu1316-opengles20-hotpath-20260909.md
---

# OpenGLES20.so hot path - where the driver spends 16 ms

Static ARM analysis of the stock Adreno 320 GLES2 userspace driver, correlated with the live
measurements in [[gles2-benchmark]]. Firmware unmodified.

## Conclusion

Command-heavy work is limited by **CPU work inside the driver** (validation + command construction),
not by rasterization. There is no safe switch that removes it while keeping correct rendering. The
practical lever is fewer redundant state changes and fewer compatible draws reaching the driver.

## Structure

- Every exported GLES entry (`glDrawElements` 0x2e840, `glUniform4fv` 0x2d780, `glBindTexture`
  0x2ef30, ...) calls `gl2_GetContext` (0x45fb4, 382 xrefs), takes a **recursive mutex**
  (`mem_pool_cleanup`, backend +0x2e4c; `os_mutex_lock` in `libOSUser.so` wraps `pthread_mutex_lock`)
  and dispatches through the context table at +0x204. The envelope is real but small:
  `INFINITE_FAST_DRIVER` keeps it all and costs 2.8 ms.
- `core_glDrawElementsInstancedXXX` (0x4920c) unconditionally calls a 2348-byte validator
  (0x48528: 587 instructions, 137 blocks, cyclomatic complexity 83) whose labelled paths include
  `gl_draw_error_checks`, `validate_vertex_attrib_state`, `validate_samplers`,
  `validate_render_targets`, `validate_transform_feedback`. It **produces state consumed downstream**
  (outputs via `sp+0x30`); NOPing it leaves the draw undefined. Then helper 0x4397c (1720 B,
  complexity 50) and backend command generation 0xa3e40.
- No per-draw `gsl_command_issueib_sync`: submission is centralized in 0x9c8f0 (four call sites);
  `glFinish` reaches it via `core_glFinish` 0x4f1f0 -> 0xa99b8. So per-draw cost is validation and
  command construction, followed by batched submit/backpressure.
- Uniform loader 0x6a7a4 (1384 B, complexity 44) validates program/location/type/count, walks
  metadata, **compares against cached values** (VFP loops / `os_memcmp`), copies changes, marks dirty
  (0xa53e8). Identical payloads are not copied, but the call/lookup/compare is paid.
  `core_glBindTexture` 0x66b98 (complexity 31), `core_glVertexAttribPointer` 0x6b60c (complexity 53).

## Driver controls, proven in code

`core_glEnableDriverControlQCOM` 0x49b18 -> handler 0x499dc. Control 0 (`INFINITE_FAST_DRIVER`)
sets bit 0x2 and copies 0x55c bytes of `ifd_*` stubs over the dispatch table (mostly immediate
returns). Control 1 (`INFINITE_FAST_HARDWARE`) sets bit 0x1, checked at 0x9d004 to jump past all
four `gsl_command_issueib_sync` sites. Both are diagnostics only. Full list: [[adreno-driver-controls]].

## Forced supersampling

`gl2_GetContext` carries a panel-settings branch: `get_panel_settings` fields +0x730/+0x734, logs
`Forcing super sampling with scale factor: %1.2f` (default 2.0), creates a hidden scaled FBO and
rebinds it. The extracted MU1316 config only has `forceSSAAEnable=gemib`
(`app/gemib.factory/adreno_config.txt`); `libpanel.so` matches the process name and reads
`QC_GFX_CONF_DIR` (default `/developer/Adreno-OS`). RetroArch/bench set `GRAPHICS_ROOT=/proc/boot/`,
no `QC_GFX_CONF_DIR`, other process names -> SSAA not active. Confirm once on a live run by the
absence of that log line.

## Vendor profiler

`app/armle/graphics/QXProfiler.so` exists; the driver watches `/pps/services/graphics/profiler` and,
when enabled, loads it (routine 0x15444, `qgl2ToolsJumpTableSelectTarget` 0x152c8) with per-API
shims and GPU counters. Best vendor path for a live trace; needs the matching client and was not
enabled while the HU is offline.

## Ranked next actions

1. PPSSPP-side elimination of byte-identical uniform updates keyed `(program, location, type, count)`.
2. Cache unchanged vertex-attrib pointer/enabled state before GLES.
3. Batch only draws proven compatible (program, textures, FBO, layout, blend/depth/stencil, order).
4. A driver-side validation-result cache keyed by state generation - invasive, last resort.
5. On the HU: verify SSAA absent, then QXProfiler / harness per-API split for every workload.

Never: remove `gl2_GetContext`/its mutex, patch out draw validation, use the infinite-fast controls
for real rendering, force direct binning globally.
