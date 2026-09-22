---
title: Install / update on the head unit
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

# Install / update on the head unit

Access: ssh as `root` to the unit (`10.173.189.1`), legacy `ssh-rsa` host key. The repo's parent
directory has `audi_ssh.sh` (`shell` / `exec` / `put` / `get`) which wraps the options and password;
never commit the password into this vault.

The unit has no `scp` server-side helpers in some firmware states, no `tail`/`wc`/`cksum`/`date` in
the login PATH, and its `grep` lacks `-o`/`-A`. Two reliable transfer idioms:

```sh
# push one file
ssh root@HU 'cat > /mnt/app/root/retroarch/retroarch' < build/mnt_app/root/retroarch/retroarch
# or scp -O (legacy protocol) via audi_ssh.sh put
../../audi_ssh.sh put build/mnt_app/root/retroarch/retroarch /mnt/app/root/retroarch/retroarch
# verify: read back and cksum on the host
ssh root@HU 'cat /mnt/app/root/retroarch/retroarch' | cksum ; cksum build/mnt_app/root/retroarch/retroarch
```

## A. App image (`/mnt/app`)

1. `mount -uw /mnt/app` (survives reboot, but leave it ro after deployment).
2. Copy the **contents** of `build/mnt_app/` to `/mnt/app/` preserving paths:
   - `root/retroarch/` (binary, `ra.sh`, cfg templates, cores, lib, assets, autoconfig, rumble, info);
   - `eso/hmi/lsd/jars/ra_mhi2q.jar`.
3. `chmod 755 /mnt/app/root/retroarch/retroarch /mnt/app/root/retroarch/ra.sh`.
4. `sync`, then `mount -ur /mnt/app` (optional but intended).
5. **Reboot** whenever the jar changed: `lsd.jxe` loads jars at boot; the runtime injection
   happens on the first *Games* press ([[games-menu-injection]]). A native-only update (binary,
   cores, `ra.sh`, cfg) needs no reboot - just do not update while RetroArch runs
   (`slay -f -Q retroarch` first).

## B. SD card

Copy the **contents** of `build/sd_card/` to the root of a FAT32 card (tested: 32 GB). It carries
games, BIOS, RDBs, cheats, box art and a factory config. Insert in slot 1 (`/fs/sda0`). Swapping
cards takes effect at the next launch. Do not remove a card while a game writes SRAM/states.

A blank card also works: `ra.sh` seeds config/info/profiles from `/mnt/app` on first launch
([[filesystem-layout]]); databases and cheats will simply be absent.

## C. First-run verification

1. Main menu shows **Games**. Press it. `/fs/sda0/retroarch/logs/ra_hook.log` must contain, in order:
   ```
   [SMM] found live SystemSMM
   [SMM] preloaded RaScreen ID 250 into OEM ScreenCache
   [SMM] refreshed live SMI stack
   [SMM] runtime SMM install PASS: state=631 enterTrans=890 exitTrans=891 screen=250
   fired main SystemSMM EV_ENTER=9990001
   RA state connected: acquiring OEM audio, saved context <N>, native route=90{16,43}, clear=transparent
   OEM route ready: launched native RetroArch
   ```
2. `ra_audio.log` reaches `ACTIVE focus=2 connection=20 route=1->1 PCM=ready` ([[audio-session]]).
3. `/tmp/ra_display.log` ends with `=== EGL context up: 1024x480 displayable=43 -- SUCCESS ===` and
   `routed display 0 -> context 90 {16,43}` ([[video-context]]).
4. Ozone renders full-screen; the volume popup still draws above it; BACK returns to the main
   wizard and restores the previous context; radio/media audio returns.
5. Plug a pad: `RA_QNX_HID_DEBUG` topology lines appear in the newest `retroarch__*.log`.

## D. Collecting diagnostics after a session

`ra_run.log`, `ra_hook.log`, `ra_audio.log`, newest `retroarch__*.log` (all under
`/fs/sda0/retroarch/logs/`), `/tmp/ra_display.log`, `/tmp/qsa_perf.log`, and for crashes
`/mnt/ota/system/core/*.core.gz` ([[profiler]]). `tools/qnx-bench/run-session.sh` automates a
timed launch + log harvest + optional profiler capture ([[gles2-benchmark]]).

## E. Removing

Delete `/mnt/app/root/retroarch/` and `/mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar`, reboot. Nothing
else in the appimg is modified by this project; the stock display-context table, SystemSMM tables
and audio bundle are untouched on disk (all changes are in-memory at runtime).
