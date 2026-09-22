---
title: Frame and audio pacing - who owns the clock
tags: [perf, audio, video]
status: verified-hardware
sources:
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §4
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md §4-§5
  - src/gfx/drivers_context/qnx_ctx.c, src/audio/drivers/qnx_qsa.c, pkg/retroarch.cfg, pkg/ra.sh
reconciles:
  - both docs above (pacing sections)
---

# Frame and audio pacing - who owns the clock

Two clocks can pace an emulator here: the display (vsync) or the audio device (blocking PCM
writes). Each was measured; the shipped default is a compromise that still has an open question.

## The mechanisms

- **Blocking audio** (`audio_sync=true`, `video_vsync=false`, `fastforward_ratio=0` = no frame
  limiter): the core runs until the QSA FIFO is full, then blocks in `qsa_write`. The audio device
  is the master clock; drift is impossible by construction. Cost: if the core cannot keep up, the
  FIFO never fills, nothing throttles, and any hitch is a seam.
- **Vsync** (`video_vsync=true`): bounded `screen_wait_vsync` or a 16.67 ms software deadline
  ([[video-context]]). RetroArch derives the resampler ratio from `video_refresh_rate=60.000000`
  and never measured the panel (`Does not have enough samples for monitor refresh rate estimation`).
  A panel at 59.5 Hz means a permanent ~1 % audio deficit that `audio_rate_control_delta=0.005`
  (needs ~30 s to rebuild one 16 ms fragment) cannot hide.

## Measurements

| Config | Core | Result |
|---|---|---|
| vsync off | PPSSPP | 22.6 ms/frame idle spin in `post`; frontend frame 24 ms |
| vsync on | PPSSPP | frontend 9.8 ms of which 9.4 ms wait; `reserve=15/16 conceal=0` steady state |
| `auto_frameskip` | PPSSPP | removed the rate limiter with nothing replacing it: ran faster than real time, audio pitched up - **never use frameskip as an underrun cure** |
| vsync on | PS1 | permanent small deficit (see above) |
| vsync **off** | PS1 | `reserve 0/16 -> 15/16`, `conceal 0` in every window |
| waiter-gated condvar | N64 | `producer_waits 0 -> 4318`, reserve 14-15/16 ([[qnx-sync-cost]]) |

Frontend cost itself is 1.1 % of CPU - nothing left to win in RetroArch.

## Shipped state and the open contradiction

`pkg/retroarch.cfg` ships `video_vsync = "true"` and `ra.sh` config-migration v2 forces existing
cards from `false` to `true`. That was done for PPSSPP, which is no longer shipped, while the PS1
measurement favours `false`. **Decision pending**: either revert the factory value and add
migration v3 (`true -> false`), or measure the panel's real refresh once and keep vsync. Until then
a card can be flipped by hand in `config/retroarch.cfg`.

## DRC signal

`qsa_rate_control_state` reports `local_avail/2 + cap/2`, parking equilibrium at a full 16-fragment
queue with a linear response ([[audio-qsa]]). `audio_rate_control_delta` stays +-0.5 %: it corrects
drift, not a core running 5-10 % slow (raising it would audibly change pitch).

## Reading the telemetry together

- QSA `production ~ worker` always holds once audio paces the core - proves nothing alone.
- `conceal / worker` is the seam rate; `producer_waits` > 0 means the core has headroom.
- `idle_max` is the longest producer gap (level loads: 600-800 ms; the 16 x 48 ms reserve covers
  it only if it was full and refills ~4 fragments per window).
- Core `swap` maxima near one refresh are normal; repeated values above the 25 ms vsync timeout
  point at presentation trouble.
