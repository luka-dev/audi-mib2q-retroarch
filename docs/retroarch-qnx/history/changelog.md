---
title: Timeline - what changed and why
tags: [history]
status: verified-source
sources:
  - git log (18 commits, 2026-08-08 .. 2026-09-22)
  - docs/2026-08-22-*, docs/2026-08-31-*
---

# Timeline - what changed and why

Earlier history (gcc49 port bring-up, milestones 0-5) lived only in the README and is summarised in
[[architecture]]; the repository starts at a squashed checkpoint.

| Date | Commit | What | Why / result |
|---|---|---|---|
| 2026-08-08 | `59d1bacb` | checkpoint: complete MHI2Q RetroArch project | frontend + gpSP + PCSX running on the unit with the GCC 4.9 toolchain |
| 2026-08-08 | `107895f7`, `ff8554e5`, `fb9abcf7` | immutable `/mnt/app` + resettable SD images; reproducible tree; Ozone icons; macOS UI test | [[filesystem-layout]], `run-macos.sh` |
| 2026-08-09 | `f9139dae`, `93b8cdf2` | vendor all sources; add Nintendo 64 (Mupen64Plus-Next GLES2) | [[vendored-sources]], [[cores-overview]] |
| 2026-08-09 | `fb11bde7`, `bea1d570` | root quit confirmation, core-option seeds; harden OEM audio lifecycle, Java namespace `com.luka.retroarch` | [[audio-session]], [[configuration]] |
| 2026-08-12 | `a5780dab` | 3-buffer window path, QSA focus/recovery + DRC hardening, HID/JIT fixes; gpSP switched to the interpreter at runtime | [[video-context]], [[audio-qsa]] |
| 2026-08-15 | `4a1d03a6`, `e1651bc4`, `4aaa5e99` | stabilise audio/N64 threading/gamepad UI; **PPSSPP** core + QNX ARMv7 port; offline PPSSPP profile | [[ppsspp-status]] |
| 2026-08-15 | `6b44a9bf` | **rebuild everything with GCC 8.5** | [[toolchain]] |
| 2026-08-21 | `568a4b5d` | launch with CarPlay owning the front terminal: `signalFocusLost` was gated on `nativeLaunchIssued`; the initial focus-48 callback aborted every session | `lossArmed` gate ([[audio-session]]) |
| 2026-08-22 | `44400c6a` | gas 2.19 VFP fix -> binutils 2.38; libstdc++/libm ordering; PPSSPP I-cache, CPU detect, vsync on, telemetry; QEMU harness + unit suites; profiler | [[toolchain]], [[qemu-harness]], [[profiler]] |
| 2026-08-24 | `7f975612` | drop `ra_math.c` shim; libstdc++ rebuilt without math stubs | [[toolchain]] |
| 2026-08-31 | `4d816dab` | condvar signal is a syscall -> waiter-gated GLideN64 queue; PCSX GTE `r0`; GLideN64 vertex span | [[qnx-sync-cost]] |
| 2026-09-02..09 | (uncommitted until 8c54b47e) | GLES2 benchmark matrix; `OpenGLES20.so` audit; GSL boundary RE; Freedreno QNX backend in QEMU | [[gles2-benchmark]], [[adreno-driver-hotpath]], [[gsl-port-boundary]], [[freedreno-qnx]] |
| 2026-09-22 | `8c54b47e` | context 90 = `{16,43}` + transparent HMI + status-bar stub; `MediaSessionBridge` (Media context 20, BAP Playing); muted-wait recovery; **PPSSPP removed from the image**; research tools committed; Mesa trees gitignored | [[display-context-90]], [[audio-session]], [[ppsspp-status]] |

## Decisions that are easy to re-litigate by accident

- **No sockets/IPC between Java and native** - signals + `/tmp` markers only ([[architecture]]).
- **Never write `MS_ENT`; never register a raw DSI router client or SDIS context** ([[audio-session]]).
- **Never patch `DisplayManagerMIB2High` in this jar** ([[display-context-90]]).
- **`/mnt/app` is read-only at runtime; every write goes to the SD** ([[filesystem-layout]]).
- **Frameskip is not an underrun cure** ([[frame-and-audio-pacing]]).
- **Do not run `tracelogger`; do not request 4 Screen buffers** ([[profiler]], [[egl-swap-path]]).
- **PPSSPP stays out until a faster GL path exists** ([[ppsspp-status]], [[freedreno-qnx]]).
