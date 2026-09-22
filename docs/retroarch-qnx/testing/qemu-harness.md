---
title: QEMU MIB2Q harness and offline test suites
tags: [testing, qemu]
status: verified-source
sources:
  - tools/qnx-qemu/README.md, run-test.sh, test-cxx-runtime.sh, build-qemu.sh, prepare-static-cxx-runtime.sh, runtime.sha256
  - tools/qnx-tests/README.md, run-libretro-common.sh, run-ppsspp.sh
  - docs/legacy/2026-08-22-toolchain-and-ppsspp-bringup.md §11
reconciles:
  - tools/qnx-qemu/README.md
  - tools/qnx-tests/README.md
---

# QEMU MIB2Q harness and offline test suites

`tools/qnx-qemu/` is the minimal subset of the MIB2Q QEMU work (imported 2026-08-22 from
`Patches/rust-hmi/tests/qemu/mib2q`, without the 1 GiB `app.img`): patched **QEMU 9.1.0** engine
(`v9.1.0` commit `fd1952d8` + MIB2Q patch, Cocoa host GL renderer), the real MIB2Q `libc.so.3`,
loader and `libm.so.2`, serial bootstrap tools, extracted Screen/EGL/GLES files, and a patched
`startup-virt`/`libstartup.a` for Cortex-A15 / 1 GiB. The runtime directory is gitignored
(contains firmware); `runtime.sha256` refuses a different engine or loader silently.

Boot: `-M virt -smp 4 -cpu cortex-a15,cntfrq=12500000`, `procnto-smp`, deterministic icount by
default (`QNX_QEMU_ICOUNT=0` for experiments; `QNX_QEMU_CPUS=1..4`). A QNX-native `spawnv(P_WAIT)`
wrapper runs the test (a `pdksh -c` child never returned), decodes the wait status to an exit code
or `128+signal`, and QEMU stops as soon as it appears on serial (~2 s per test).

```sh
tools/qnx-qemu/run-test.sh build/qnx-cxx-runtime-smoke
tools/qnx-qemu/test-cxx-runtime.sh            # build + run the C++/EHABI/libm/mutex/future smoke
tools/qnx-tests/run-libretro-common.sh        # 133/133 (stdstring, utils, hashes, list, queue, rpng)
tools/qnx-tests/run-ppsspp.sh                 # 24/24 groups incl. VertexJit, ThreadManager stress
tools/qnx-qemu/build-qemu.sh                  # rebuild engine -> build/qemu-system-arm.rebuilt (not promoted)
```

`test-cxx-runtime.sh` proves: only `libc.so.3 libm.so.2` as dynamic deps; `throw`/`catch` through
the target loader; `powf/sqrtf/floorf`; 32 `std::mutex`+`condition_variable` recreate cycles at one
address; `promise/future`. This closed the loader/EHABI and libstdc++ uncertainty from
[[toolchain]] offline. Output `CXX_RUNTIME exception=ok math=70.000 sync-reuse=ok future=ok`.

`prepare-static-cxx-runtime.sh` is the shared gate that sanitises `libstdc++.a` (no libm stubs)
before any static C++ link.

## What it does not prove

APQ8064 speed, Adreno, QSA buffering, SD latency, physical `screen_wait_vsync()`, HMI integration.
JIT vs interpreter wall-time ratios under icount are meaningless and are not asserted. A full
frontend launch would need a larger IFS with Screen/EGL/input/QSA services.

Other cores' suites were **not** faked: PCSX's gpulib replay needs external dumps, gpSP's tests are
host code-generator diffs needing other cross-assemblers, Mupen's regression needs ROMs and
reference screenshots.

The Freedreno research reuses the harness with a disposable QNX6 image ([[freedreno-qnx]]).
