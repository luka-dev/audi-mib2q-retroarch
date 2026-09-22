# MU1316 EGL swap path

Firmware: `MHI2Q_US_AUG22_P5087_MU1316`

## Binary fingerprints

- `libEGL.so.1`: SHA-256 `a7b3ad6b59dcb700c4588df2e0d7fc26a5d76492645a38d068c71efd94f9d067`
- `egl14.so`: SHA-256 `2f44c70685693388ba60335d410b502bd7af2c8f94472edc3dc1faac7cd3574e`
- `eglsub-screen.so`: SHA-256 `204df49536c44dd870177ff27b98c80e89786b42f256192e0e6fd0c6c4c86841`
- `libscreen.so.1`: SHA-256 `4fed3e9abea37b36b79492684dfcc1804f7e652f8f946524b576ccc0709f63d0`
- `libdisplayinit.so`: SHA-256 `5be545eafc0775358085e5dec7062cc6bd92e0becdc0e4f2a00810e2d0c07eef`

All are stripped ARMv7 little-endian ELF32 shared objects with base address 0;
the virtual addresses below are also file offsets.

## Findings

- `libdisplayinit.so:display_create_window` at `0x11bc` is a compatibility
  wrapper. It inserts the constant buffer count `2` at `0x11cc`-`0x11d0` and
  delegates to `display_create_window_nbuffers` at `0x0e00`.
- Recovered `display_create_window_nbuffers` signature:
  `int(EGLDisplay, EGLConfig, int width, int height, int displayable,
  int buffers, EGLNativeWindowType *, int *)`.
- The requested count reaches `screen_create_window_buffers(window, count)` at
  `libdisplayinit.so:0x1114` without clamping it to two.
- Public `libEGL.so.1:eglSwapBuffers` at `0x5e48` dispatches to the Adreno
  implementation. `egl14.so:qeglDrvAPI_eglSwapBuffers` is at `0xc6d8`.
- The Screen subdriver has a dedicated updater path. In `eglsub-screen.so`, the
  updater posts with `screen_post_window` at `0x9a58` (retry `0x9ac4`) and waits
  on condition variables at `0x9b1c` and `0x9bb4` when no render buffer is
  available. This matches the measured 15-17 ms steady-state swap wait with a
  two-buffer window.
- `libscreen.so.1` exports `screen_create_window_buffers` at `0x13b30`,
  `screen_destroy_window_buffers` at `0x136b0`, and `screen_post_window` at
  `0x143bc`.

## Applied frontend fix

RetroArch now prefers the firmware's `display_create_window_nbuffers` and asks
for three buffers before `eglCreateSurface`. It retains the legacy API plus a
transactional destroy/create/restore fallback for other firmware revisions.
The native Screen swap interval is set to zero before surface creation and is
kept synchronized with `eglSwapInterval(0)` later.

The firmware binaries themselves were not modified.
