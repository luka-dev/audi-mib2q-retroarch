# Universal recipe: making an ARM dynarec (JIT) safe on QNX 6.5 (MHI2Q / APQ8064)

**Applies to any libretro core with an ARMv7 dynamic recompiler on this target:**
gpSP, Mupen64Plus-Next, PCSX-ReARMed (and its threaded paths). Same root cause,
same one-function fix.

## Symptom

An ARM dynarec built for QNX 6.5 either:
- runs but crashes/glitches intermittently (invalid branches, jumps to a
  sentinel like `0xfffffffe`), or
- was disabled on this target "for stability" (`HAVE_DYNAREC := 0`) and the core
  falls back to the much slower interpreter.

## Root cause

A JIT writes freshly emitted code into a data-mapped buffer, then executes it.
On ARM the instruction cache is **not** coherent with the data cache, so the
emitter MUST invalidate the I-cache for the written range before the CPU fetches
it. Cores route this through a `platform_cache_sync()` / `cache_flush()` helper
that, on generic ARM, calls GCC's `__clear_cache()`.

**On QNX 6.5 with the qcc/GCC 4.x ARM toolchain, `__clear_cache()` lowers to a
no-op.** No I-cache maintenance is emitted. The JIT then executes whatever stale
bytes were already in the I-cache → nondeterministic corruption.

### Verified evidence

Disassembly of a real QNX-built object from the old standalone gpSP experiment
(`AUDI_2/Patches/gpSP/cpu_threaded.o`, ELF 32-bit ARM EABI5):

```
000004ac <platform_cache_sync>:
     4ac:  e12fff1e   bx  lr        <- empty function, no cache maintenance
```

That build "worked" only by cache luck — hence it was a hacky crutch, not a fix.
It did, however, prove the two hard parts already work on the real MHI2Q unit:
the ARM emitter executes correctly, and the RWX translation-cache mapping
(`mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANON|MAP_PRIVATE)`, validated to stay
within ±32 MB branch reach) is honoured by QNX. Only cache coherency was left to
chance.

## The fix

Give the sync helper a real QNX branch. The proven call is already shipping in
PCSX-ReARMed's new_dynarec (`libpcsxcore/new_dynarec/new_dynarec.c`,
`new_dyna_clear_cache`), which is why PCSX's dynarec is the one that works on
this unit:

```c
#include <sys/mman.h>   /* msync + MS_* on QNX */

void platform_cache_sync(void *start, void *end) {
#if defined(__BLACKBERRY_QNX__)      /* QNX 6.5: __clear_cache is a no-op */
    size_t len = (char *)end - (char *)start;
    msync(start, len, MS_SYNC | MS_CACHE_ONLY | MS_INVALIDATE_ICACHE);
#else
    __clear_cache(start, end);
#endif
}
```

Then re-enable the dynarec for the QNX platform (`HAVE_DYNAREC := 1`, or the
core's equivalent), keeping the existing RWX JIT mapping.

### Per-core drop-in points

| Core        | Function to patch                | File                                                      |
|-------------|----------------------------------|-----------------------------------------------------------|
| gpSP        | `platform_cache_sync`            | `cpu_threaded.c` (cache-invalidation dispatch, ~line 247) |
| Mupen64Next | `cache_flush` (calls `__clear_cache`) | `mupen64plus-core/.../new_dynarec/arm/assem_arm.c:242` |
| PCSX-ReARMed| already has the QNX branch       | `libpcsxcore/new_dynarec/new_dynarec.c` (reference impl)  |

## Risk boundary — READ THIS

`msync(... MS_INVALIDATE_ICACHE)` invalidates the I-cache of the **calling
core**. That is sufficient **only when the same thread both compiles and
executes** the code (single-core coherency). This holds for:

- gpSP dynarec — single-threaded. **Safe.**
- Mupen ARM new_dynarec — single-threaded execution. **Safe.**
- PCSX single-threaded dynarec (`drc_thread` off). **Safe** — this is the
  proven case.

It does **NOT** automatically cover cross-core cases, where a helper thread
compiles on core A and the emulation thread executes on core B:

- PCSX `pcsx_rearmed_drc_thread = enabled`
- Mupen `mupen64plus-ThreadedRenderer = True` (GL, not CPU, but same class of
  cross-thread coherency question)

For those, a single-core `msync` may miss the executing core's I-cache. Validate
those paths separately on hardware before trusting them; if unstable, pin
compile+execute to the same core (QNX `_NTO_TCTL_RUNMASK`) or force the
single-threaded dynarec.

## Verification checklist (on device)

1. Build the core with the QNX `msync` branch + dynarec enabled.
2. Boot a JIT-heavy title (GBA: Golden Sun / Mode-7 racer; N64: any 3D game).
3. Run 10+ minutes; watch for invalid-branch crashes in the slog.
4. If stable single-threaded, only then evaluate the cross-core threaded options
    against the risk boundary above.

## Status in this repo

All three cores now carry the QNX `msync` cache-invalidation path — confirmed by
a `U msync` import in each stripped `.so` after a clean build.

- PCSX-ReARMed: dynarec ON, QNX `msync` path present upstream. Working.
- gpSP: **patched + dynarec ENABLED**. `platform_cache_sync` (cpu_threaded.c)
  uses the QNX `msync` branch; QNX Makefile block sets `HAVE_DYNAREC := 1` and
  `MMAP_JIT_CACHE = 1` (RWX JIT buffer). Was interpreter-only before.
- Mupen64Plus-Next: **patched**. `cache_flush` (assem_arm.c) uses the QNX `msync`
  branch. Dynarec was already built (`WITH_DYNAREC = arm`); only the flush was
  broken. Still **never run on hardware** — first N64 boot is the real test.

All patched builds compile and link clean. Hardware validation (the checklist
above) is still pending — the fix is proven correct in source and in PCSX's
shipping precedent, not yet re-confirmed on the unit for gpSP/Mupen.
