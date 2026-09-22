# retroarch-qnx

Play Game Boy Advance, PlayStation and Nintendo 64 games on the built-in screen of an
Audi MMI head unit (MHI2Q / MIB2 High, QNX 6.5), with a USB or Bluetooth gamepad and
sound through the car's amplifier.

It is a port of [RetroArch](https://github.com/libretro/RetroArch) plus three libretro
cores (gpSP, PCSX-ReARMed, Mupen64Plus-Next) that runs *inside* the stock infotainment
system: a small Java hook adds a **Games** entry to the main menu, the native emulator
takes over the display while a game runs, and the car's own audio manager, volume
knob, phone calls and parking sensors keep working around it. Nothing in the stock
firmware is modified on disk.

**Status:** working on one unit (firmware `MHI2Q_US_AUG22_P5087_MU1316`, US navigation
variant). Everything up to 2026-08-31 has been exercised on the hardware; the changes
from September 2026 (volume popup over the game, CarPlay-entry audio fix) are built but
not yet confirmed on the unit. See
[what is proven vs pending](docs/retroarch-qnx/testing/hardware-validation-matrix.md).

## What you get

- **Three systems**: GBA (gpSP), PS1 (PCSX-ReARMed, ARM dynarec), N64 (Mupen64Plus-Next,
  GLideN64 on GLES2). PSP was ported and measured but is not shipped — the stock GPU
  driver is too slow for it ([why](docs/retroarch-qnx/cores/ppsspp-status.md)).
- **Integrated, not bolted on**: a real HMI state machine entry; BACK/MENU return to the
  car menu; a phone call pauses the game and hands it back afterwards; a forced screen
  (reverse camera, parking sensors) closes it cleanly with saves flushed; audio goes through the OEM entertainment session, so volume, mute and ducking behave
  like radio or media.
- **Appliance UX**: Ozone menu at 1024x480 with Audi fonts, playlists generated
  automatically from the SD card, 234 controller profiles, rumble, cheats, game databases.
- **Nothing written to the firmware partition**: the app image is read-only at runtime;
  all state (config, saves, playlists, logs) lives on a removable FAT32 SD card.

## Requirements

| Host (macOS/Linux) | Version |
|---|---|
| Docker | any recent; the `qnx65-armv7-toolchain` image (GCC 8.5.0 + binutils 2.38 `as`) is built by `../qnx-65-sdp-docker` |
| JDK 8 + the MU1316 class/JCL jars | from `../jxe2jar` (`lsd_patch/build.sh` prints the exact paths) |
| `sshpass` | only for the helper scripts that talk to the unit |

| Head unit | |
|---|---|
| Hardware | MHI2Q / MU1316 (APQ8064, Adreno 320, 1024x480), US nav variant |
| Firmware | `MHI2Q_US_AUG22_P5087_MU1316` — the HMI hook checks an exact fingerprint of the stock state tables and refuses to install on anything else |
| Access | root ssh (legacy `ssh-rsa`), ability to `mount -uw /mnt/app` |
| Media | FAT32 SD card in slot 1 (tested: 32 GB) |

## Quick start

```sh
git clone <this repo> retroarch-qnx && cd retroarch-qnx
./build.sh
```

Builds the HMI jar, the frontend and the three cores inside Docker and stages two
deployable trees. The last lines look like:

```
=== artifacts ===
  retroarch (stripped): 2330824 bytes  (exec ceiling 15 MB)
  gpSP core (stripped) : 671856 bytes  (git 8b5812e)
  PCSX core (stripped) : 1421452 bytes  (fresh cores-src build)
  Mupen core (stripped): 3227888 bytes  (git f275caf; GLES2 + ARM dynarec)
  mnt_app image       : ... bytes -> build/mnt_app
  SD-card image       : ... bytes -> build/sd_card
  ...
  frontend Machine:  ARM
  frontend Flags:    0x5000002, Version5 EABI, has entry point
```

Then:

1. Copy `build/sd_card/*` to the root of a FAT32 SD card and insert it in slot 1.
   Put games in `retroarch/ps1`, `retroarch/gba`, `retroarch/n64` (a second card in
   slot 2 is scanned too). PS1 BIOS goes in `retroarch/system/`.
2. On the unit: `mount -uw /mnt/app`, copy `build/mnt_app/*` into `/mnt/app/`
   (binary, `ra.sh`, cores, assets under `root/retroarch/`; the jar under
   `eso/hmi/lsd/jars/`), `chmod 755` the binary and `ra.sh`, `sync`.
3. Reboot the unit (the jar is loaded at boot). A **Games** row appears in the main
   menu; select it. First-run checks and log locations:
   [install procedure](docs/retroarch-qnx/deploy/install-procedure.md).

Optional: `./run-macos.sh` builds a native macOS copy of the same UI at 1024x480
(`out/macos-test/RetroArchTest.app`) for menu/asset work without the car;
`./fetch-thumbnails.sh <games dir>` downloads box art into the SD tree.

## How it works (one paragraph)

The HMI runs a Java state machine. On the first *Games* press the hook appends one
state and two transitions to it at runtime (fail-closed against a table fingerprint),
whose screen launches `/mnt/app/root/retroarch/ra.sh`. The native process renders
through the firmware's `libdisplayinit` onto compositor displayable 43 and switches the
display to a private context where that layer sits under a transparent HMI plane; audio
is plain PCM into `/dev/snd/mpl1_int_ent` while the Java side asks the stock audio
manager for media focus and entertainment connection 20 exactly like the built-in media
player would. Java and native talk only through POSIX signals and marker files in `/tmp`.
Full picture with diagrams: [architecture](docs/retroarch-qnx/architecture.md).

## Documentation

The documentation is an Obsidian vault at **`docs/retroarch-qnx/`** (open the folder as a
vault; plain Markdown otherwise). Start at
[`INDEX.md`](docs/retroarch-qnx/INDEX.md). Every note states how it was verified.

| I want to… | Read |
|---|---|
| understand the build and the toolchain traps | [build-pipeline](docs/retroarch-qnx/build/build-pipeline.md), [toolchain](docs/retroarch-qnx/build/toolchain.md) |
| install or update the unit, collect logs | [install-procedure](docs/retroarch-qnx/deploy/install-procedure.md), [launcher-ra-sh](docs/retroarch-qnx/deploy/launcher-ra-sh.md) |
| change what the HMI hook does | [games-menu-injection](docs/retroarch-qnx/hmi/games-menu-injection.md), [session-lifecycle](docs/retroarch-qnx/hmi/session-lifecycle.md), [audio-session](docs/retroarch-qnx/hmi/audio-session.md) |
| touch video / audio / input code | [video-context](docs/retroarch-qnx/native/video-context.md), [audio-qsa](docs/retroarch-qnx/native/audio-qsa.md), [input-hid-xusb](docs/retroarch-qnx/native/input-hid-xusb.md) |
| add or tune a core | [cores-overview](docs/retroarch-qnx/cores/cores-overview.md), [jit-icache-qnx](docs/retroarch-qnx/cores/jit-icache-qnx.md) |
| profile or debug a crash | [profiler](docs/retroarch-qnx/perf/profiler.md), [qnx-sync-cost](docs/retroarch-qnx/cores/qnx-sync-cost.md) |
| know why the GPU is the limit and what the research does about it | [adreno-driver-hotpath](docs/retroarch-qnx/re/adreno-driver-hotpath.md), [freedreno-qnx](docs/retroarch-qnx/research/freedreno-qnx.md) |

Older long-form write-ups are kept unchanged in `docs/legacy/` for history.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| *Games* row missing after install | jar not in `/mnt/app/eso/hmi/lsd/jars/` or no reboot yet |
| *Games* does nothing; `ra_hook.log` says `runtime SMM install FAILED` | firmware is not MU1316 — the state-table fingerprint (631 states / 890 transitions) did not match; nothing was changed |
| Screen goes back to the menu after ~10 s, `ra_audio.log` ends in `activation aborted` / `QSA PCM handshake timeout` | native did not open the PCM device or the OEM audio manager refused; read [audio-session](docs/retroarch-qnx/hmi/audio-session.md) |
| `ra_hook.log`: `refusing relaunch: prior native process missed exit timeout` | a previous RetroArch is still alive; `slay -f -Q retroarch` over ssh, then retry |
| `/tmp/ra_display.log` stops at `FAIL egl_init_context` | `GRAPHICS_ROOT` not exported — always start through `ra.sh` |
| Pad not detected | check the newest `logs/retroarch__*.log` for HID topology; `hidview` on the unit shows what the pad reports |
| Unit stopped answering ssh after a video experiment | you asked for 4 Screen buffers; only 2 or 3 are supported |
| `ra.sh` run by hand dies with an empty timestamp; `tar`/`cksum`/`scp` "not found" over ssh | the ssh login PATH lacks `/armle/usr/bin`: `export PATH=/armle/usr/bin:/armle/bin:$PATH` first |

Logs live in `/fs/sda0/retroarch/logs/` (`ra_run.log`, `ra_hook.log`, `ra_audio.log`,
`retroarch__*.log`) and `/tmp/ra_display.log`.

## Limitations and non-goals

- One firmware, one unit. Other MIB2 variants (including the G24 cluster, where
  display context 90 collides with a stock context) will not install.
- No PSP. The stock Adreno GLES2 driver spends ~16 ms per PSP frame validating
  commands on the CPU; a Mesa/Freedreno backend exists in `tools/qnx-freedreno` but is
  QEMU-only research.
- Save states on lifecycle pause are disabled until every core passes a manual
  Save+Load on hardware (`RA_QNX_AUTO_SAVE_STATE=0`).
- No online features: no updaters, netplay, achievements or thumbnails download from
  the unit; everything is on the SD card.
- Not a general RetroArch build for QNX: the BB10 code paths are replaced, not
  maintained; there is no `./configure`.

## Repository layout

```
src/            RetroArch (vendored; QNX frontend, display, QSA, HID/XUSB, lifecycle, playlist scanner)
cores-src/      gpsp, pcsx_rearmed, mupen64plus_next (shipped); ppsspp (ported, not shipped)
java_patch/     HMI hook sources (Java 1.4)         lsd_patch/   jar build script + ra_mhi2q.jar
pkg/            factory config, ra.sh, assets, controller/rumble profiles, databases, cheats
build/          deployable trees (mnt_app, sd_card) and research artefacts
tools/          qnx-qemu, qnx-tests, qnx-bench, qnx-profiler, qnx-freedreno, qnx-gsl-port
docs/           retroarch-qnx/ (Obsidian vault), legacy/     output/r2/   RE disassembly evidence
```

Upstream commits of every vendored tree: [`VENDORED_SOURCES.md`](VENDORED_SOURCES.md).

## License

RetroArch and the cores are GPL-3.0 (see `src/COPYING` and each `cores-src/*` tree).
The QNX-specific changes in `src/` and `cores-src/` are contributed under the same
licenses. The Java hook in `java_patch/` and the scripts/tools in this repository are
provided as-is for use with a unit you own; assets under `pkg/` carry their own
`COPYING`/`SOURCE.txt` (Ozone/Audi assets, libretro databases CC BY-SA 4.0, controller
profiles). Audi, MMI and MIB are trademarks of their owners; this project is not
affiliated with them.
