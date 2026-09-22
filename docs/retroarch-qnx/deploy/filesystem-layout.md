---
title: Filesystem layout - immutable app image + replaceable SD
tags: [deploy, filesystem]
status: verified-source
sources:
  - build.sh (staging section)
  - pkg/ra.sh
  - src/frontend/drivers/platform_qnx.c (frontend_qnx_get_env_settings)
  - pkg/mnt_app.README.txt, pkg/sd/retroarch/RESOURCES.txt
reconciles:
  - README.md "Filesystem layout & install paths"
  - docs/legacy/DEPLOYMENT_LAYOUT.md
  - build/mnt_app/README_INSTALL.txt, build/sd_card/README_INSTALL.txt
---

# Filesystem layout - immutable app image + replaceable SD

Two layers, mirrored 1:1 by `build/mnt_app/` and `build/sd_card/`.

| Mount | What | Runtime r/w | Role |
|---|---|---|---|
| `/mnt/app` | signed appimg | **read-only** (remount only for deployment) | binary, cores, runtime libs, UI assets, seeds, launcher, jar |
| `/fs/sda0` | SD slot 1, partition 0, FAT32 | rw (may come up ro; `ra.sh` remounts) | **all** user/runtime state + games |
| `/fs/sdb0` | SD slot 2 | rw, may be absent | additional games (content rules scan it) |
| `/tmp` -> `/dev/shmem` | tmpfs | rw, volatile | markers, lock, no-card fallback config |
| `/mnt/ota/system/core/` | core dumps (`dumper -d`) | - | crash analysis ([[profiler]]) |

gpSP's original mistake was defaulting saves/config into `/mnt/app`. Nothing here ever writes there.

## `/mnt/app` tree (build/mnt_app)

```text
/mnt/app/root/retroarch/
    retroarch                        frontend (griffin, stripped, 2.33 MB)
    ra.sh                            launcher / supervisor              -> launcher-ra-sh
    retroarch.cfg                    FACTORY config template (never edited at runtime)
    retroarch-core-options.cfg       factory core-option seed
    content-rules.cfg                playlist scanner rules             -> content-discovery
    cores/{gpsp,pcsx_rearmed,mupen64plus_next_gles2}_libretro.so
    lib/libstdc++.so.6  lib/libhiddi.so.1  lib/SOURCE.txt
    assets/{ozone,audi,pkg,xmb/monochrome}/   Ozone UI, Audi fonts/wallpaper, fallback fonts, icons
    autoconfig/qnx/*.cfg             234 factory pad profiles (immutable seed)
    rumble/qnx/*.cfg                 verified HID output-report layouts
    info/*.info                      3 core-info files (seed)
/mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar
```

## `/fs/sda0/retroarch` tree (build/sd_card/retroarch)

```text
config/retroarch.cfg                 WORKING config = primary --config (seeded from factory)
config/retroarch-core-options.cfg    per-card core options (seeded once)
config/remaps/                       controller remaps
config/.qnx-config-version           migration stamp (currently 2)
autoconfig/*.cfg                     user "Save Controller Profile" output (highest priority)
autoconfig/qnx/*.cfg                 factory profile cache, re-seeded when .qnx-factory-version != 3
info/*.info + core_info.cache        writable core metadata
database/rdb/*.rdb                   145 libretro databases
cheats/**/*.cht                      28 301 cheat files
system/                              BIOS (PS1, GBA), Mupen64plus/IPL.n64 optional
ps1/ gba/ n64/ roms/                 game content, scanned recursively (both cards)
saves/ states/                       SRAM + savestates
playlists/ thumbnails/ logs/ screenshots/ downloads/ filters/ wallpapers/ overlays/
```

## How the native side resolves paths

`platform_qnx.c` builds `g_defaults` from two env roots (defaults in parentheses):

- `RA_DATA_DIR` (`/mnt/app/root/retroarch`) -> `cores`, `assets`, `autoconfig`, `database/rdb`,
  `info`, `overlays`;
- `RA_USER_DIR` (`/fs/sda0/retroarch`) -> `cheats`, `config`, `config/remaps`, `downloads`,
  `filters/audio`, `playlists`, `saves`, `screenshots`, `states`, `system`, `wallpapers`, `logs`,
  history;
- `RA_CONFIG_PATH` -> the primary config file; cache dir is `/tmp`.

`retroarch.cfg` additionally pins `libretro_directory`, `assets_directory`, `joypad_autoconfig_dir`
to `/mnt/app/...` and `log_dir`, `rgui_*_directory` to `/fs/sda0/...` ([[configuration]]).

## Factory-reset semantics

- **Existing card**: config, saves, states, playlists stay; only versioned migrations touch config.
- **Blank card**: `ra.sh` creates the tree and copies the factory config, core-info and pad-profile
  seed. Databases/cheats are *not* copied (they ship on the SD image only).
- **No card**: config is rebased into `/tmp/retroarch` (`sed s#/fs/sda0/retroarch#/tmp/retroarch#`),
  so every write stays in RAM.

FAT caveats: case-insensitive, no symlinks, 4 GB/file, may mount read-only. Logs are rotated
([[launcher-ra-sh]]) so the card cannot fill.
