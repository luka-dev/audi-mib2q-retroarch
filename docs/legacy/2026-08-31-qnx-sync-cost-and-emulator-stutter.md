# Thread synchronisation is a kernel call on QNX 6.5 — and what it cost us

**Target:** MHI2Q head unit, QNX Neutrino 6.5.0, APQ8064 Krait 4×1512 MHz, Adreno 320.
**Session:** 2026-08-31. Everything below was measured on the device, not inferred.

This note covers one platform-wide finding that changes how you write threaded
code for this target, two real upstream bugs fixed along the way, and the
diagnostic recipes that got us there. Companion to
[`qnx-arm-jit-icache-recipe.md`](qnx-arm-jit-icache-recipe.md).

---

## 1. The headline: `pthread_cond_signal` always enters the kernel

### Symptom

Mupen64Plus-Next ran at roughly real time but never *above* it. Audio told the
story precisely — the QSA driver's software queue was empty in **every** telemetry
window:

```
reserve=0/16  production=387  worker=404  conceal=17  idle_max=40 ms
reserve=0/16  production=387  worker=401  conceal=14  idle_max=35 ms
reserve=0/16  production=381  worker=382  conceal= 0  idle_max=38 ms
```

`producer_waits=0` across whole sessions: the emulator thread *never once* had to
wait for audio. The audio worker was always waiting for the emulator instead.
With no headroom the cushion could never be rebuilt, so every 35–47 ms hitch
became an audible concealment seam.

### Root cause

Sampling the process (`tools/qnx-profiler`) showed 63.5% of on-CPU time inside
`libc.so.3`, concentrated in ~450 bytes. Disassembling those bytes showed they
are not a function at all but the QNX **kernel call stub table**:

```
e92d4000  push {lr}
e3a0c053  mov  r12, #0x53      <- kernel call number
ef000051  svc  #0x51
e8bd8000  pop  {pc}
```

Cross-referencing `r12` with `kercalls.h` from the SDP:

| call | name | share of on-CPU |
|------|------|-----------------|
| 0x53 | `__KER_SYNC_CONDVAR_SIGNAL` | **59.0%** |
| 0x50 | `__KER_SYNC_MUTEX_LOCK` | 24.1% |
| 0x54 | `__KER_SYNC_SEM_POST` | 6.1% |
| 0x51 | `__KER_SYNC_MUTEX_UNLOCK` | 2.0% |

About **54% of all CPU time was thread synchronisation**, not emulation. 300 of
the 357 `CONDVAR_SIGNAL` samples were on the emulator thread.

The reason is platform-specific and easy to miss when porting:

> On Linux an **uncontended** `pthread_cond_signal` is resolved in userspace and
> costs essentially nothing. On QNX it is *always* the `SyncCondvarSignal`
> kernel call — even when no thread is waiting.

GLideN64's threaded renderer turns one emulator GL call into one
`BlockingQueue::push`, and `push` called `notify_one()` unconditionally. That is
one syscall per GL call, tens of thousands per frame.

`SyncMutexLock` appearing at all is the same class of signal: QNX mutexes are
userspace-fast when uncontended, so kernel-call samples there mean **real
contention**, not just lock traffic.

### Fix

Count blocked consumers and signal only when one exists.
`GLideN64/src/Graphics/OpenGLContext/ThreadedOpenGl/BlockingQueue.h`:

```cpp
void push(T const& value)
{
    bool wake;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.push_front(value);
        wake = m_waiters != 0;
    }
    if (wake)
        m_condition.notify_one();
}
```

with `pop()`/`tryPop()` bracketing their wait in a scope guard that increments
and decrements `m_waiters`.

**Why this cannot lose a wakeup:** a consumer only blocks after testing the
predicate *while holding* `m_mutex` (that is what `wait(lock, pred)` does), and
the producer both pushes and reads `m_waiters` under that same mutex. Either the
consumer sees the new item and never blocks, or it was already counted before
the producer looked. The one benign race — a consumer counted but about to
return without blocking — costs a single spurious signal.

The same gate was applied to `RingBufferPool::removeBufferFromPool`
(`notify_all()` once per draw).

### Measured result

| | before | after |
|---|---|---|
| `CONDVAR_SIGNAL` share of on-CPU | 59.0% | **6.7%** |
| all sync calls | ~55% | ~40% |
| QSA `reserve` | `0/16` in every window | `14–15/16` in most |
| `conceal` in gameplay windows | 4–17 | **0 in 13 of 18** |
| `producer_waits` per session | 0 | 4318 |

`producer_waits` is the clearest signal: the emulator now blocks on audio, which
means it finally runs *ahead* of real time and the cushion holds.

### Generalising

Anywhere on this target that signals a condition variable per unit of work —
command queues, ring buffers, producer/consumer pipelines — is paying a syscall
per unit. Audit for unconditional `notify_one()`/`notify_all()` on hot paths.
`std::condition_variable`, `pthread_cond_signal` and
`condition_variable_any` are all affected.

**Remaining known cost:** mutex contention on the same GL command queue is now
the top consumer (24.1% of on-CPU). Reducing it needs command batching or a
lock-free queue, not a smaller change.

---

## 2. PCSX-ReARMed: the ARM dynarec hands a clobbered `r0` to the GTE

### Symptom

Hard SIGSEGV entering gameplay (Need for Speed III), always at the same
instruction:

```
ip=7886ad98 (pcsx_rearmed_libretro.so@+0x49de4)  r0=00000b80  ref=00000be4
```

### Diagnosis

Core dumps land in `/mnt/ota/system/core/`. Registers from the faulting thread:

```
pc = memcpy-like prologue,  r0 = 0xb80 (garbage),  lr = 0x790ec394
```

`lr` pointed into the core's own BSS — i.e. the caller was JIT-generated code.
Rebuilding the core unstripped (byte-identical to the deployed strip) resolved
the address to `gteMACtoRGB_nf + 0x4`, whose first instruction is
`ldr r1, [r0, #0x64]`; `0xb80 + 0x64 = 0xbe4` matches `ref` exactly.

Root cause in `libpcsxcore/new_dynarec/assem_arm.c`:

```c
emit_far_call(func);
if (need_flags || need_ir) {
    // func is C code and trashes r0
    emit_addimm(FP, ...CP2D..., 0);      // r0 restored ONLY here
    c2op_call_MACtoIR(lm, need_flags, fm_load);
}
emit_far_call(need_flags ? gteMACtoRGB : gteMACtoRGB_nf);   // but r0 needed always
```

`func` is a C function from `gte.c` (`gteDPCS_part_*`, `gteGPF_part_*`, …) and
under the AAPCS is free to clobber `r0`. Verified in the binary: the small
`gteGPF_part_noshift` keeps the pointer in `r0`, but `gteDPCS_part_shift` moves
it to `r10` and uses `r0` as scratch, returning a packed colour value. When both
the GTE flag register and all three IR registers are dead at that point, the
branch is skipped and `gteMACtoRGB_nf` is called with that colour as its `this`
pointer.

**Not a toolchain regression.** The same hazard signature (`str r0,[r10,#0x6c]`)
appears twice in both the GCC 4.9 and GCC 8.5 builds. Whether it fires depends
on the liveness of the *emulated* game's code, so it is game- and
scene-dependent — which is why PS1 "used to work".

### Fix

Hoist the `r0` reload out of the conditional. The `gteMACtoIR_*` helpers are
assembly and preserve `r0`, so the reorder is safe.

---

## 3. GLideN64: vertex client-array over-read in the threaded renderer

### Symptom

Intermittent SIGSEGV in `memcpy` called from
`opengl::RingBufferPool::createPoolBuffer`, faulting on the **source** pointer
exactly at a page boundary (`ref=23f55000`, len `0x370`).

`memcpy` copies forward, so the register source is the advanced position — the
source buffer was simply shorter than the declared length.

### Root cause

`opengl_Wrapper.cpp` sized the vertex copy by guesswork:

```cpp
createPoolBuffer(ptr, (count + 1)          * getStride());   // glDrawArrays
createPoolBuffer(ptr, (maxElementIndex + 1) * getStride());  // glDrawElements
```

Three faults in one expression: the `+1` deliberately reads one whole stride
past the last vertex; `getStride()` takes the stride of the *first* enabled
attribute and assumes all share it; and `glDrawArrays`'s `first` is ignored
entirely.

Arithmetic for the observed crash — stride 40, 21 vertices:
`(21+1)*40 = 880`, the exact faulting length. The correct span is
`32 + 20*40 + 8 = 840`. The overshoot is one stride, which normally lands in
heap slack and passes unnoticed; when the array ends on a page boundary it
faults. Hence "crashes rarely".

### Fix

`GlVertexAttribPointerManager::getAttribsSpan(lastIndex)` derives the exact span
from the attributes themselves — for each enabled attribute
`offset + lastIndex * stride + elementSize`, take the maximum — and both draw
sites use it. Correct for split arrays and differing strides, and `first` is
included. Clamped to the 2 MB `m_attribsData` consumer vectors.

The consumer rebases pointers as `attrPtr - smallestPtr`, so only the *length*
may change; the copy must still start at `getSmallestPtr()`.

---

## 4. Audio pacing on this target

### The QSA DRC control signal

`src/audio/drivers/qnx_qsa.c` reports free space to RetroArch's dynamic rate
control. RetroArch treats `buffer_size/2` as neutral, so **whatever you report
at a full queue is where the reserve parks**.

| formula | equilibrium | problem |
|---|---|---|
| `MIN(cap, local_avail + cap/2)` (original) | queue full | saturates for `local_avail ≥ 8`: half-empty and fully-empty read identically, so DRC pins its correction and cannot modulate |
| `local_avail` | queue **half** full | halves every core's cushion (~256 ms → ~128 ms) |
| `local_avail/2 + cap/2` (current) | queue full | linear across the whole range, no flat zone |

### Frame pacing: vsync vs. blocking audio

`44400c6a` switched `video_vsync` `false → true` (forced by the `ra.sh` v2
migration) for PPSSPP's benefit. That hands pacing to the display.

RetroArch computes the resampler ratio from `video_refresh_rate = 60.000000`,
and never measured the panel (`Does not have enough samples for monitor refresh
rate estimation`). An automotive panel that is not exactly 60.000 Hz therefore
produces a permanent 1–3% audio deficit — small, constant, and unfixable by
`audio_rate_control_delta = 0.005` (which needs ~30 s to rebuild one 16 ms
fragment).

Setting `video_vsync = false` returns pacing to blocking audio writes
(`fastforward_ratio = 0.000000` disables the frame limiter, so audio is the only
clock). PS1 result: `reserve` `0/16 → 15/16`, `conceal` `0` in every window.

**PPSSPP is no longer in the build**, so the migration in `pkg/ra.sh` that forces
`video_vsync=true` is now wrong for a fresh SD card and should be revised.

---

## 5. Diagnostic recipes for this target

**Core dumps** — `/mnt/ota/system/core/<name>.core.gz` (dumper configured with
`-d /mnt/ota/system/core`). Registers and thread states:
`diagnostics/hu-crash-1970-01-01-001049/analysis/qnx_core_notes.py <core>`.
`lr` inside a core's own BSS means the caller was JIT-generated code.

**Symbol resolution** — the cores build reproducibly: rebuilding unstripped and
comparing the strip against the deployed binary (`cmp`) proves the symbols match
before trusting any address. Both PCSX and Mupen came out byte-identical.

**Sampling profiler** — `tools/qnx-profiler/ra_prof <pid> <secs> [hz] [tid]` on
the device, then `ra_prof_report.py <trace> <symbols-dir>` (symbols dir is
**positional**, not a flag; a wrong path is swallowed silently and you get
`0 object(s) with symbols`).

**Decoding libc hotspots** — boot-image objects (`proc/boot/libc.so.3`) have no
symbols, and the `offset` field in `DCMD_PROC_MAPINFO` is an offset into the IFS
image, *not* into the file. Use `ip - map_vaddr` as the library virtual address.
If the bytes look like `push {lr} / mov r12,#N / svc #0x51`, you are looking at a
kernel call stub — decode `N` against `kercalls.h` in the SDP rather than hunting
for a function name.

**Transferring files** — the head unit has no `scp`, `tail`, `wc`, `cksum`, or
`date`, and its `grep` lacks `-o` and `-A`. Stream over ssh stdin
(`ssh host 'cat > /path' < file`) and verify by reading it back through `cksum`
on the host. `/mnt/app` needs `mount -uw` and survives reboot.

**Reading QSA telemetry** — one window is 300 rate-control queries, not a fixed
time, and `reserve`/`free` are instantaneous samples. The meaningful ratio is
`conceal / worker`; `production ≈ worker` always holds once audio paces the
core, so it proves nothing on its own. `producer_waits` is the honest indicator
of whether the emulator has headroom.
