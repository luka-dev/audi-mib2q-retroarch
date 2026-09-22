---
title: MU1316 Adreno GLES2 driver controls and extensions
tags: [re, firmware, gpu]
status: verified-decompile
sources:
  - docs/legacy/mu1316-adreno-controls.md
  - firmware OpenGLES20.so (SHA-256 cc89187b...), live enumeration on the Adreno 320
reconciles:
  - docs/legacy/mu1316-adreno-controls.md
---

# MU1316 Adreno GLES2 driver controls and extensions

`GL_QCOM_driver_control` entry points: `glGetDriverControlsQCOM` 0x2aad8,
`glGetDriverControlStringQCOM` 0x2aa9c, `glEnableDriverControlQCOM` 0x2aa78,
`glDisableDriverControlQCOM` 0x2aa54. IDs enumerated live, not inferred from string order.

| ID | Name | Description (embedded) | Use |
|---|---|---|---|
| 0 | `INFINITE_FAST_DRIVER` (0xc4f08) | return after minimal driver work; no real GL objects | diagnostic split only |
| 1 | `INFINITE_FAST_HARDWARE` (0xc5040) | validate + build commands, never submit | diagnostic split only |
| 2 | `POWERFLAGS_OVERRIDE` (0xc5198) | "Override GPU power management clocks to high" | measured: noise-sized |
| 3 | `AUTO_TEX_COMPRESSION` (0xc51da) | compress new textures automatically | changes quality; no gain |

Extension string (0xc6150) also advertises `GL_QCOM_binning_control`, `GL_QCOM_tiled_rendering`,
`GL_QCOM_writeonly_rendering`, `GL_EXT_discard_framebuffer` - all testable from
[[gles2-benchmark]] without touching firmware.

Environment: the library imports `getenv` (0x143b8) but has no hidden clock/perf variable.
`GRAPHICS_ROOT` (0xc08a2) is the loader root RetroArch already sets; `QC_GFX_CONF_DIR` (0xc3120)
selects the API-trace/log directory and the Adreno config file ([[adreno-driver-hotpath]]).

Safety: four native Screen buffers are excluded (`EGL_BAD_ALLOC` then a temporarily unresponsive
unit); 2 and 3 are the supported matrix. Infinite-fast controls suppress real output and are only
useful with fresh one-shot contexts.
