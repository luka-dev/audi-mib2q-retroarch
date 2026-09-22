---
title: MU1316 EGL swap path and buffer count
tags: [re, firmware, display]
status: verified-decompile
sources:
  - docs/legacy/mu1316-egl-swap.md
  - firmware libdisplayinit.so, libEGL.so.1, egl14.so, eglsub-screen.so, libscreen.so.1
reconciles:
  - docs/legacy/mu1316-egl-swap.md
---

# MU1316 EGL swap path and buffer count

All stripped ARMv7 ELF32, base 0 (VA = file offset).

| Binary | SHA-256 (prefix) | Finding |
|---|---|---|
| `libdisplayinit.so` | `5be545ea` | `display_create_window` @0x11bc is a compatibility wrapper: inserts constant buffer count **2** (0x11cc-0x11d0) and tail-calls `display_create_window_nbuffers` @0x0e00 with signature `int(EGLDisplay, EGLConfig, int w, int h, int displayable, int buffers, EGLNativeWindowType*, int*)`; the count reaches `screen_create_window_buffers(window, count)` @0x1114 unclamped |
| `libEGL.so.1` | `a7b3ad6b` | public `eglSwapBuffers` @0x5e48 dispatches to Adreno |
| `egl14.so` | `2f44c706` | `qeglDrvAPI_eglSwapBuffers` @0xc6d8 |
| `eglsub-screen.so` | `204df495` | updater posts with `screen_post_window` @0x9a58 (retry 0x9ac4) and **waits on condvars** @0x9b1c/0x9bb4 when no render buffer is free - the measured 15-17 ms steady-state swap wait with two buffers |
| `libscreen.so.1` | `4fed3e9a` | `screen_create_window_buffers` @0x13b30, `screen_destroy_window_buffers` @0x136b0, `screen_post_window` @0x143bc |

Applied in the frontend: prefer `display_create_window_nbuffers` with **3** buffers before
`eglCreateSurface`; keep the legacy call plus a transactional destroy/create/restore fallback; set
the native Screen swap interval to 0 before surface creation and keep it in sync with
`eglSwapInterval(0)` ([[video-context]]). Firmware untouched.
