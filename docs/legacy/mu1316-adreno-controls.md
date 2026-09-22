# MU1316 Adreno GLES2 controls

Firmware: `MHI2Q_US_AUG22_P5087_MU1316`

Analyzed image:
`extracted/ifs2/ifs_display/proc/boot/OpenGLES20.so`

- SHA-256: `cc89187b21c921f109e7802ac805879a52002883c0d001355aafc44fd93bbc4d`
- ELF32 ARM EABI5, stripped, base address `0`
- GLES build reported by the live unit: `OpenGL ES 2.0 3929146`

## Findings

The driver exposes `GL_QCOM_driver_control`; these are GL extension controls,
not environment variables or binary patches. Relevant exported entry points:

- `glDisableDriverControlQCOM`: `0x2aa54`
- `glEnableDriverControlQCOM`: `0x2aa78`
- `glGetDriverControlStringQCOM`: `0x2aa9c`
- `glGetDriverControlsQCOM`: `0x2aad8`

The embedded control descriptions are:

- ID 0, `INFINITE_FAST_DRIVER` at `0xc4f08`: return after minimal driver work. It is
  diagnostic only and does not create real GL objects.
- ID 1, `INFINITE_FAST_HARDWARE` at `0xc5040`: perform driver validation and command
  generation but do not submit command buffers to the GPU. Diagnostic only.
- ID 2, `POWERFLAGS_OVERRIDE` at `0xc5198`: "Override GPU power management clocks to
  high." This is the only discovered control intended to improve real output.
- ID 3, `AUTO_TEX_COMPRESSION` at `0xc51da`: automatically compress new textures;
  it changes texture representation/quality and is not enabled by the bench.

The IDs above were enumerated through the extension on the live Adreno 320, not
inferred from string order.

The extension string at `0xc6150` also advertises
`GL_QCOM_binning_control`, `GL_QCOM_tiled_rendering`,
`GL_QCOM_writeonly_rendering`, and `GL_EXT_discard_framebuffer`. The standalone
bench can now test power override, binning modes, write-only rendering, FBO
formats, and discard without modifying the firmware.

The library imports `getenv` at `0x143b8`, but the string/xref sweep found no
hidden Adreno performance/clock environment-variable name. `GRAPHICS_ROOT` at
`0xc08a2` is the loader-root setting already used by RetroArch and the bench;
`QC_GFX_CONF_DIR` at `0xc3120` selects the API trace/log output directory.

## Safety boundary

No firmware binary was modified. The two infinite-fast controls suppress real
rendering and are useful only for profiling fresh one-shot contexts. Four
native Screen buffers are excluded: the live test created four buffers, then
failed `eglCreateWindowSurface` with `EGL_BAD_ALLOC (0x3003)`; immediately
afterward, the unit temporarily stopped answering over SSH. Two and three
buffers remain the supported matrix.
