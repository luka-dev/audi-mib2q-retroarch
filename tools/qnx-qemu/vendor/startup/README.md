# guest/startup — the loader that lifts the 256 MiB cap

## Origin

| what | source |
|---|---|
| `bsp-qnx65-qemu-virt-a15/` | QNX 6.5 BSP for QEMU `-M virt` cortex-a15 (myQNX). Local original: `AUDI_2/Refferences/QNX_QEMU_BSP/bsp-qnx65-qemu-virt-a15/` |
| `bsp-qemu-arm/` | older QNX BSP for QEMU ARM, kept for reference only. Not built. |

The tree here is **already patched**. The original was left untouched.

## What we changed

The runtime changes are limited to the two board files below.

### 1. `init_raminfo.c` — RAM 256 MiB → 1 GiB

```c
- add_ram(0x40000000, MEG(256));
+ add_ram(0x40000000, MEG(1024));
```

The `startup-2gb` variant is this same tree built with `MEG(2048)`.

### 2. `init_qtime_virt.c` — clock fix

```c
- qtime->intr = 1;	/* GPT1 irq */
+ qtime->intr = 27;
```

On `qemu -M virt` the generic timer arrives on GIC PPI 27. With `intr = 1` the
guest clock never ticks and the whole stack hangs on the first timeout.

### 3. `init_mmu.c` — deliberately unchanged

Keep the stock `ARM_1TO1_SIZE = 0x10000000` (256 MiB) bootstrap direct map.
The verified Docker-built 1 GiB startup uses exactly this limit; after memmgr
starts, the rest of the advertised physical pool is reached through normal
temporary mappings.

Do not extend the early map to 320 MiB. That reaches
`0xe0000000-0xf3ffffff`, collides with QNX kernel virtual-address structures,
and makes generic `procnto-smp` fault near `0xf3fb0000` during
`bootimage_init`.

## What this does and does not fix

The patch only pays off together with the **generic SDP `procnto-smp`**
(1023776 bytes, REL — mkifs relocates it).

With the firmware `procnto-smp` (507904 bytes, EXEC) it is useless: the 256 MiB
cap is baked into that kernel's closed page allocator and kernel-VA layout.
Proven by deep IDA RE and by a clean source rebuild of startup at 1 GiB, which
crashes byte-for-byte identically at `bootimage_init@299`. The full list of
failed workarounds is in `docs/02-memory-and-loader.md`.

We did **not** compile procnto — we simply swapped it for the generic SDP one,
which turned out to be ABI-compatible with the ESO/HMI stack.

`build_boot_ifs.sh` references that kernel directly from the QNX toolchain
image. A firmware `guest/blobs/proc/boot/procnto-smp` is forbidden: the
507904-byte closed kernel crashes at `bootimage_init@299` with the 1 GiB
startup map.

## Build

Docker image `qnx65-armv7-toolchain` (https://github.com/luka-dev/qnx65-armv7-toolchain):

```sh
export QNX_HOST=/opt/qnx650/host/linux/x86
export QNX_TARGET=/opt/qnx650/target/qnx6
export MAKEFLAGS=-I$QNX_TARGET/usr/include   # else make dies on recurse.mk
cd bsp-qnx65-qemu-virt-a15/src/hardware/startup/boards/virt
make
# result: arm/le.v7/startup-virt
```

The Docker image's `qcc` driver provides the QNX-to-GCC option translation
(`-nostartup` → `-nostartfiles`, `-a` → `ar rc`, plus the ARMv7 toolchain
compatibility fixes). Stage 10 symlinks that exact driver into
`$QNX_HOST/usr/bin/qcc`, because the BSP invokes it by absolute path.

The BSP's obsolete bare `-M` bootstrap link-map option is disabled in
`boards/common.mk`: with the GCC-backed Docker `qcc`, bare `-M` means
dependency generation and prevents linking. No extra compiler wrapper is used.

Stage 10 cleans both the library and board object trees before building and
asserts that `startup-virt` contains the Cortex-A15 descriptor. Incremental
make once retained a stale pre-A15 `armv_list`, causing an immediate
`Unsupported CPUID` before the QNX console.

`boards/common.mk` passes the project `libstartup.a` as an explicit object.
Using ordinary `-lstartup` is unsafe with this toolchain wrapper: its effective
search order selected the SDP's stock A9-only archive even though the printed
command placed the BSP `-L` first.

startup is never loaded on its own: `mkifs` **relinks** it into the IFS through
the `[linker=...]` line of the build file.

## Verification

```sh
pidin info      # expect FreeMem: 1016Mb/1024Mb
```

Or dump the syspage asinfo: the `sysram` entry should read
`0x40000000-0x7fffffff`.

## Status in the current boot

This startup and the generic SDP `procnto-smp` are the only supported boot
stack. `run_hmi.sh` loads `out/boot.ifs`, and the application image begins
immediately above the 1 GiB managed range at `0x80000000`.

The split-image KGSL integration still needs a control boot after every
boot-recipe change; do not fall back to the historical 256 MiB firmware kernel
to hide an integration failure.

## Editor diagnostics

clang on macOS complains about `startup.h`, `add_ram` and `MEG`. These are false
positives — the QNX headers are not present locally. Only the docker build is
authoritative.
