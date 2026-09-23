---
title: Install guide
tags: [deploy, howto]
status: verified-trace
sources:
  - pkg/mnt_app.README.txt, pkg/sd/retroarch/RESOURCES.txt
  - firmware: system/etc/inetd.conf, app/armle/usr/sbin/sshd, ifs2/ifs_coreservices3/usr/sbin/{telnetd,ftpd}
  - ../../audi_ssh.sh (external), tools/qnx-bench/run-session.sh
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §5 (transfer recipe)
reconciles:
  - build/mnt_app/README_INSTALL.txt
  - build/sd_card/README_INSTALL.txt
---

# Install guide

_From a built tree to a running game on the head unit — about 20 minutes the first time, 3 for an update._

---

> ⚠️ **Experimental, manual, and unsigned.** There is no installer, no `.tar.gz` update package
> and no OTA path. You copy files onto the firmware partition from a root shell. A wrong copy into
> `/mnt/app` can leave the HMI unable to start; know how you would recover the unit before you
> begin.

## 📋 Before you start

- [ ] **Shell access as root.** SSH is what the rest of this guide uses. The firmware also ships
      `telnetd` (enabled in `/etc/inetd.conf`) and `ftpd` — either works, the only requirement is
      that you can write to `/mnt/app`. Getting that access is out of scope here.
- [ ] **Network.** The unit answers on `10.173.189.1`. Its ssh host key is legacy RSA, so a modern
      client needs `-oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa`. The repo's
      parent directory has `audi_ssh.sh` (`shell` / `exec` / `put` / `get`) which wraps that.
- [ ] **The right firmware.** `MHI2Q_US_AUG22_P5087_MU1316`. On anything else the HMI hook fails
      closed and *Games* does nothing ([[known-issues]]).
- [ ] **A built tree.** `./build.sh` has produced `build/mnt_app/` and `build/sd_card/`
      ([[build-pipeline]]).
- [ ] **Ignition on**, HMI fully booted, no game running (`slay -f -Q retroarch` if unsure).
- [ ] **Games and BIOS** already copied into `build/sd_card/retroarch/` — they survive rebuilds.
      `./fetch-thumbnails.sh <games dir>` fetches box art into the same tree.

```mermaid
flowchart LR
    accTitle: Install Steps Overview
    accDescr: The SD card carries user content and state while the app image carries the binary and HMI jar; only a jar change requires a reboot.

    step1["💾 1. SD card<br/>content + state"]
    step2["📦 2. App image<br/>binary, cores, jar"]
    step3["🔄 3. Reboot<br/>(jar changes only)"]
    step4(["🎮 4. First launch"])

    step1 --> step2 --> step3 --> step4

    classDef primary fill:#dbeafe,stroke:#2563eb,stroke-width:2px,color:#1e3a5f
    classDef success fill:#dcfce7,stroke:#16a34a,stroke-width:2px,color:#14532d

    class step1,step2,step3 primary
    class step4 success
```

## 💾 Step 1 — SD card

1. Format a card as **FAT32** (32 GB tested).
2. Copy the **contents** of `build/sd_card/` to the card root, so the card has `retroarch/` at top
   level with `config/`, `ps1/`, `gba/`, `n64/`, `system/`, `database/`, `cheats/`, …
3. Eject cleanly and insert it into **slot 1** — it mounts as `/fs/sda0`.

A blank card also works: `ra.sh` seeds config, core info and controller profiles on first launch.
Only databases, cheats and box art come exclusively from the image ([[filesystem-layout]]).

## 📦 Step 2 — App image

Everything you type on the unit runs in **ksh** (`/bin/sh` is a symlink to `/bin/ksh`), and the ssh
login PATH lacks `/armle/usr/bin`, where `tar`, `scp`, `date`, `tail` and `wc` live — so every
remote command below prepends it.

```bash
HU=root@10.173.189.1
SSH="ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa $HU export PATH=/armle/usr/bin:/armle/bin:\$PATH;"

# 1. make the app image writable for the copy
$SSH 'mount -uw /mnt/app && mkdir -p /mnt/app/root/retroarch /mnt/app/eso/hmi/lsd/jars'

# 2. push the tree (tar over ssh preserves paths in one command)
tar -C build/mnt_app -cf - . | $SSH 'tar -C /mnt/app -xf -'

# 3. permissions, flush, back to read-only
$SSH 'chmod 755 /mnt/app/root/retroarch/retroarch /mnt/app/root/retroarch/ra.sh; sync; mount -ur /mnt/app'

# 4. the sizes must match build/mnt_app
$SSH 'ls -l /mnt/app/root/retroarch/retroarch /mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar'
```

Single files can also go through `$SSH 'cat > /mnt/app/<path>' < build/mnt_app/<path>` or
`../../audi_ssh.sh put <local> <remote>` (`scp -O`). Over FTP, upload into the same paths after
remounting `/mnt/app` writable.

Two paths are installed, and they are the only two this project ever touches:

| Path | What |
| ---- | ---- |
| `/mnt/app/root/retroarch/` | binary, `ra.sh`, cores, runtime libs, UI assets, controller profiles, factory config |
| `/mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar` | the HMI hook |

## 🔄 Step 3 — Reboot

Required whenever `ra_mhi2q.jar` changed — the HMI loads jars at boot. For a native-only update
(binary, cores, `ra.sh`, `retroarch.cfg`) skip it; just make sure no game was running.

## 🎮 Step 4 — First launch

1. Main menu → a new **Games** row. Select it.
2. Within a few seconds the screen switches to the RetroArch menu, full-screen, with sound. Turn
   the volume knob: the stock popup still draws on top.
3. Plug in a **USB** pad (Bluetooth pads are not supported, [[known-issues]]), pick a game from a
   playlist, play.
4. **BACK** or **MENU** returns to the car menu and restores whatever audio source was active.

A healthy first entry writes these lines, in this order:

```text
[SMM] runtime SMM install PASS: state=631 enterTrans=890 exitTrans=891 screen=250   (ra_hook.log)
RA state connected: acquiring OEM audio, saved context 25, native route=90{16,43}, clear=transparent
OEM route ready: launched native RetroArch
=== EGL context up: 1024x480 displayable=43 -- SUCCESS ===                          (ra_display.log)
routed display 0 -> context 90 {16,43}
ACTIVE focus=2 connection=20 route=1->1 PCM=ready                                   (ra_audio.log)
```

Read them over ssh:

```bash
$SSH 'cat /fs/sda0/retroarch/logs/ra_hook.log'    # HMI side: install + lifecycle
$SSH 'cat /fs/sda0/retroarch/logs/ra_audio.log'   # OEM audio session
$SSH 'cat /tmp/ra_display.log'                    # EGL / display routing
$SSH 'ls /fs/sda0/retroarch/logs/'                # retroarch__<timestamp>.log = frontend
```

| Symptom | Where to look |
| ------- | ------------- |
| No *Games* row | jar not in `/mnt/app/eso/hmi/lsd/jars/`, or not rebooted |
| *Games* does nothing | `ra_hook.log`: `runtime SMM install FAILED` = wrong firmware fingerprint |
| Back to the menu after a few seconds | CarPlay connected is the common cause ([[known-issues]]); otherwise the last lines of `ra_audio.log` |
| `refusing relaunch: prior native process missed exit timeout` | `slay -f -Q retroarch`, then retry |
| Black screen, `ra_display.log` ends at `FAIL egl_init_context` | started outside `ra.sh`, so `GRAPHICS_ROOT` was unset |
| No pad | HID topology lines in the newest `retroarch__*.log`; `hidview` on the unit shows what the pad reports |

## ⚙️ Updating and removing

**Update — SD only** (games, config, profiles): swap or re-copy the card; it takes effect at the
next launch. Existing config, saves and playlists on a card are never overwritten by an app
update; only the versioned migrations in `ra.sh` touch config ([[configuration]]).

**Update — native only:** Step 2 without the jar, no reboot.

**Update — jar:** Step 2 plus a reboot.

**Remove:**

```bash
$SSH 'mount -uw /mnt/app; rm -rf /mnt/app/root/retroarch /mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar; sync; mount -ur /mnt/app'
```

then reboot. Nothing else in the firmware was changed: the state-machine tables, display contexts
and audio bundle are only patched in memory while the HMI runs.

## 🔍 Collecting diagnostics

For a bug report, collect `ra_run.log`, `ra_hook.log`, `ra_audio.log`, the newest
`retroarch__*.log` (all in `/fs/sda0/retroarch/logs/`), `/tmp/ra_display.log`, `/tmp/qsa_perf.log`,
and for a crash the newest `/mnt/ota/system/core/*.core.gz` ([[profiler]]).
`tools/qnx-bench/run-session.sh` automates a timed launch plus log harvest ([[gles2-benchmark]]).
