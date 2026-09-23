---
title: Hardware validation matrix - proven vs pending
tags: [testing, status]
status: verified-trace
sources:
  - diagnostics/hu-20260815-*, out/hu-logs/20260809-*, build/bench, build/gles2-bench, build/profiles
  - docs/2026-08-22-*, docs/2026-08-31-*
  - git log (8c54b47e is the first commit after the unit went offline)
---

# Hardware validation matrix - proven vs pending

Per-item status. The user-facing view of the same information - what breaks and what to do about
it - is [[known-issues]].

Last hardware session: **2026-09-02** (benchmarks). The 2026-09-01/22 HMI changes have not been on
the unit. "Proven" = observed in logs or measured; "pending" = built and reasoned only.

## Proven on MU1316

| Area | Evidence |
|---|---|
| Games row, runtime SMM injection, RaScreen enter/exit, restore of the previous context | `ra_hook.log` milestones (Aug 2026), [[games-menu-injection]] |
| Native launch via `ra.sh`, PID lock, SIGTERM clean exit with SRAM flush, exit marker -> `EV_EXIT` | [[session-lifecycle]], [[signals-and-lock]] |
| Display on displayable 43, context `{43}`, 3 buffers, bounded vsync, interval-0 swap | `/tmp/ra_display.log`, [[video-context]], [[egl-swap-path]] |
| OEM audio session to `ACTIVE` and clean release; focus loss/recovery (phone) with 41/42 | `ra_audio.log`, [[audio-session]] |
| QSA on `mpl1_int_ent` 6 voices, prefill handshake, DRC formula, concealment | `qsa_perf.log`, [[audio-qsa]] |
| HID pad (report index 1 fallback parser), 8BitDo, hot-plug, io-hid per session | [[input-hid-xusb]] |
| gpSP (interpreter), PCSX-ReARMed (dynarec, NFS3 after the `r0` fix), Mupen64Plus-Next (dynarec + GLideN64 threaded after the condvar gate) | [[cores-overview]], [[qnx-sync-cost]] |
| Content discovery playlists from both cards | [[content-discovery]] |
| GCC 8.5 + gas 2.38 frontend/cores; libstdc++ without math stubs | [[toolchain]] |
| Profiler `ra_prof`; `tracelogger` reboots the unit | [[profiler]] |
| GLES2 driver split, buffer count, binning/power/FBO/API-traffic matrix | [[gles2-benchmark]] |
| `flock` unsupported on FAT32 and tmpfs (PID file only) | [[signals-and-lock]] |
| Four Screen buffers -> `EGL_BAD_ALLOC` + temporary hang | [[egl-swap-path]] |

## Pending on hardware

| Item | Risk if wrong | Where |
|---|---|---|
| CarPlay connected at launch: does the session now survive focus 48 instead of exiting? | the reported "exits immediately with CarPlay" symptom stays | [[known-issues]] |
| Context 90 = `{16, 43}` with transparent HMI plane + status-bar stub; volume popup visible; no HMI chrome over the game | game hidden behind HMI, or popups missing | [[display-context-90]] |
| `MediaSessionBridge`: entry from CarPlay focus 48 no longer bounced by context 9; BAP `RetroArch / Playing` | audio bounce on CarPlay entry; VC shows NO_PLAYABLE_FILES | [[audio-session]] |
| `waitMutedForRecovery` instead of exiting on focus/route timeouts | RA stays muted forever instead of returning | [[audio-session]] |
| Manual Save + Load state per core (needed before `RA_QNX_AUTO_SAVE_STATE=1`) | crash in EHABI unwind on state ops (seen with PPSSPP before the runtime repair) | [[signals-and-lock]] |
| XUSB/GIP Xbox pads and every rumble family, controller by controller | dead pad / wrong report | [[input-hid-xusb]] |
| Bluetooth pads: needs a second io-hid transport **and** pairing driven from `btstack` - not attempted | BT pads remain unusable | [[input-hid-xusb]], [[known-issues]] |
| `video_vsync` default for PS1/GBA (factory says `true`, PS1 measured better with `false`) | small permanent audio deficit | [[frame-and-audio-pacing]] |
| Connection-20 "sticks" (dormant OEM issue) | Games refuses to start until reboot | [[audio-session]] |
| Freedreno gates 1-7 | - (research) | [[freedreno-qnx]] |
| Forced SSAA absent for the `retroarch` process name (expected absent) | 2x render cost | [[adreno-driver-hotpath]] |

## Never validated, by design

`SIGUSR1/SIGUSR2` pause/resume from the HMI (no sender exists), HOLD-BACK SIGKILL escalation (not
implemented), a G24-cluster unit (context 90 collides with a stock KDK context there).
