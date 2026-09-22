---
title: Factory configuration - retroarch.cfg, core options, migrations
tags: [deploy, config]
status: verified-source
sources:
  - pkg/retroarch.cfg
  - pkg/retroarch-core-options.cfg
  - pkg/ra.sh (migrations)
reconciles:
  - pkg/sd/retroarch/config/README.txt
---

# Factory configuration - retroarch.cfg, core options, migrations

`pkg/retroarch.cfg` is copied verbatim to `build/sd_card/retroarch/config/retroarch.cfg` and to
`/mnt/app/root/retroarch/retroarch.cfg` (template). RetroArch always runs with the **SD copy** as
its primary `--config`, so "Save Configuration" and save-on-exit can never hit `/mnt/app`.

## Non-default keys that define the port

| Key | Value | Why |
|---|---|---|
| `video_driver` | `gl` | GLES2 via `qnx` context ([[video-context]]) |
| `video_fullscreen`, `_x`, `_y` | true, 1024, 480 | fixed Ozone surface |
| `video_vsync` | **true** | bounded Screen vsync / 60 Hz deadline; see [[frame-and-audio-pacing]] for the open question |
| `video_refresh_rate` | 60.000000 | panel is assumed 60 Hz; never measured |
| `audio_driver` | `qsa` | [[audio-qsa]] |
| `audio_latency` | 128 | ms |
| `audio_rate_control_delta` | 0.005 | +-0.5 % DRC: corrects drift, not a slow core |
| `audio_sync` | true | blocking writes pace the core when vsync does not |
| `input_joypad_driver` | `qnx` | HIDDI/XUSB ([[input-hid-xusb]]) |
| `input_autodetect_enable` | true | profiles from `autoconfig/` |
| `menu_driver` | `ozone`, theme 15, sidebar shown+collapsed | Audi look via `assets/audi` |
| `menu_show_*` | load_core/load_content/online_updater/core_updater/configurations/help/information = false | appliance UI; content comes from playlists ([[content-discovery]]) |
| `menu_swap_ok_cancel_buttons_auto` | true | OK/Cancel from the pad's A/B vs Cross/Circle geometry |
| `savestate_auto_load` / `savestate_auto_save` | true / false | resume a state if one exists; never auto-write on exit |
| `log_to_file`, `log_to_file_timestamp` | true | `logs/retroarch__<ts>.log` (8 kept) |
| `libretro_directory`, `assets_directory`, `joypad_autoconfig_dir` | `/mnt/app/root/retroarch/...` | immutable inputs |
| `log_dir`, `rgui_browser_directory`, `rgui_config_directory` | `/fs/sda0/retroarch/...` | writable outputs |

## Core options (`retroarch-core-options.cfg`, seeded once per card)

```ini
gpsp_bios = "auto"            gpsp_drc = "disabled"        gpsp_frameskip = "disabled"
pcsx_rearmed_bios = "auto"    pcsx_rearmed_drc = "enabled" pcsx_rearmed_spu_thread = "enabled"
pcsx_rearmed_cd_readahead = "256"   pcsx_rearmed_frameskip_type = "disabled"
pcsx_rearmed_rgb32_output = "disabled"
pcsx_rearmed_noxadecoding = "enabled"   pcsx_rearmed_nocdaudio = "enabled"   ; inverted keys: enabled = keep XA / CD-DA
mupen64plus-cpucore = "dynamic_recompiler"  mupen64plus-rdp-plugin = "gliden64"
mupen64plus-rsp-plugin = "hle"  mupen64plus-aspect = "4:3"  mupen64plus-43screensize = "640x480"
```

Rationale per core: [[cores-overview]].

## Versioned migrations (`ra.sh`)

Existing cards are never overwritten wholesale. Each migration is a `sed` of one exact factory
value, committed with a stamp file only after success:

| Stamp | Version | Effect |
|---|---|---|
| `config/.qnx-config-version` | 2 | `video_vsync = "false"` -> `"true"` |
| `autoconfig/.qnx-factory-version` | 3 | replace `autoconfig/qnx/` with the current factory profile set (user files at the autoconfig root are kept) |

A previous version-2 migration also flipped `ppsspp_performance_stats`; it was removed with PPSSPP.

## Things deliberately absent

7zip (`HAVE_7ZIP` off), online updaters, core/content loading menus, `fps_show` overlays (they cost
frames on this GPU - see [[ppsspp-status]]).
