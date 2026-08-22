# QNX ARM/MIB2Q QEMU test harness

This directory is the minimal self-contained test subset imported from
`Patches/rust-hmi/tests/qemu/mib2q` on 2026-08-22. It deliberately excludes
the 1 GiB `app.img`, complete stock HMI images, build caches and unrelated Rust
UI sources.

Included locally (ignored by Git because the runtime contains firmware):

- patched QEMU 9.1.0 MIB2Q engine and ROM data;
- the real MIB2Q `libc.so.3`/loader and `libm.so.2`;
- serial bootstrap utilities and the extracted Screen/EGL/GLES runtime;
- patched QNX `startup-virt` and `libstartup.a` for Cortex-A15/1 GiB RAM.

Tracked recipe inputs include the QEMU patch, host GL renderer, complete
startup BSP source, hashes, IFS recipe and runner. `runtime.sha256` prevents a
different engine or firmware loader from being accepted silently.

## Run an ARM QNX executable

```sh
tools/qnx-qemu/run-test.sh build/qnx-cxx-runtime-smoke
tools/qnx-qemu/run-test.sh build/tests/qnx-ppsspp/PPSSPPUnitTest MemMap
```

Or build and run the included C++ runtime/EHABI test in one command:

```sh
tools/qnx-qemu/test-cxx-runtime.sh
```

It verifies that only the real MIB2Q `libc.so.3` and `libm.so.2` remain as
dynamic dependencies; libstdc++ and libgcc are linked statically. The test then
exercises STL allocation, `throw`/`catch`, string destruction, the
`powf`/`sqrtf`/`floorf` paths involved in the earlier PPSSPP failures,
repeated mutex/condition-variable construction at one address, and
promise/future synchronization.

The runner builds its QNX-native status wrapper and a small IFS with the existing
`qnx65-armv7-toolchain:latest` Docker image, boots it on
`-M virt -smp 4 -cpu cortex-a15,cntfrq=12500000`, captures serial output and
returns the decoded guest exit status (or `128 + signal`). QEMU is stopped as
soon as that status appears; logs remain in `tools/qnx-qemu/build/`. Override
the CPU count with `QNX_QEMU_CPUS=1..4` when a single-core comparison is useful.
`QNX_QEMU_ICOUNT=0` is available for benchmark experiments. Deterministic
icount is the default for functional tests. PPSSPP still prints the
JIT/interpreter ratio, but the QNX suite does not use it as a correctness gate:
QEMU scheduling and icount are not an APQ8064 performance model.

This is authoritative for QNX loader/EHABI, C++ exceptions, ARM codegen and
pure core logic. The current PPSSPP suite passes all 24 target groups,
including `VertexJit` and the full `ThreadManager` stress path. It is not a
performance model of the APQ8064, Adreno, QSA device or SD card.

PPSSPP's `PPSSPPUnitTest` and RetroArch/libretro-common tests can be linked as
QNX ARM executables using the scripts in `tools/qnx-tests/`. A full frontend/core launch is
different: it additionally needs an IFS containing Screen, EGL/GLES, input,
filesystem content and (for meaningful audio tests) QSA. This minimal harness
intentionally does not pretend that emulated timing proves hardware pacing or
`screen_wait_vsync()` behavior.

## Rebuild the patched QEMU engine

```sh
tools/qnx-qemu/build-qemu.sh
```

This fetches locked upstream QEMU `v9.1.0` commit
`fd1952d814da738ed107e05583b3e02ac11e88ff`, applies the imported MIB2Q patch
and rebuilds the Cocoa host renderer. The result is written to
`build/qemu-system-arm.rebuilt`; it does not silently replace the locked
runtime. Test it explicitly with:

```sh
QNX_QEMU_BIN="$PWD/tools/qnx-qemu/build/qemu-system-arm.rebuilt" \
  tools/qnx-qemu/run-test.sh build/qnx-cxx-runtime-smoke
```

A rebuilt binary must be hardware/visual validated before promoting it and
updating `runtime.sha256`.
