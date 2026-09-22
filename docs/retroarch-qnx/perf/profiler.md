---
title: Profiling and crash analysis on the unit
tags: [perf, tools, howto]
status: verified-hardware
sources:
  - tools/qnx-profiler/{ra_prof.c,ra_prof_report.py,ra_prof_hot.py}
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md §6
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §5
  - diagnostics/hu-crash-1970-01-01-001049/analysis/qnx_core_notes.py
reconciles:
  - both docs (profiler / diagnostic-recipe sections)
---

# Profiling and crash analysis on the unit

## Do not use `tracelogger`

The instrumented kernel is running and the SDP tool works, but the load spike **reboots the unit**
(HU watchdog; not memory pressure - 890 MB of 2048 free).

## `ra_prof` - a sampling profiler that the HU tolerates

`tools/qnx-profiler/ra_prof.c` (QNX ARM binary `ra_prof`, staged in `build/ra_prof`). Samples
`DCMD_PROC_TIDSTATUS` per thread - one devctl per thread per tick, ~1000/s at 100 Hz. Two 30 s
captures caused no disturbance.

```sh
# on the unit
/tmp/ra_prof <pid> <seconds> [hz] [tid]  > /tmp/ra_prof.txt
# on the host
python3 tools/qnx-profiler/ra_prof_report.py trace.txt tools/qnx-profiler/symbols   # states, worst blocks, flat profile
python3 tools/qnx-profiler/ra_prof_hot.py    trace.txt tools/qnx-profiler/symbols   # per-object hot functions (ip - base)
```

The symbols dir is **positional**; a wrong path is swallowed silently (`0 object(s) with symbols`).
`tools/qnx-profiler/symbols/` holds unstripped copies of `retroarch` and the cores; each one's
`strip` output must be byte-identical (`cmp`) to what runs on the unit or every address lies.
`tools/qnx-bench/run-session.sh --profile-secs N` automates launch + mid-session capture.

**Limitation:** flat profile, one IP per thread per sample, no call stacks (`TIDSTATUS` has no
unwind). `MAP` lines from `DCMD_PROC_MAPDEBUG` name every mapped object - without them all
out-of-frontend addresses collapse onto the last symbol (`_fini` once showed a bogus 91.8 %).

## Decoding `libc` hotspots

Boot-image objects (`proc/boot/libc.so.3`) have no symbols and `DCMD_PROC_MAPINFO`'s `offset` is
into the IFS image, not the file. Use `ip - map_vaddr` as the library VA. Bytes that look like
`push {lr} / mov r12,#N / svc #0x51` are the **kernel call stub table**: decode `N` against
`kercalls.h` in the SDP (0x53 condvar signal, 0x50 mutex lock, 0x54 sem post, 0x51 mutex unlock).
Example outcome: [[qnx-sync-cost]].

## Core dumps

`dumper` is configured with `-d /mnt/ota/system/core`; files are `<name>.core.gz`. Registers and
thread states: `diagnostics/hu-crash-1970-01-01-001049/analysis/qnx_core_notes.py <core>`.
Heuristics that paid off:

- `ip` with **no module name** = JIT code ([[jit-icache-qnx]]);
- `lr` inside a core's own BSS = the caller was JIT-generated code (dynarec ABI bug, see PCSX `r0`);
- `memcpy` faulting on the source at a page boundary = the source buffer was shorter than the
  declared length (GLideN64 vertex span).

## Other diagnostics

- QSA: `/tmp/qsa_perf.log` ([[audio-qsa]]); display: `/tmp/ra_display.log` ([[video-context]]);
  HMI: `ra_hook.log`, `ra_audio.log`; RetroArch: `retroarch__*.log` ([[launcher-ra-sh]]).
- `slog`/`sloginfo` for system-side faults (`ifs`, io-hid, display manager).
- File transfer without `scp`: `ssh host 'cat > /path' < file`, verify with `cksum` on the host
  ([[install-procedure]]).
