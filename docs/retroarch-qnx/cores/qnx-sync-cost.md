---
title: Thread sync is a kernel call on QNX - and two dynarec bugs
tags: [cores, perf, qnx]
status: verified-hardware
sources:
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md
  - git 4d816dab
  - cores-src/mupen64plus_next/GLideN64/src/Graphics/OpenGLContext/ThreadedOpenGl/{BlockingQueue.h,RingBufferPool.cpp,opengl_Wrapper.cpp}
  - cores-src/pcsx_rearmed/libpcsxcore/new_dynarec/assem_arm.c
reconciles:
  - docs/legacy/2026-08-31-qnx-sync-cost-and-emulator-stutter.md §1-3
---

# Thread sync is a kernel call on QNX - and two dynarec bugs

Session 2026-08-31, all measured on the unit with [[profiler]].

## 1. `pthread_cond_signal` always enters the kernel

**Symptom.** Mupen64Plus-Next ran at ~real time but never above it; QSA `reserve=0/16` in every
window, `producer_waits=0` per session (the emulator never had to wait for audio), 35-47 ms hitches
became audible seams.

**Finding.** 63.5 % of on-CPU samples sat in ~450 bytes of `libc.so.3` that are not a function but
the **kernel call stub table** (`push {lr}; mov r12,#N; svc #0x51; pop {pc}`). Decoding `r12`
against the SDP's `kercalls.h`:

| call | name | share |
|---|---|---|
| 0x53 | `__KER_SYNC_CONDVAR_SIGNAL` | **59.0 %** |
| 0x50 | `__KER_SYNC_MUTEX_LOCK` | 24.1 % (= real contention; uncontended mutexes stay in userspace) |
| 0x54 | `__KER_SYNC_SEM_POST` | 6.1 % |
| 0x51 | `__KER_SYNC_MUTEX_UNLOCK` | 2.0 % |

On Linux an uncontended `pthread_cond_signal` is userspace-only; **on QNX it is always the
`SyncCondvarSignal` kernel call, waiter or not.** GLideN64's threaded renderer turns each GL call into
a `BlockingQueue::push` that called `notify_one()` unconditionally: one syscall per GL call.

**Fix.** Count blocked consumers under the mutex; signal only when the count is non-zero
(`push` reads `m_waiters` under the same mutex the consumer tests its predicate under, so no wakeup
can be lost; the only race costs one spurious signal). Same gate on
`RingBufferPool::removeBufferFromPool` (`notify_all` once per draw).

| | before | after |
|---|---|---|
| `CONDVAR_SIGNAL` share | 59.0 % | **6.7 %** |
| all sync calls | ~55 % | ~40 % |
| QSA reserve | 0/16 | 14-15/16 |
| conceal per gameplay window | 4-17 | 0 in 13 of 18 |
| `producer_waits` / session | 0 | 4318 (the emulator now runs ahead of real time) |

**Rule for this target:** audit every `notify_one/notify_all/pthread_cond_signal` on a hot path;
the remaining cost is mutex contention on the GL queue (needs batching or a lock-free queue).
Memory: `qnx-condvar-signal-is-a-syscall`.

## 2. PCSX-ReARMed: dynarec hands a clobbered `r0` to the GTE

Hard SIGSEGV entering gameplay (Need for Speed III), `ip` in `gteMACtoRGB_nf+0x4` (`ldr r1,[r0,#0x64]`)
with `r0=0xb80`, `lr` inside the core's BSS (= called from JIT code). In `assem_arm.c` the
`c2op` path called a C helper (`gteDPCS_part_*`, `gteGPF_part_*`) which may clobber `r0`, then
restored `r0` only inside `if (need_flags || need_ir)` before calling `gteMACtoRGB*`. When flags and
IR are dead, `gteMACtoRGB_nf` received the packed colour as `this`. Game/scene dependent, present in
both GCC 4.9 and 8.5 builds. **Fix:** hoist the `r0` reload out of the conditional.

## 3. GLideN64: vertex client-array over-read

Intermittent `memcpy` SIGSEGV in `RingBufferPool::createPoolBuffer` on the *source* pointer at a
page boundary (`len 0x370`). `opengl_Wrapper.cpp` copied `(count+1)*getStride()` bytes from the
lowest attribute pointer: reads one stride past the end, assumes one stride for all attributes,
ignores `first`. `(21+1)*40 = 880` = the faulting length; correct span 840. **Fix:**
`GlVertexAttribPointerManager::getAttribsSpan(lastIndex)` = max over enabled attributes of
`offset + lastIndex*stride + elementSize`, clamped to the 2 MB consumer vectors.

Diagnostic recipes used (core dumps, symbol matching, libc stub decoding): [[profiler]].
