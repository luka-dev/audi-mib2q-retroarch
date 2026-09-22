---
title: Install guide - from a built tree to a running game
tags: [deploy, howto]
status: verified-trace
sources:
  - pkg/mnt_app.README.txt, pkg/sd/retroarch/RESOURCES.txt
  - ../../audi_ssh.sh (external), tools/qnx-bench/run-session.sh
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §5 (transfer recipe)
reconciles:
  - build/mnt_app/README_INSTALL.txt
  - build/sd_card/README_INSTALL.txt
---

# Install guide - from a built tree to a running game

Time: ~20 minutes the first time, ~3 minutes for an update. You need a laptop on the same
network as the head unit (the unit answers on `10.173.189.1`), a FAT32 SD card, and the two
trees `build/mnt_app/` and `build/sd_card/` from `./build.sh` ([[build-pipeline]]).

## Before you start

- [ ] The unit is an MHI2Q with firmware `MHI2Q_US_AUG22_P5087_MU1316`. On another firmware the
      Games entry will appear but refuse to install itself (safe, but useless).
- [ ] You can log in: `../../audi_ssh.sh shell` (root, password in that script - never commit it
      anywhere else). Plain ssh needs `-oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa`.
- [ ] Ignition on, HMI fully booted, no game running (`slay -f -Q retroarch` if unsure).
- [ ] Games, BIOS and box art are already inside `build/sd_card/retroarch/` (they are preserved
      across rebuilds; `./fetch-thumbnails.sh <games dir>` fetches box art).

## Step 1 - SD card (2 min)

1. Format a card as **FAT32** (32 GB tested).
2. Copy the **contents** of `build/sd_card/` to the card root, so the card has `retroarch/` at
   top level with `config/`, `ps1/`, `gba/`, `n64/`, `system/`, `database/`, `cheats/`, ...
3. Eject cleanly, insert into **slot 1** of the unit (it mounts as `/fs/sda0`).

A blank card also works - `ra.sh` seeds config, core info and controller profiles on first launch -
but databases, cheats and box art only come from the image ([[filesystem-layout]]).

## Step 2 - App image (5 min)

The ssh login PATH on the unit lacks `/armle/usr/bin`, where `tar`, `cksum`, `scp`, `date`,
`tail`, `wc` live - so every remote command below prepends it.

```sh
HU=root@10.173.189.1
SSH="ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa $HU export PATH=/armle/usr/bin:/armle/bin:\$PATH;"

# 1. make the app image writable for the copy
$SSH 'mount -uw /mnt/app && mkdir -p /mnt/app/root/retroarch /mnt/app/eso/hmi/lsd/jars'

# 2. push the tree (tar over ssh keeps paths and is one command)
tar -C build/mnt_app -cf - . | $SSH 'tar -C /mnt/app -xf -'

# 3. permissions, flush, back to read-only
$SSH 'chmod 755 /mnt/app/root/retroarch/retroarch /mnt/app/root/retroarch/ra.sh; sync; mount -ur /mnt/app'
```

Verify the binary arrived intact:

```sh
$SSH 'cksum /mnt/app/root/retroarch/retroarch'
cksum build/mnt_app/root/retroarch/retroarch      # the two numbers must match
```

Single files can also go through `$SSH 'cat > /mnt/app/<path>' < build/mnt_app/<path>` or
`../../audi_ssh.sh put <local> <remote>` (`scp -O`).

## Step 3 - Reboot

Needed whenever `ra_mhi2q.jar` changed (the HMI loads jars at boot). For a native-only update
(binary, cores, `ra.sh`, `retroarch.cfg`) skip the reboot - just make sure no game was running.

## Step 4 - First launch (3 min)

1. Main menu -> **Games** appears as a new row. Select it.
2. Within ~10 s the screen goes to the RetroArch/Ozone menu, full-screen, with sound. The stock
   volume popup still draws on top when you turn the knob.
3. Plug in a pad, pick a game from a playlist, play. **BACK** or **MENU** returns to the car menu
   and restores whatever audio source was active before.

If something is off, check the logs on the card (also readable over ssh):

```sh
$SSH 'cat /fs/sda0/retroarch/logs/ra_hook.log'   # HMI side: install + lifecycle
$SSH 'cat /fs/sda0/retroarch/logs/ra_audio.log'  # OEM audio session
$SSH 'cat /tmp/ra_display.log'                   # EGL / display routing
$SSH 'ls /fs/sda0/retroarch/logs/'               # retroarch__<timestamp>.log = frontend
```

A healthy first entry contains, in this order:

```
[SMM] runtime SMM install PASS: state=631 enterTrans=890 exitTrans=891 screen=250   (ra_hook.log)
RA state connected: acquiring OEM audio, saved context 25, native route=90{16,43}, clear=transparent
OEM route ready: launched native RetroArch
=== EGL context up: 1024x480 displayable=43 -- SUCCESS ===                          (ra_display.log)
routed display 0 -> context 90 {16,43}
ACTIVE focus=2 connection=20 route=1->1 PCM=ready                                   (ra_audio.log)
```

| Symptom | What to look at |
|---|---|
| no Games row | jar not in `/mnt/app/eso/hmi/lsd/jars/`, or not rebooted |
| Games does nothing | `ra_hook.log`: `runtime SMM install FAILED` = wrong firmware fingerprint |
| back to the menu after ~10 s | `ra_audio.log` last lines: `activation aborted` / `QSA PCM handshake timeout` -> [[audio-session]] |
| `refusing relaunch: prior native process missed exit timeout` | `slay -f -Q retroarch`, try again |
| black screen, `ra_display.log` ends at `FAIL egl_init_context` | started outside `ra.sh` (no `GRAPHICS_ROOT`) |
| no pad | HID topology lines in the newest `retroarch__*.log`; `hidview` on the unit |

## Updating later

- **SD only** (games, config, profiles): swap or re-copy the card; takes effect at the next launch.
  Existing config/saves/playlists on a card are never overwritten by an app update; only the
  versioned migrations in `ra.sh` touch config ([[configuration]]).
- **Native only**: Step 2 without the jar, no reboot.
- **Jar**: Step 2 + reboot.

## Removing

```sh
$SSH 'mount -uw /mnt/app; rm -rf /mnt/app/root/retroarch /mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar; sync; mount -ur /mnt/app'
```

then reboot. Nothing else in the firmware was changed: the state-machine tables, display contexts
and audio bundle are only patched in memory while the HMI runs.

## Collecting diagnostics for a bug report

`ra_run.log`, `ra_hook.log`, `ra_audio.log`, the newest `retroarch__*.log` (all in
`/fs/sda0/retroarch/logs/`), `/tmp/ra_display.log`, `/tmp/qsa_perf.log`, and for a crash the
newest `/mnt/ota/system/core/*.core.gz` ([[profiler]]). `tools/qnx-bench/run-session.sh`
automates a timed launch plus log harvest ([[gles2-benchmark]]).
