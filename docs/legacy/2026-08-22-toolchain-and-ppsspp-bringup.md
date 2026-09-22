# 2026-08-22 — GCC 8.5 bring-up, PPSSPP on hardware, and a profiler

One session, four independent defects, one new tool. Sections 1–8 record
hardware measurements. The later static-runtime, SaveState-hardening and direct
telemetry work was completed while the unit was offline and is labelled where
hardware validation remains pending.

---

## 1. gas 2.19 mis-encodes three of the four VFP multiply-accumulate mnemonics

**The big one.** Cost most of the day and had been latent in the toolchain from
the start.

### Symptom

The GCC 8.5 frontend (commit `6b44a9bf`) had never run on hardware. When
deployed it produced, in order of discovery:

- menu text missing entirely, selection outline shifted ~30% left;
- with the rasteriser at `-O0`: glyphs as hollow outlines;
- crashes: `SIGSEGV ip=…(libstdc++.so.6@sqrtf+0x1c) ref=0005fff8`.

Three faces of the same bug.

### Root cause

GCC emits **correct** mnemonics. The SDP's `gas 2.19.1` (2007) encodes them
wrong. Established by assembling GCC's own output and decoding the bytes with a
modern disassembler:

| GCC emits | gas 2.19 encodes as | effect |
|---|---|---|
| `vmla`  | `vmla`  | correct |
| `vmls`  | `vnmls` | negates the whole result |
| `vnmla` | `vmls`  | flips the sign of the addend |
| `vnmls` | `vnmla` | flips the sign of the product |

Every `a*b±c` silently computes the wrong value. Upstream fixed this in
`gas/config/tc-arm.c` on 2009-10-29 — **binutils 2.20**. 2.19 predates it.

GCC 4.9 rarely formed these patterns, so the bug lay dormant for the life of
the port. GCC 8.5 forms them freely.

### Proof at the instruction level

In the glyph rasteriser, `rtt__dev()`:

```c
*dx =  tx * r->sx - r->ox;   /* executed as  -(ox + tx*sx) */
*dy = -ty * r->sy - r->oy;   /* executed as    oy - ty*sy  */
```

Confirmed against the running binary — the logged values matched the wrong
arithmetic to three decimals:

```
u=(610,815) -> d=(-9.659,-35.561)     wrong   ( = -(4+5.66), -28-7.56 )
expected       d=( 1.659, 20.439)
```

Every glyph outline therefore landed at negative `y` and was clipped:
`acc_line calls=24 flat=6 clipped=18 rows=0`. No text could exist.

**No `-f` flag suppresses these patterns.** Checked `-ffp-contract=off`,
`-frounding-math`, `-fsignaling-nans` — codegen unchanged in all three.

### Fix

`qnx-65-sdp-docker/binutils/build.sh` + a `binutils-build` Dockerfile stage:
build **binutils 2.38 `as`** for `arm-unknown-nto-qnx6.5.0eabi` and make it the
default assembler. 2.19 stays reachable as `…-as-2.19` for A/B.

Only `as` is replaced — `ld` and the rest stay at 2.19, because linking was
never implicated and keeping the link step byte-identical limits the blast
radius.

### Verification

Six reference encodings match hand-derived values byte for byte, including a
predicated form and a double-precision one. Then a **full census** of the
frontend's assembly: every VFP mnemonic GCC wrote, compared against what came
out of the assembler.

```
mismatches with gas 2.19:  vmls.f32 37, vmls.f64 16, vmlsmi.f32 1,
                           vnmla.f32 1, vnmls.f32 9, vnmls.f64 3
mismatches with gas 2.38:  none
```

The one remaining difference is benign: `vmov.f32 sN, rN` decodes without the
`.f32` suffix.

### Consequence for the cores

The cores were built by the same broken assembler and nobody had checked them.
`-B/opt/tools/gas-compat/bin` is hardcoded in the vendored makefiles of
`mupen64plus_next` and `pcsx_rearmed`, and that shim called `as-2.19` directly.
All four cores were rebuilt.

The `gas-compat` shim is now vestigial; it was left in place but **repointed at
the new assembler**, since leaving it aimed at 2.19 would silently reintroduce
the bug for anyone still passing `-B`.

---

## 2. libstdc++ interposes libm and its math stubs recurse into themselves

### Symptom

`SIGSEGV` at `libstdc++.so.6@sqrtf+0x1c`, faulting address `0005fff8` — a stack
guard page. Also long hangs.

### Root cause

The frontend was linked with `$(CXX)`. The g++ driver appends
`-lstdc++ -lm -lc`, putting **libstdc++ ahead of libm** in `DT_NEEDED`. QNX
6.5's libstdc++ exports its own `sqrtf`/`powf`, so those bind there instead of
libm — and their slow path branches through the PLT straight back to
themselves:

```asm
00069e14 <sqrtf>:
   vsqrt.f32 s15, s15
   vcmp.f32  s15, s15          ; NaN?
   bne  69e30
   69e30:  push {r3, lr}
   69e34:  bl 4b344 <sqrtf@plt> ; ← itself
```

Unbounded self-recursion; the stack dies. Triggers only on a negative or NaN
argument, which is why it read as an intermittent fault.

**This also explains the note in `Makefile.griffin` blaming a `powf()` hang on
NEON during the 2026-08-02 bring-up.** That diagnosis was wrong: `powf` is
exported and recurses identically. `-mfpu=vfpv3-d16` helped by accident, by
changing codegen so the slow path stopped being reached. The stale comment has
been corrected in place; the flag was kept, because the frontend has never been
validated with NEON enabled and that is a separate, testable change.

### Fix

`LINK = $(CC)` for the QNX platform. The griffin blob is pure C here
(`HAVE_GRIFFIN_CPP := 0`), so libstdc++ buys nothing. Result: libstdc++ leaves
`DT_NEEDED` entirely, `libm.so.2` becomes first, `sqrtf` resolves to libm.

### ROOT CAUSE, found later the same day

The interposition above is only half of it. **Our own GCC 8.5 builds
`libstdc++-v3/src/c++98/math_stubs_float.cc`**, whose `powf` calls `pow(x, y)`
on floats — which C++ overload resolution sends straight back to `powf`. Every
libstdc++ this toolchain produces carries that recursion, not just the one QNX
shipped. See section 9.

### Note on `src/ra_math.c`

A previous session hit the same interposition and worked around it by defining
`expf/exp/powf/pow/ceilf/floorf` in the executable and forwarding to libm via
`dlsym`. **`sqrtf` was not in that list** — which is exactly why the crash
landed there.

With the link fixed the shim is redundant. It was left alone for now, but its
failure mode is a landmine worth removing: if `dlsym` fails it silently
`return x`, i.e. `powf(a,b)` returns `a`.

---

## 3. PPSSPP's ARM JIT had no I-cache maintenance on QNX

### Symptom

Black screen a few seconds after starting a game, then
`SIGSEGV ip=048000d8` — an address with **no module name**, i.e. execution
inside anonymous memory: JIT-generated code.

### Root cause

`Common/ArmEmitter.cpp`:

```c
#elif PPSSPP_ARCH(ARM)
    __builtin___clear_cache(start, end);   /* a NO-OP on QNX 6.5 */
```

The project already documents this exact trap in
`docs/legacy/qnx-arm-jit-icache-recipe.md`, and gpSP, Mupen64Plus-Next and
PCSX-ReARMed all carry the fix. **PPSSPP did not** — it was added later and the
recipe never reached it.

### Fix

```c
#if defined(__QNXNTO__)
    msync(start, (size_t)(end - start),
          MS_SYNC | MS_CACHE_ONLY | MS_INVALIDATE_ICACHE);
#endif
```

`__QNXNTO__` is compiler-predefined, unlike `__BLACKBERRY_QNX__` which depends
on build flags.

Also fixed a genuine upstream syntax error in
`libretro/libretro-common/memmap/memmap.c`: a missing `|` before
`MS_CACHE_ONLY` under `#ifdef __QNX__`, plus the missing
`MS_INVALIDATE_ICACHE`. That translation unit is not compiled into this core,
which is why it never surfaced.

---

## 4. Frame pacing: vsync was implemented but switched off

`video_vsync = "false"` while `src/gfx/drivers_context/qnx_ctx.c` — **our own
driver**, 37 KB against upstream's 11 KB, with none of this machinery in
upstream — implements both a hardware `screen_wait_vsync()` path and a software
deadline fallback at `QNX_SOFTWARE_REFRESH_NS = 16666667` (60 Hz), explicitly
fail-safe:

> *"This also keeps a `video_vsync=true` configuration fail-safe: the runloop
> must never become unlimited merely because Screen rejected the display
> handle."*

### Measured effect

```
before:  setup=33   draw=92  post=22602  swap=1267   total=24054 us
after:   setup=310  draw=58  post=25     swap=9400   total=9800  us
```

`post` is `before_swap − after_chain`. **22.6 ms per frame of idle spinning was
being burned there.** With vsync on it is gone and the time sits in a proper
`swap` wait. Frontend frame cost fell from 24 ms to 9.8 ms, of which 9.4 ms is
waiting — the frontend has ample headroom.

---

## 5. Audio pacing

### What throttles the emulator

Comparing cores through the QSA counters:

| core | production | worker | write_max |
|---|---|---|---|
| gpSP | 626 | 610 | 30.2 ms (blocking) |
| PCSX | 628 | 612 | 30.2 ms (blocking) |
| PPSSPP (before) | 584 | **657** | 1.0 ms (never blocks) |

Audio writes block only once the buffer fills. While PPSSPP under-produced, the
buffer never filled, so **the CPU itself was the rate limiter**.

### Why frameskip was wrong

Enabling `auto_frameskip` removed that limiter while nothing replaced it: the
game ran faster than real time and audio pitched up. Frameskip is not usable on
this port as a cure for underruns. Reverted.

The correct fix was vsync (section 4) — a hard 60 Hz cap independent of buffer
state. After it:

```
production=590  worker=586  conceal=0  reserve=15/16
```

Zero underruns in steady state.

### What still tears

Level loads: `idle_max` 593–827 ms. The reserve is 16 fragments × 48 ms =
768 ms, which would cover it, but it refills at only ~4 fragments per window
and each load empties it. Not fixable by enlarging the buffer.

**CSO was evaluated and rejected**: measured random-read cost on the card is
~6.6 ms per 64 KB (after subtracting ~36 ms of process-spawn overhead that
corrupted the first measurement). Compression cuts bytes, not seeks, and the
cost here is per-seek.

---

## 6. A sampling profiler for this target

`tools/qnx-profiler/`

**`tracelogger` was tried first and rebooted the unit.** The instrumented
kernel `procnto-smp-instr` is running and the SDP tool works, but the load
spike trips the HU watchdog. Free memory was 890 MB of 2048, so it was not
memory pressure. Do not use it on this box.

Instead: `ra_prof.c` samples `DCMD_PROC_TIDSTATUS` per thread — one devctl per
thread per tick, ~1000/s at the default 100 Hz. Two 30-second captures caused
no disturbance.

Emits `ms tid state ip blocked_ms`, plus `MAP` lines naming every mapped object
via `DCMD_PROC_MAPDEBUG` — without names, every address outside the frontend
collapses onto its last symbol (`_fini` showed a bogus 91.8% before that was
added).

- `ra_prof_report.py` — thread states, worst blocks, flat profile
- `ra_prof_hot.py` — per-object hot functions, resolved at `ip − object_base`

**Honest limitation: this is a flat profile, not a flamegraph.**
`TIDSTATUS` yields one instruction pointer per thread, not a call stack. Real
flamegraphs need target-side unwinding.

Symbols live in `tools/qnx-profiler/symbols/` (gitignored — multi-MB build
by-products). Both were verified: the stripped form of each unstripped artefact
is **byte-identical** to what runs on the unit, otherwise the addresses would
lie.

---

## 7. Measured performance profile — God of War: Ghost of Sparta

Three independent 30-second gameplay captures:

| | ra4 | ra5 | ra6 |
|---|---|---|---|
| JIT code (anon exec) | 32.0% | 34.0% | 31.0% |
| `libc.so.3` (memcpy) | 22.0% | 22.1% | 24.0% |
| `ppsspp_libretro.so` | 20.4% | 23.2% | 23.7% |
| `OpenGLES20.so` | 23.8% | 18.5% | 19.2% |
| `retroarch` | 1.1% | 1.1% | 1.1% |

Inside the core:

| function | ra4 | ra5 | ra6 |
|---|---|---|---|
| `FramebufferManagerCommon::NotifyBlockTransferAfter` | 13.1% | 14.0% | 10.7% |
| `SasInstance::MixVoice` | 5.0% | 9.7% | 8.1% |
| `TextureCacheCommon::LoadClut` | 8.2% | 7.1% | 6.7% |
| `GPUgstate::FastLoadBoneMatrix` | 6.1% | 5.7% | 5.2% |
| `GPUCommonHW::FastRunLoop` | 5.6% | 5.3% | 5.4% |

### Reading

- **The frontend is 1.1% and waits 9.4 ms per frame.** Nothing left to win in
  RetroArch itself.
- No dominant hotspot. The largest single core function is ~14% of 23%, i.e.
  ~3% of total. Work is spread evenly — the signature of a machine doing
  legitimate work at its ceiling, not wasting time.
- `libc` at 22% and `NotifyBlockTransferAfter` + `LoadClut` are the same story:
  block transfers and palette loads bottom out in `memcpy`.
- `MixVoice` at 5–10% means PSP audio mixing runs on the same CPU. Audio
  crackle and frame drops are therefore **one symptom, not two**.

### Options tried, with outcomes

| change | outcome |
|---|---|
| `auto_frameskip` | broke pacing — reverted |
| `skip_buffer_effects`, `skip_gpu_readbacks` | verified to bind to real config flags; framebuffer bookkeeping remains regardless |
| `spline_quality=Low`, anisotropy off | within noise |
| software skinning → GPU | no effect — reverted |
| **`video_vsync=true`** | **22.6 ms/frame of idle spin removed** |

`FastLoadBoneMatrix` did not move when skinning was pushed to the GPU because
it is the PSP's bone-matrix *upload* command, not the skinning maths.

`ppsspp_cache_iso` is all-or-nothing (`bCacheFullIsoInRam`) and the ISO is
1.75 GB — unusable here.

---

## 8. QNX CPU detection was disabling PPSSPP's fast paths

The first hardware profile also exposed a target-port bug in
`Common/ArmCPUDetect.cpp`. QNX is not handled by PPSSPP's Linux `/proc/cpuinfo`
path, so it fell into the generic fallback and reported:

- one CPU core;
- no NEON or VFPv3/VFPv4;
- no ARM hardware divide.

That result overrode what the core had been compiled to support. In
particular, the ARM JIT selected scalar VFPU and software-divide paths,
`ParallelLoop` stayed serial, and `DefaultSasThread()` kept the measured
5–10% `SasInstance::MixVoice` load on the emulation thread.

The QNX branch now reads `num_cpu`, `ARM_CPU_FLAG_NEON` and
`ARM_CPU_FLAG_IDIV` from the syspage. On the fixed APQ8064 target this selects
four Krait cores, NEON, VFPv4 and IDIV. The core is also scheduled for the
closest GCC 8.5 model (`-mtune=cortex-a15`) and built with `neon-vfpv4`, while
remaining on `-O2`: an `-O3` experiment crashed inside
`ldqnx.so.2::__gnu_Unwind_Find_exidx` and is not production-safe yet.

That unwind failure was subsequently isolated from the CPU fast paths.

> **Superseded — `-static-libstdc++` was tried on hardware and REVERTED.**
> An earlier revision of this section stated the core links `-static-libstdc++`
> and has no `libstdc++.so.6` dependency. That is not the shipped state: the
> build hung on Run with **100% of samples in `powf`**, because static linking
> pulls in libstdc++'s own recursive math stubs (section 2). The core links
> libstdc++ dynamically again. Section 9 has the measurement and the actual
> root cause.

`retro_init()` now logs the full detected feature set. The next hardware run
must contain both of these before performance numbers are accepted:

```
Host CPU: ARMv7 (QNX, ... MHz), 4 cores, ..., VFPv4, NEON, IDIVa, IDIVt
ThreadManager::Init(compute threads: 4, all: 8)
```

The repository factory config and launcher migration now also preserve the
already-measured `video_vsync=true` fix on both fresh and existing SD cards.
Previously all packaged configs still said `false`, so a rebuild/reinstall
could silently restore the bad pacing despite section 4's successful test.

---

## 9. Two regressions chasing the exception crash, and the root cause they exposed

Recorded because the mistakes are the instructive part.

### `-O3` — reverted, and innocent

Three things went into one build: `-mtune=cortex-a9` -> `cortex-a15` (the unit
is a Krait; GCC 8.5 has no `-mtune=krait`, and PCSX-ReARMed already tunes for
a15), `-O2` -> `-O3`, and the section-8 CPU detection. The core then started
crashing, and **nothing could be attributed** — so `-O3` went first as the
likeliest to expose latent UB.

It was innocent: a clean run worked with it, and only save/load state crashed.
`-mtune=cortex-a15` is kept, `-O3` is still reverted and untested.

### `-static-libstdc++` — repaired and verified

Save and load state both died in
`ldqnx.so.2@__gnu_Unwind_Find_exidx+0x7c`, `ref=0x8`.

`gcc/README.md` documents a ceiling on the `--target2=rel` fix: *"catching by a
typeinfo imported from another `.so` can't GOT-indirect under `rel`; static
libstdc++ / same-module catches are unaffected"*. The core does import its
typeinfos from `libstdc++.so.6`, so this looked like the documented cure.

**Applying it without verifying the mechanism was the error.** The crash is in
`Find_exidx` — the unwind *table lookup* — while the documented ceiling concerns
*typeinfo matching*. Different phases.

Result: RetroArch hung on Run, and the profiler from section 6 found it in one
8-second capture:

```
100.0%  557 samples  ->  powf   inside ppsspp_libretro.so
```

Static linking had pulled libstdc++'s math stubs into the core — the section-2
recursion, reintroduced by hand.

### What the failed follow-up revealed

Shadowing the stubs with a small QNX shim would not link:

```
libstdc++-v3/src/c++98/math_stubs_float.cc:188: multiple definition of `powf'
```

That error is the valuable part. libstdc++ compiles `math_stubs_float.cc` only
when configure believes the target libm lacks the float entry points — but this
libm has all of them (`expf exp powf pow ceilf floorf sqrtf`, verified in
`/armle/lib/libm.so.2`).

**The correct fix is therefore at the toolchain level: do not install the
compatibility stubs when the target libm already supplies them.** Inspection of
the installed archive found two copies each of `math_stubs_float.o` and
`math_stubs_long_double.o`; the old QNX `ar d` removes only one matching member
per invocation.

`qnx-65-sdp-docker/gcc/build.sh` now deletes every copy in a loop, runs
`ranlib`, and fails the toolchain build if the resulting archive still defines
`ceilf`, `expf`, `floorf`, `powf`, or `sqrtf`. The RetroArch build also creates
and validates a temporary sanitized archive, so an older installed Docker
image cannot silently reintroduce the defect.

PPSSPP now links this repaired C++ runtime statically. Offline ELF validation
of the resulting 14 MB stripped core shows exactly:

```
NEEDED: libGLESv2.so.1 libEGL.so.1 libm.so.2 libc.so.3
```

There is no `libstdc++.so.6` dependency, while `ceilf`, `floorf`, and `powf`
remain undefined imports and therefore resolve to `libm`, not recursive code in
the core. A small target smoke executable also linked `std::vector`,
`std::string`, throw/catch, and float libm calls with the same dependency set.

This repairs the previously invalid static-runtime experiment. The MIB2Q/QNX
QEMU gate below now exercises a real `throw`/`catch` through the target loader,
so the C++ exception/EHABI path is verified offline. Save/Load still needs its
own hardware test because it also covers PPSSPP state logic, file I/O and the
full RetroArch/core lifecycle.

All repository entry points now use one runtime preparation gate:
`tools/qnx-qemu/prepare-static-cxx-runtime.sh`. It copies the toolchain archive,
removes every bad math-stub member, rebuilds its index and rejects the result if
it still exports QNX libm functions. Both `build.sh` and the PPSSPP/QEMU test
runner call this script, preventing an older Docker image from silently
reintroducing the recursive `powf` failure.

The other QNX C++ defect was a libstdc++/pthread lifetime mismatch. With the
default GCC 8 gthread path, `std::mutex` used a static initializer and a no-op
destructor. QNX synchronization objects are associated with their address, so
reusing the same heap address later produced `EINVAL` in `condition_variable`
and `WaitableCounter`. Every PPSSPP C++ translation unit now force-includes
`QnxCompat.h`, which defines `_GTHREAD_USE_MUTEX_INIT_FUNC` before libstdc++ and
therefore pairs explicit `pthread_mutex_init()`/`pthread_mutex_destroy()` for
every lifetime. This is a global PPSSPP fix, not a special-case replacement of
`WaitableCounter`.

### State prepared while the unit is offline

The new core keeps `-mtune=cortex-a15`, `-O2`, CPU detection, and the repaired
static C++ runtime. SaveState entry points now validate their buffers, balance
emulation-thread pause/resume even after exceptions, and log failures. A mere
`retro_serialize_size()` capability query no longer leaves the emulation thread
paused. Lifecycle pause always flushes SRAM, but automatic SaveState is gated
by `RA_QNX_AUTO_SAVE_STATE` and defaults to `0` until one manual Save+Load cycle
passes on hardware.

---

## 10. Low-overhead PPSSPP performance telemetry

The generic RetroArch `fps_show` / `statistics_show` overlay is not suitable
for this target: it previously reduced PPSSPP from 58–60 FPS by itself. The
core therefore has a dedicated `Performance Statistics` option under PPSSPP's
Core Options -> System menu:

- `Disabled` — no timer calls or telemetry work in the frame path;
- `Log only` — one compact line per second directly in
  `/tmp/ppsspp_perf.log` (or `$RA_PPSSPP_PERF_LOG`);
- `On-screen + log` — the same sample as one replace-in-place three-line
  RetroArch status, plus the direct file. It does not grow the notification
  queue.

The current diagnostic image defaults to `Log only`. The launcher migrates
only the exact old factory value `disabled`, preserving an explicit user
selection. Direct tmpfs output is intentional: neither the frontend OSD nor
timestamped log routing was reliable enough for the first capture.

The status looks like this:

```
PPSSPP 96.2% | VPS 57.7 | game 28.9/30.0 FPS | host 57.7
core 13.4/22.1 ms | swap 3.8/8.0 ms | audio 96.1% accepted 100.0% (0 short)
GPU draw 1840/s | xfer 58/s | blocking readback 0/s
```

Interpretation:

- `PPSSPP` / `VPS` is the important number: 100% / 59.94 VPS means the PSP
  clock is keeping up. A 30 FPS game is still full speed when it says roughly
  `30/30 FPS` and `100%`.
- `game actual/target FPS` distinguishes genuine game frame rate from the
  frontend's duplicated 60 Hz output. `host` is how often RetroArch completed
  `retro_run`.
- `core average/max` covers the emulation-thread wait and GLES command
  generation. Repeated maxima above 16.7 ms together with speed below 100%
  point at CPU/core work; use the sampling profiler to name the function.
- `swap average/max` covers the frontend swap/vsync boundary. High swap with
  100% speed is normal pacing; high swap accompanied by falling speed suggests
  GPU or presentation pressure.
- `audio` is frames produced relative to 44.1 kHz. It should track emulation
  speed. `accepted` below 100% or non-zero `short` means the frontend rejected
  samples; `audio` below 100% with accepted 100% means PPSSPP itself is running
  too slowly, which explains pitched/crackling sound.
- `xfer` and `blocking readback` expose framebuffer traffic. They are especially
  relevant to the measured `NotifyBlockTransferAfter`/`memcpy` hotspot.

Use `On-screen + log` only long enough to reproduce a bad scene, then switch
back to `Disabled` for maximum performance. `Log only` is preferable for a
clean A/B capture.

The QSA driver writes its independent transport window to
`/tmp/qsa_perf.log` (or `$RA_QNX_AUDIO_PERF_LOG`). Read the two streams
together:

- PPSSPP audio below 100%, frontend accepted 100%, and an empty queued tail
  means the core did not emulate fast enough; QSA cannot invent the missing
  samples.
- PPSSPP audio near 100%, but QSA reserve falling and concealment increasing,
  means a worker/scheduling/device underrun.
- frontend accepted below 100% or a growing queued tail means backpressure or
  an overrun between the core and QSA.
- core maxima above 16.7 ms point at emulation/GLES command generation; swap
  maxima near one refresh interval are normal, while repeated values above the
  25 ms hardware-vsync timeout point at presentation trouble.

Generic RetroArch dynamic rate control is intentionally kept at ±0.5%: it
corrects clock drift, not a core running 5–10% slow. Increasing it enough to
mask that deficit would audibly change pitch.

### First hardware capture

Start PPSSPP, let the intro and then gameplay run for at least 20 seconds each,
and copy these files before restarting RetroArch (they live in `/tmp`):

```
/tmp/ppsspp_perf.log
/tmp/qsa_perf.log
/tmp/ra_display.log
```

Then perform one manual SaveState and one LoadState while also preserving the
timestamped RetroArch/crash log. Do not enable automatic lifecycle SaveState
until both operations pass.

---

## 11. Local MIB2Q/QNX ARM QEMU harness

The minimal useful subset of the MIB2Q QEMU work now lives in
`tools/qnx-qemu/`; the 11 GB Rust HMI workspace is no longer required for
runtime and loader tests. The imported local runtime includes the patched QEMU
9.1 engine, real MIB2Q loader/libc/libm, serial bootstrap tools, extracted
Screen/EGL/GLES files, and the patched Cortex-A15 QNX startup BSP. Hashes lock
the engine, firmware loader and source patch.

The source recipe is also self-contained. `tools/qnx-qemu/build-qemu.sh`
fetches the pinned upstream commit, applies the imported patch, rebuilds the
host OpenGL command renderer, and leaves the candidate binary in the ignored
build directory without replacing the known runtime. The recipe explicitly
removes Homebrew GNU binutils from `PATH`: its `ar` produces Mach-O archives
that Apple `ld` rejects in this build.

The first autonomous guest test found that launching `pdksh -c` from the IFS
startup script never returned even after its child exited. The harness now uses
a QNX-native `spawnv(P_WAIT)` wrapper, decodes QNX wait status into an exit code
or `128 + signal`, and stops QEMU as soon as the status reaches the serial log.
A test takes about two seconds instead of waiting for the timeout. It boots
`procnto-smp` with four Cortex-A15 vCPUs to match MIB2Q's four Krait cores; this
does not make QEMU a cycle-accurate APQ8064 performance model.

Verified against both the imported engine and a clean local rebuild:

```
dynamic dependencies: libc.so.3 libm.so.2
CXX_RUNTIME exception=ok math=70.000 sync-reuse=ok future=ok
__QNX_TEST_RC__=0
QNX guest test: PASS
```

Run the complete compile/dependency/boot check with:

```sh
tools/qnx-qemu/test-cxx-runtime.sh
```

This closes the offline loader/EHABI and basic synchronization uncertainty:
STL construction/destruction, `throw`/`catch`, affected float libm calls, 32
destroy/recreate cycles of `std::mutex` + `std::condition_variable` at the same
address, and `std::promise`/`future` return normally under the real MIB2Q
userspace loader. It does **not** validate APQ8064 speed, Adreno, QSA buffering,
SD latency, or physical `screen_wait_vsync()`. Those remain hardware
measurements.

The actual RetroArch and PPSSPP suites are now wired through
`tools/qnx-tests/`:

```sh
tools/qnx-tests/run-libretro-common.sh
tools/qnx-tests/run-ppsspp.sh
```

Verified QEMU results on 2026-08-22:

- Combined PPSSPP result: **24/24 PASS** after a clean rebuild.
- RetroArch/libretro-common: **133/133 PASS** across stdstring, utils, hashes,
  linked list, generic queue and rpng.
- PPSSPP primary QNX matrix: **13/13 PASS**. `VertexJit` had exposed strict QNX
  ARM alignment faults: the generated decoder used a 32-bit NEON load for a
  packed three-byte S8 vector, then another for an S16 vector aligned to only
  two bytes. QNX-specific code generation now performs exact byte/halfword
  loads and assembles the NEON input without reading past or misaligning it.
- PPSSPP additional platform coverage: **11/11 PASS**. `ThreadManager` now
  completes its full 9-thread, 40,000-iteration stress path; this covers
  `WaitableCounter`, `Promise`, `Mailbox`, and `LimitedWaitable` using the
  repaired standard mutex lifecycle.
- Vulkan/glslang `ShaderGenerators` is unsupported by design in the QNX
  GLES2-only build and is not falsely counted as a pass.

The main JIT test executes generated ARM code successfully. QEMU icount and host
scheduling distort the JIT/interpreter wall-time ratio (observed values cross
both sides of 1.0), so the QNX test still prints the ratio but does not turn it
into a correctness assertion. Treat it only as proof that the generated-code
path runs, never as an HU speed estimate. A full graphical frontend still
requires the larger Screen/EGL/input service IFS.

The other vendored cores do not contain equivalent self-contained target unit
suites. PCSX-ReARMed's `plugins/gpulib/Makefile.test` is a replay tool and needs
external GPU state plus command-list dumps, neither of which is vendored. gpSP's
`tests/` compares code-generator bytes using external AArch64/MIPS assemblers;
it is a host generator test, not an ARM/QNX runtime test. Mupen64Plus-Next's
regression runner needs copyrighted N64 ROMs and reference screenshots. These
were therefore not replaced with meaningless empty inputs or reported as QEMU
passes. Captured PCSX GPU dumps and user-supplied test ROMs can be mounted into
a larger test IFS later.

---

## 12. Open

- **OEM entertainment connection 20 sticks — DORMANT, not fixed.**
  Three consecutive activations were refused in 17–43 ms (`pause conn=20`
  instead of `start`); the bridge timed out and the HMI hook SIGTERMed
  RetroArch, which looked like "RetroArch closes by itself after a couple of
  seconds". Release is correct on our side (`released connection=20`, focus and
  connection 9 restored) — the state sticks inside the OEM audio manager. The
  trigger was the OEM pausing connection 20 *mid-session*, before RetroArch
  exited.

  **A reboot cleared it and it has not returned:**

  ```
  2112847  pause conn=20  -> aborted     ]
  2283651  pause conn=20  -> aborted     ]  before the reboot
  2409942  pause conn=20  -> aborted     ]
  --------------------------- reboot, clock restarts ---------------------
   386877  start conn=20  -> ACTIVE      ]
   519802  start conn=20  -> ACTIVE      ]  after: four in a row,
  1127843  start conn=20  -> ACTIVE      ]  no refusals
  1435812  start conn=20  -> ACTIVE      ]
  ```

  So it is intermittent and needs a specific coincidence — something taking
  audio away mid-game. Not worth hunting until it reproduces: without a
  reproduction there is nothing to fix, and the logs after the fact only show
  the aftermath. If it reappears, capture `ra_audio.log` and `ra_hook.log` at
  that moment.

  Worth adding to the bridge regardless: on a `pause` during acquisition, do
  not simply wait out the 6 s for a `STARTED` that will never come — release
  and retry once.
- Whether the port is at fault for heavy-title performance at all is still
  unproven: only God of War has been tested, one of the heaviest titles on the
  platform. A lighter game would separate "port issue" from "hardware limit".
- **Save/load state hardware validation is pending.** The recursive static
  runtime is repaired and SaveState boundaries are hardened, but the unit was
  offline after the build. Keep `RA_QNX_AUTO_SAVE_STATE=0`; capture the crash
  log if either manual operation still reaches
  `ldqnx.so.2@__gnu_Unwind_Find_exidx`.
- **`AsyncIOManager::WaitResult` burns 22.5% of core CPU during cutscenes** while
  in RUNNING state — it is waiting *actively*. Prime suspect for the wheezing in
  video sequences.
- **`DisplayProperties::GetDeviceOrientation` at 10.2%** in the same capture. A
  trivial getter cannot cost that; it is being called from a hot loop.
