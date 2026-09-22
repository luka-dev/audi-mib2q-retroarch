---
title: Signals, lock file and the runloop reconcile
tags: [native, lifecycle]
status: verified-hardware
sources:
  - src/frontend/drivers/platform_qnx.c
  - src/gfx/drivers_context/qnx_ctx.c (gfx_ctx_qnx_check_window, swap_buffers)
  - java_patch/java_src/com/luka/retroarch/inject/Shell.java
reconciles:
  - README.md "Lifecycle / focus state machine" (signal table), milestone 3.5
---

# Signals, lock file and the runloop reconcile

One-way command channel, HMI -> emulator, plain POSIX signals. Nothing is negotiated.

| Signal | Sender | Native effect (applied in the runloop, never in a handler) |
|---|---|---|
| `SIGUSR1` / `SIGUSR2` | (reserved: context switched away/back) | desired pause = 1 / 0 -> `CMD_EVENT_PAUSE/UNPAUSE`; on pause `CMD_EVENT_SAVE_FILES` (SRAM) and, only if `RA_QNX_AUTO_SAVE_STATE=1`, `CMD_EVENT_SAVE_STATE`; `swap_buffers` skips `eglSwapBuffers` while paused (0 GPU) |
| `SIGRTMIN` (41) | Java `Shell.signalRetroArchAudioFocusLost` | one edge: read `/tmp/retroarch.audio.desired`; if `0` -> `CMD_EVENT_PAUSE` + `audio_driver_stop()` |
| `SIGRTMIN+1` (42) | Java `Shell.signalRetroArchAudioFocusGained` | same edge; if `1` -> `audio_driver_start()`; **core stays paused** (the user resumes) |
| `SIGTERM` / `SIGINT` | Java `Shell.terminateRetroArch`, `ra.sh` trap | `qnx_lifecycle_quit=1` -> RetroArch's normal quit: SRAM flush, core unload, exit |
| `SIGKILL` | ssh `slay -f` | uncatchable; `ra.sh` still reaps and clears the lock |

Today the Java hook sends only 15, 41 and 42. SIGUSR1/2 are wired natively but no HMI code emits
them (the RA state is either connected or not; forced transitions disconnect it and SIGTERM).

## Why `sigwaitinfo()` in a dedicated thread

On QNX a process-directed signal lands on any thread that does not block it - possibly the QSA
worker. SRAM/EGL/QSA/mutex/logging are not async-signal-safe. So `frontend_qnx_install_signal_handlers`
blocks `TERM INT USR1 USR2 RTMIN RTMIN+1` in the main thread **before** RetroArch spawns threads
(children inherit the mask) and starts one thread that only flips `volatile sig_atomic_t`s:
`qnx_lifecycle_quit`, `qnx_lifecycle_paused`, `qnx_audio_focus_command_edge`,
`qnx_audio_focus_fallback_state`. `gfx_ctx_qnx_check_window()` reads them once per frame on the main
thread and calls `command_event`.

Signals coalesce and QNX delivers realtime signals by priority, not send order: 41 and 42 could be
reordered. Hence the **desired-state file** - either signal is just a wakeup; the file carries the
truth; the signal number is only a fallback when `/tmp` is unwritable.

## Lock file reality

`RA_LOCK_PATH=/tmp/retroarch.lock` (default in code is `/fs/sda0/retroarch/ra.lock`, overridden by
`ra.sh`). Sequence in `qnx_lock_acquire`:

1. `open(O_RDWR|O_CREAT)`, `FD_CLOEXEC`;
2. **write our PID first, unconditionally** - the HMI must be able to signal us even if locking
   fails;
3. try `flock(LOCK_EX|LOCK_NB)`: `EINTR` retry; `ENOSYS/EOPNOTSUPP` -> log and proceed;
   `EWOULDBLOCK` -> poll 100 ms up to 8 s, then proceed anyway (HMI must SIGKILL the wedged one).

**Measured on the unit: neither FAT32 (`/fs/sda0`) nor tmpfs (`/tmp`) supports `flock`.** So the
lock file is in practice only a PID file; serialization of relaunches is done in Java
([[session-lifecycle]]) and by `ra.sh` clearing only the PID it reaped ([[launcher-ra-sh]]).
Because `ra.sh` stays the parent, the PID in the file is RetroArch's own (`getpid()`), never a shell.

## Verified vs pending

- Verified on HU: SIGTERM -> clean exit with SRAM flush (PS1 saves persist); 41/42 pause/stop and
  restart audio; PID file always populated.
- Pending: `RA_QNX_AUTO_SAVE_STATE=1` (savestate on pause) is disabled until every core passes a
  manual Save+Load cycle on hardware ([[hardware-validation-matrix]]).
