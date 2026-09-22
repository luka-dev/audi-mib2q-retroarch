---
title: Vendored sources - provenance
tags: [build, reference]
status: verified-source
sources:
  - VENDORED_SOURCES.md
  - VENDORED_SOURCES.env
  - cores-src/ppsspp/VENDORED_SUBMODULES.txt
reconciles:
  - VENDORED_SOURCES.md
---

# Vendored sources - provenance

No submodules or gitlinks: every source tree is tracked directly so one checkout reproduces the
image. Imported 2026-08-15. `VENDORED_SOURCES.env` carries the same ids for `build.sh`.

| Local path | Upstream | Base commit | Local changes |
|---|---|---|---|
| `src/` | libretro/RetroArch | `abb72220` (snapshot `67189d12`) | QNX frontend, display, QSA audio, HID/XUSB/GIP input, lifecycle, Audi Ozone UI, content discovery task |
| `cores-src/gpsp/` | saulfabregwiivc/gpSP | `587f9f3e` (snapshot `8b5812e5`) | QNX ARM dynarec + `msync`, build integration |
| `cores-src/pcsx_rearmed/` | libretro/pcsx_rearmed | `da2cb8ec` (snapshot `8fc35f30`) | QNX ARM/NEON dynarec, GTE `r0` fix ([[qnx-sync-cost]]) |
| `cores-src/pcsx_rearmed/frontend/libpicofe/` | notaz/libpicofe | `dd11f2d7` | unchanged |
| `cores-src/mupen64plus_next/` | libretro/mupen64plus-libretro-nx | `f275caf4` (master 2026-08-06) | QNX GLES2 port, GCC 8.5/gas compat, waiter-gated command queue, exact vertex span, reduced BSS |
| `cores-src/ppsspp/` | hrydgard/ppsspp | `fa50bb19` (v1.20.4) | QNX 6.5/GCC 8.5 port, JIT I-cache, GLES2-only, Krait CPU detect, telemetry - **not shipped** ([[ppsspp-status]]) |
| `cores-src/ppsspp/ffmpeg/` | hrydgard/ppsspp-ffmpeg | `1e3b4965` | `blackberry/armv7` static libs rebuilt for the QNX 6.5 linker |

Not vendored: `mupen64plus-rsp-paraLLEl/lightning/gnulib` (HLE RSP only), PPSSPP's prebuilt FFmpeg
for other hosts and its PSP test corpus. Nested `.git` metadata was moved to
`.git/nested-repos-backup-20260809/` (local only).

Other external data with its own `SOURCE.txt`: `pkg/info` (libretro-core-info `f105af29`, 3 files
shipped), `pkg/database/rdb` (145 RDBs), `pkg/cheats` (28 301 `.cht`, libretro-database `6fd53f98`),
`pkg/autoconfig/qnx` (234 profiles retargeted from DInput/HID), `pkg/rumble/qnx`,
`pkg/runtime-libs` ([[toolchain]]).
