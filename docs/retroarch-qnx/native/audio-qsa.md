---
title: QSA audio driver - PCM transport on mpl1_int_ent
tags: [native, audio]
status: verified-hardware
sources:
  - src/audio/drivers/qnx_qsa.c
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §4, §5
  - firmware: armle/lib/dll/deva-ctrl-qc.so, org/dsi/ifc/audio/Constants.java
reconciles:
  - README.md milestone 5a, "Which channel, and who sets MS_ENT"
---

# QSA audio driver - PCM transport on mpl1_int_ent

`audio_driver = "qsa"` (`src/audio/drivers/qnx_qsa.c`, `-lasound`). PCM transport **only**: it
never touches `MS_ENT` or any mixer switch; focus/route belong to Java ([[audio-session]]).

## Device choice

| virtual channel (`org/dsi/ifc/audio/Constants`) | id | `/dev/snd` | use |
|---|---|---|---|
| `VIRTUALCHANNEL_ENT_INTMEDIA` | 1 | **`mpl1_int_ent`** | internal media (HU's own USB/SD/BT player) - **ours** |
| `VIRTUALCHANNEL_ENT_DIO` | 4 | `mpl5_dio_ent` | CarPlay - what gpSP grabbed (collides with CarPlay) |
| `VIRTUALCHANNEL_ENT_GAL` / `BCL` | 5 / 33 | `mpl6_gal_ent` / `mpl7_bcl_ent` | Android Auto / BaiduCarLife |

`mpl1` is provisioned (ACDB/CSD calibration) because the stock player uses it daily; "free" channels
may exist on paper and play into nothing. `deva-ctrl-qc.so` owns the Qualcomm CSD/amp session
internally; `/dev/audio_service`, `/dev/csdProxy`, `audio_chime` are not touched.

## Open / prefill / handshake

1. `snd_pcm_open_name(RA_QNX_AUDIO_DEV | /dev/snd/mpl1_int_ent, PLAYBACK)`;
   `snd_pcm_plugin_set_disable(PLUGIN_DISABLE_MMAP)` (consistent DMA with `snd_pcm_write`).
2. `snd_pcm_channel_info`: take the device's **native voice count** (`mpl1` = 6, `mpl5` = 2) -
   stereo is upmixed into it (L,R,L,R,... by default; `RA_QNX_AUDIO_CHANNEL_MAP="l,r,0,0,l,r"` to
   re-test layouts by ear), pick S16 or S32 as advertised (some firmware advertises mpl1 as S32
   only), clamp the rate (48 kHz), fragment size from `min_fragment_size`.
3. Hardware ring: 8..16 fragments; software FIFO reserve: 16 fragments.
4. A worker thread (pinned to **CPU3** via `_NTO_TCTL_RUNMASK`; PCSX reserves CPU1 for CDR+SPU and
   CPU2 for its async GPU) primes the ring with up to 10 fragments of **real** PCM
   (`qsa_worker_prime`) before steady state.
5. `/tmp/retroarch.pcm.ready` is written after the worker started (init) and after every
   successful focus-recovery `qsa_start`; it is removed on stop/close/free. Java waits for it before
   requesting connection 20 / fading in ([[session-lifecycle]]).

## Steady state

- Producer (`qsa_write`, RetroArch main thread): converts S16 stereo -> native format/voices into
  the FIFO; blocks when the FIFO is full (that blocking is what paces the core when vsync is off).
- Worker: takes one fragment per loop; when the hardware ring is down to
  `QSA_HW_LOW_WATER_FRAGS=2` and no real PCM is available it submits **one** concealment fragment
  (5 ms fade of the last sample -> silence) and re-checks status after each, so it never fills the
  ring with silence just as the producer wakes up.
- Underrun recovery: `snd_pcm_channel_status` -> `snd_pcm_channel_prepare` -> re-prime with 8
  fragments.
- Focus loss (`audio_driver_stop` via SIGRTMIN): flush, clear marker. Focus gain
  (`audio_driver_start`): flush, prepare a blank START_DATA ring, publish marker, wait for real PCM
  to prime again.

## Dynamic rate control

`qsa_rate_control_state` reports `control_avail = local_avail/2 + fifo_cap/2`. RetroArch treats
`buffer_size/2` as neutral, so this parks the equilibrium at a **full** software queue and stays
linear over the whole range (the original `MIN(cap, avail + cap/2)` saturated at half-empty and
could not modulate; plain `avail` halved every core's cushion to ~128 ms).

## Telemetry (`/tmp/qsa_perf.log`, `RA_QNX_AUDIO_PERF_LOG`)

One line per window of 300 rate-control queries:

```
DRC reserve=15/16 fragments (free=1), production=590 worker=586 fragments/window;
calls=... conceal=0 idle_max=38.000 ms write_max=30.200 ms input_max=... frames
```

How to read it ([[frame-and-audio-pacing]]): `production ~ worker` always holds once audio paces
the core and proves nothing; `conceal / worker` is the seam rate; `reserve` is an instantaneous
sample; `producer_waits` (session total) is the honest headroom indicator - 0 means the core never
runs ahead of real time.
