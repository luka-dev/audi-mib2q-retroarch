# Vendored source provenance

The source trees below are tracked directly by this repository. They are not
Git submodules or gitlinks. This keeps the complete MHI2Q build reproducible
from a single checkout, including all local QNX changes.

`VENDORED_SOURCES.env` contains the same snapshot IDs in machine-readable form
and is the source of embedded version strings during QNX and macOS builds.

Imported: 2026-08-09

| Local path | Upstream repository | Upstream/base commit | Imported snapshot | Local changes |
| --- | --- | --- | --- | --- |
| `src/` | https://github.com/libretro/RetroArch.git | `abb72220ebc1f58323916608f0950cbea640430f` | `67189d12fe9ccd495296f71e009f961e7caddde4` | MHI2Q QNX frontend, display, QSA/DSI audio, HID/XUSB/GIP input, lifecycle and Audi Ozone UI |
| `cores-src/gpsp/` | https://github.com/saulfabregwiivc/gpSP | `587f9f3e7809e7a9a38238f00b6ac008b904805b` | `8b5812e5e63b7841635036cd8b79819eaa357a2b` | QNX ARM dynarec, audio/video/input and build integration |
| `cores-src/pcsx_rearmed/` | https://github.com/libretro/pcsx_rearmed.git | `da2cb8ecd17fd0932ab6d94774c0522beebce6e3` | `8fc35f30fec05fe3fe664761811743c4bb5cd45d` | QNX ARM/NEON dynarec and MHI2Q libretro build |
| `cores-src/pcsx_rearmed/frontend/libpicofe/` | https://github.com/notaz/libpicofe.git | `dd11f2d723162eb1cf8e6db9f40de7db0d0b6bba` | same | Vendored unchanged from the PCSX-ReARMed submodule |
| `cores-src/mupen64plus_next/` | https://github.com/libretro/mupen64plus-libretro-nx.git | `3a676196500545b637b83cb19fb393d2359e1f9d` (`master`) | base commit plus the QNX changes in this root repository | Known-good GLideN64 rendering base, QNX ARM dynarec/GLES2 port, GCC 4.9 compatibility and reduced static memory use |

The optional upstream `mupen64plus-rsp-paraLLEl/lightning/gnulib` submodule was
not initialized and is not required by the QNX GLES2/HLE build. No gitlink is
recorded for it in this repository.

The former nested Git metadata is not part of the source snapshot. During the
2026-08-09 conversion it was moved, for local recovery only, to
`.git/nested-repos-backup-20260809/`.
