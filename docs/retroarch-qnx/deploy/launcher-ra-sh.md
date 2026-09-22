---
title: ra.sh - launcher and native-process supervisor
tags: [deploy, lifecycle]
status: verified-source
sources:
  - pkg/ra.sh
  - java_patch/java_src/com/luka/retroarch/inject/items/RetroArchHook.java (buildLaunchCommand)
reconciles:
  - README.md "Launcher must, at startup"
  - java_patch/README.md "Device contract"
---

# ra.sh - launcher and native-process supervisor

Installed at `/mnt/app/root/retroarch/ra.sh`. Java runs it as
`/bin/sh /mnt/app/root/retroarch/ra.sh </dev/null >/tmp/ra_bootstrap.log 2>&1` wrapped in
`rm -f <markers>; trap ': > /tmp/retroarch.exited.<gen>' 0; ...; : > /tmp/retroarch.exited.<gen>`
([[session-lifecycle]]). The bootstrap log lives in `/tmp` because the SD may still be read-only.

On this unit `/bin/sh` is a symlink to **`/bin/ksh`**, so the script - and anything you type over
ssh - runs under QNX ksh, not dash/bash. It is written to POSIX-sh level anyway (no arrays, no
`[[ ]]`, no `local`) so the same file also runs on the macOS host for `RA_LAUNCH_VALIDATE_ONLY`.

## Sequence

```mermaid
flowchart TD
    A["mount -uw /fs/sda0; mkdir + write-probe"] -->|ok| B["RA_MEDIA=/fs/sda0/retroarch"]
    A -->|fail| C["RA_MEDIA=/tmp/retroarch (volatile)"]
    B --> D["mkdir full user tree"]
    C --> D
    D --> E["seed config/retroarch.cfg if missing<br/>(no-card: sed-rebase paths to /tmp)"]
    E --> F["migration: .qnx-config-version != 2 -><br/>video_vsync false->true, stamp"]
    F --> G["seed core-options if missing"]
    G --> H["autoconfig/qnx re-seed if .qnx-factory-version != 3"]
    H --> I["seed info/*.info if none"]
    I --> J["export env knobs; LD_LIBRARY_PATH order"]
    J --> K{"RA_LAUNCH_VALIDATE_ONLY=1?"}
    K -->|yes| L["print roots, exit 0"]
    K -->|no| M["rotate logs; keep 8 retroarch__*.log; exec >> ra_run.log"]
    M --> N["kill any old io-hid -d usb (TERM, then KILL)"]
    N --> O["start io-hid -d usb upath=/dev/io-usb/io-usb &"]
    O --> P["retroarch --config $RA_USER_CONFIG \"$@\" &  ; wait"]
    P --> Q["if lock PID == child: rm lock; rm pcm.ready; stop io-hid; exit status"]
```

## Why it is a supervisor, not `exec`

A QNX process whose last thread exited stays in `/proc` as a zombie until its parent calls
`waitpid()`. With `exec`, the asynchronous Java wrapper became that parent and the stock ksh did
not reliably reap signal-driven exits. Keeping the shell as parent means the child is reaped and
`/tmp/retroarch.lock` is cleared only for the exact PID it reaped (never another instance's lock).
The Java side reads the PID from the lock for `kill` ([[signals-and-lock]]); `trap _ra_forward_term
HUP INT TERM` forwards a shell-level TERM to both children.

## io-hid ownership

One `io-hid -d usb` instance per RetroArch session. A persistent instance once deadlocked inside its
USB/mutex graph and even `test -e /dev/io-hid` blocked forever - so the script never stats
`/dev/io-hid`; it finds the old instance via `pidin ar | grep '/armle/sbin/io-[h]id -d usb ...'`,
TERMs, then KILLs it, starts a fresh one, and stops it on exit. If io-hid dies within 0.5 s the
launch aborts with exit 1.

## Environment contract (exported to the frontend)

| Variable | Value | Consumer |
|---|---|---|
| `RA_DATA_DIR` / `RA_USER_DIR` / `RA_CONFIG_PATH` | app root / SD (or /tmp) root / working cfg | `platform_qnx.c` ([[filesystem-layout]]) |
| `RA_CONTENT_RULES` | `$RA_DIR/content-rules.cfg` | playlist scanner ([[content-discovery]]) |
| `LIBRETRO_AUTOCONFIG_DIRECTORY` | `$RA_MEDIA/autoconfig` | pad profiles ([[input-hid-xusb]]) |
| `RA_LOCK_PATH` | `/tmp/retroarch.lock` (**must match Java's constant**) | lock/PID file |
| `RA_QNX_DISPLAYABLE_ID` / `RA_QNX_CONTEXT_ID` / `RA_QNX_DISPLAY_ID` | 43 / 90 / 0 | [[video-context]] |
| `RA_QNX_SCREEN_W` / `RA_QNX_SCREEN_H` | 1024 / 480 | render surface size |
| `RA_QNX_AUDIO_DEV` | `/dev/snd/mpl1_int_ent` | [[audio-qsa]] |
| `RA_QNX_AUDIO_READY_PATH` | `/tmp/retroarch.pcm.ready` | PCM-ready handshake |
| `RA_QNX_AUTO_SAVE_STATE` | 0 (savestate on lifecycle pause disabled) | [[signals-and-lock]] |
| `RA_QNX_AUDIO_PERF_LOG` | `/tmp/qsa_perf.log` | QSA telemetry |
| `RA_QNX_HID_DEBUG=1`, `RA_QNX_HID_DUMP=1` | bounded HID diagnostics | [[input-hid-xusb]] |
| `GRAPHICS_ROOT=/proc/boot/` | **the EGL fix**: `libOSUser` reads `$GRAPHICS_ROOT/graphics.conf` for the eglsub list; unset -> `egl14.so` SIGSEGV in `OpenSubDriver` | EGL init |
| `IPL_CONFIG_DIR` | `/etc/eso/production` if unset | dmdt/display manager |
| `LD_LIBRARY_PATH` | `$RA_DIR/lib:/mnt/app/eso/lib:/mnt/app/armle/lib:...:/proc/boot` | Adreno EGL/GLES from `/mnt/app/eso/lib` must win over `/proc/boot` (those fault in `eglGetDisplay`) |

Not exported but honoured if set: `RA_QNX_SCREEN_BUFFERS` (2..4, default 3),
`RA_QNX_AUDIO_CHANNEL_MAP` (e.g. `l,r,0,0,l,r`).

## Logs it manages

| File | Limit | Producer |
|---|---|---|
| `$RA_MEDIA/logs/ra_run.log` (`/tmp/ra_run.log` without SD) | 512 KiB, `.1` rotation | this script's stdout/stderr + RetroArch stdio |
| `$RA_MEDIA/logs/ra_hook.log`, `ra_audio.log` | 256 KiB | Java hook / audio bridge |
| `/tmp/ra_bootstrap.log` | 256 KiB | Java's exec redirection (pre-SD) |
| `$RA_MEDIA/logs/retroarch__*.log` | newest 8 | RetroArch `log_to_file` |
| `/tmp/ra_display.log` | unbounded (append) | `qnx_ctx.c` raw tracer |
| `/tmp/qsa_perf.log` | truncated per session | QSA telemetry |

`ra.sh` avoids `wc`, `find` and `tail`: they live in `/armle/usr/bin`, which is on the HMI's PATH
but not on the ssh login PATH, and the script must behave identically from both; rotation uses
`ls -l` and shell globs instead. When running
`ra.sh` by hand over ssh, `export PATH=/armle/usr/bin:/armle/bin:$PATH` first or `date` fails.

## Host validation

`RA_LAUNCH_VALIDATE_ONLY=1 RA_APP_DIR=... RA_SD_MOUNT=... sh pkg/ra.sh` exercises media detection
and seeding on a host without touching HID/display.
