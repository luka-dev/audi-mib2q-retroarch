# QNX Freedreno A3xx port

This directory contains an offline/QEMU-validated native Mesa Freedreno path
for the MU1316 APQ8064/Adreno 320 head unit.  It is not deployed to the HU or
the RetroArch package, and it is not yet a complete EGL/GLES2 replacement.

## Architecture

```text
Mesa state tracker / GLES2 (future integration)
        |
Gallium Freedreno A3xx + IR3
        |
Mesa fd_device / fd_pipe / fd_bo / fd_submit QNX backend
        |
qfd winsys
        |
stock libGSLUser.so
        |
/dev/kgsl-3D -> GSLKernel-A320.so -> Adreno 320
```

The port is pinned to Mesa commit
`479773c7e4264506f2d9ec4bf15c6bf677f0d67a`.  `mesa-qnx.patch`, the Meson
cross file and the overlays in this directory are sufficient to apply the QNX
changes to a clean checkout and repeat the ARM build.

## What is implemented

`qfd_winsys.[ch]` implements the recovered GSL transport boundary:

- dynamic loading of the stock MU1316 `libGSLUser.so` exports;
- device info and 3D context lifecycle;
- BO allocation, CPU mapping, 32-bit GPU VA and cache operations;
- bounded IB submission with BO ownership/range/alignment checks;
- timestamp read/wait with rollover-safe comparison;
- injectable dispatch for host tests.

`qfd_drmif_bridge.[ch]` and `mesa-overlay/qnx/` implement Mesa's
`fd_device`, `fd_pipe`, `fd_bo`, ringbuffer, reloc, submit and fence model on
top of that transport.  The QNX Mesa configuration intentionally builds only
the relevant A3xx Gallium backend and IR3 compiler.  The shader disk cache is
disabled because QNX 6.5 lacks the ELF build-id enumeration used by current
Mesa.  The QNX patch also initializes and destroys the `u_trace_flush` queue
fence explicitly.  Without that lifecycle, Mesa's first asynchronous trace
queue job asserts on QNX (the Linux futex fence happens to tolerate a
zero-filled object).

Raw GSL context/allocation/submit flags remain explicit.  Zero flags work in
the QEMU resource manager, but that is not evidence that the same flags and
cache policy are correct on the physical HU.

## Verified offline

Run the complete local sequence with:

```sh
tools/qnx-freedreno/test-host.sh
tools/qnx-freedreno/build-qnx.sh
tools/qnx-freedreno/build-mesa-qnx.sh
tools/qnx-freedreno/run-qemu.sh
```

The QEMU run currently verifies:

- GSL device info reports Adreno 320 and 512 KiB GMEM;
- allocation/map/cache/free and context/timestamp lifecycle;
- NOP submit, `CP_MEM_WRITE` readback and a synthetic IR3 triangle command
  stream containing `CP_DRAW_INDX_2`;
- the DRMIF-shaped BO/reloc/submit/fence path with canary readback;
- Mesa `fd_screen_create()` reports `FD320`, creates a real Gallium Freedreno
  context, compiles a clear through the normal asynchronous path, submits the
  generated A3xx command stream and observes its fence retire.

The probe has two explicit modes.  `--qemu-only` requires the Mesa submit and
fence to pass, but reports the final pixel readback as `SKIP`: this QEMU
translator keeps render targets in a host OpenGL FBO instead of copying them
back to the guest BO.  `--hardware` treats mapping failure or any pixel other
than approximately `{64,128,191,255}` as a test failure.  Therefore the QEMU
result is command-path evidence, not proof that the physical Adreno rendered
the right pixels.

The Mesa-linked probe is too large for this BSP's boot IFS.  `run-qemu.sh`
therefore creates a disposable QNX6 image, loads it at physical `0xd1000000`,
mounts it read-only and leaves the original firmware/QEMU images untouched.
Serial logs, IFS/QFS sizes and hashes are retained in
`build/qnx-freedreno/qemu-runs/run-*`; each successful run also contains a
compact `validation-summary.txt`.

An optional host-FBO trace is retained in
`build/qnx-freedreno/diagnostics/mesa-clear-host-fbo-20260909/`.  Its 16x16
target changes from black to white rather than the requested clear color,
while the guest BO remains zero.  This documents an incomplete QEMU
PM4-to-OpenGL translation path and is deliberately not counted as a render
pass.

Static build outputs are in `build/qnx-freedreno/mesa-static/`:

- `libfreedreno_qnx_drm.a`;
- `libfreedreno_qnx_winsys.a`;
- `libfreedreno_ir3.a`;
- `libfreedreno_a3xx_gallium.a`;
- stripped and debug variants of `qnx-freedreno-screen-probe`;
- `SHA256SUMS` and `MESA_COMMIT`.

The standalone QNX transport artifacts in `build/qnx-freedreno/` have their
own `SHA256SUMS` manifest.

The checksums are artifact integrity records, not a promise of bit-identical
archives: QNX `ar`, ELF build IDs and absolute debug paths can differ between
build directories.

## What is not proven yet

QEMU proves API wiring and command-path correctness; it does not measure real
Adreno performance, physical cache coherency, Screen presentation latency or
PPSSPP frame rate.  It also does not prove Mesa's requested color reached a
physical render target.  There is no QNX Screen buffer import/present path and
no EGL/GLES2 frontend in this port yet.  PPSSPP remains excluded from the HU
package, and nothing here is installed or started on the offline HU.

## Remaining HU gates

1. Run the non-submitting info probe on the HU.
2. Validate allocation flags, mappings and cache clean/invalidate with no IB.
3. Validate context creation, remembering that context handle `0` is valid.
4. Run `qnx-freedreno-screen-probe --hardware`: submit the Mesa-generated
   offscreen clear, wait with the bounded fence timeout and require the strict
   `{64,128,191,255}` CPU readback; do not attach a Screen buffer yet.
5. Run a Mesa-generated offscreen triangle and read it back after the clear
   gate passes.
6. Implement and validate QNX Screen buffer import/present and synchronization.
7. Expose EGL/GLES2, then profile the same RetroArch/PPSSPP workload matrix
   against the stock driver.

No performance or packaging claim should be made before gates 1-6 pass on the
physical unit.
