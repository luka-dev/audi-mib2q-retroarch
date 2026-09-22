---
title: GSL boundary - the user-space edge below GLES (Adreno 320)
tags: [re, firmware, gpu, research]
status: verified-decompile
sources:
  - docs/legacy/mu1316-gsl-port-boundary-20260908.md (Russian original)
  - output/r2/mu1316-gsl-client-disassembly-20260908.txt, mu1316-gsl-wire-disassembly-20260908.txt
  - tools/qnx-gsl-port/qnx_gsl_abi.h, README.md
reconciles:
  - docs/legacy/mu1316-gsl-port-boundary-20260908.md
  - tools/qnx-gsl-port/README.md
---

# GSL boundary - the user-space edge below GLES (Adreno 320)

Static r2/rabin2 analysis (2026-09-08) of the extracted MU1316 display image. No GPU commands were
sent; firmware unmodified.

## The stack

```text
OpenGLES20.so  --DT_NEEDED-->  libGSLUser.so  --MsgSendv(_IO_MSG) /dev/kgsl-3D-->  kgsl (resmgr) + GSLKernel-A320.so  -->  Adreno 320
```

The client hands **ready-made indirect buffers** (GPU address + length in dwords) to the kernel
side. That is the insertion point for a self-made command generator / a QNX backend for Mesa
Freedreno ([[freedreno-qnx]]). Memory/device management (pmem, smmu, mmap_peer, resmgr_attach) stays
in the stock kernel-side process.

| Binary | SHA-256 (prefix) |
|---|---|
| `OpenGLES20.so` | `cc89187b` |
| `libGSLUser.so` | `3e8d2554` |
| `GSLKernel-A320.so` | `08410783` |
| `kgsl` (base 0x100000, interp `/usr/lib/ldqnx.so.2`) | `a4408fcc` |
| `libOSUser.so` | `ae3950cd` |

r2's "os linux" auto-detection is wrong for these QNX ELFs.

## Exports used by GLES (`libGSLUser.so`)

`gsl_library_open` 0x37a4 (opens `/dev/kgsl-3D`), `gsl_context_create` 0x1b4c / `_destroy` 0x25c8,
`gsl_memory_alloc_pure` 0x2fb4 / `_free_pure` 0x3388, `gsl_command_issueib` 0x2558,
`gsl_command_issueib_sync` 0x2378, `_with_alloc_list` 0x2260, `gsl_command_readtimestamp` 0x2108,
`_waittimestamp` 0x2094, `gsl_memory_cacheoperation` 0x4350.

## Wire protocol (`_IO_MSG`, not devctl, not Linux ioctl)

8-byte QNX header `type=0x113, combine_len=8, mgrid=0xf000, subtype` built by helpers 0x45f0/0x6164.

| subtype | purpose | evidence |
|---|---|---|
| 0x900 / 0x910 | library/client enter / exit | 0x39ac; 0x910 confirmed at runtime vs the QEMU resmgr |
| 0x920 / 0x921 / 0x923 | device open / close / get property | 0x3f94, 0x2dc4, 0x3ba4 |
| 0x950 / 0x951 | context create (reply 4 B) / destroy | 0x6e38, 0x6d9c |
| 0x960 / 0x961 | GPU alloc (payload 16 B, reply 24 B) / free | 0x30e0, 0x327c, 0x34d0 |
| 0x930 | submit IB list: `device, context, numibs, flags, timestamp, {gpuaddr, sizedwords}...` | 0x6398, serialization 0x6310-0x63d0 |
| 0x931 / 0x933 | read / wait timestamp | 0x7174, 0x7240 |
| 0x991 | cache operation (+ 24 B descriptor) | 0x43d8 |

## Memory descriptor layouts (do not memcpy public -> wire)

```c
struct gsl_memdesc_arm32 { uint32_t hostptr; uint32_t reserved04; uint64_t gpuaddr; uint64_t size; uint64_t flags; }; /* 32 B; +12 cleared on alloc */
struct gsl_memdesc_wire  { uint32_t hostptr; uint32_t gpuaddr; uint64_t size; uint64_t flags; };                      /* 24 B */
```

`gsl_command_issueib_sync(device, context, direct_ib[], numibs, uint32_t *timestamp, flags, syncobj)`
consumes **16-byte direct-IB** elements (bytes +0..7 -> gpuaddr, `sizedwords` at +8, +12 ignored on
this path) and builds the internal 32-byte record. Seven arguments confirmed at all four GLES call
sites (0x9d0a4, 0x9d194, 0x9d23c, 0x9d2f4). Compile-time-checked C in `tools/qnx-gsl-port/qnx_gsl_abi.h`.

Timestamps: 0x7010 reads via a shared-memory fast path when flags allow, else 0x931; types 1/2/3
differ; 0x71f4 checks first, then waits (0x933) with a `gfx_os_sleep` retry. **IB acceptance is not
GPU completion** - never free memory right after `MsgSendv`.

## Not proven

Semantic name of direct-IB dword +4 (`gpuaddr_hi_or_pad`), full flag semantics, the simple
`gsl_command_issueib` input contract, and any submission on the real HU.

## Path (mirrors `tools/qnx-gsl-port/README.md`)

1. `libGSLUser` as transport with the recovered signatures; a direct `MsgSendv` backend later.
   First a **non-submitting** open/getinfo probe: `tools/qnx-gsl-port/build-probe.sh`, `run-probe.sh`
   (refuses to run beside RetroArch).
2. Offscreen: allocate -> CPU write + cache clean -> minimal IB -> retired timestamp -> invalidate ->
   verify; memwrite/copy first, then a triangle.
3. Context ownership: which registers/GMEM shadow the stock kernel preserves.
4. Freedreno BO/submit/fence adaptation. 5. Screen/EGL buffer import + sync. 6. Compare the same
   GLES workload in time and pixels - the overhead reduction is a hypothesis until then.
