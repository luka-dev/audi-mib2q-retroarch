# QNX core unit tests

These scripts cross-compile the vendored test sources as ARMv7 QNX executables
and execute them inside the four-vCPU MIB2Q QEMU harness.

```sh
tools/qnx-tests/run-libretro-common.sh
tools/qnx-tests/run-ppsspp.sh
```

`run-libretro-common.sh` uses a small local implementation of the subset of
the Check API used by the six vendored suites. Test bodies and assertions still
come from libretro-common; each case is reported independently.

`run-ppsspp.sh` builds the standalone PPSSPP unit-test executable against the
same QNX core objects and runs each group in a fresh guest. This lets the suite
continue after target signals. Vulkan/glslang shader validation is excluded
because the MIB2Q build intentionally contains only GLES2; it is reported as
unsupported rather than counted as a pass.

Build products are written below `build/tests/` and are ignored by Git.

PCSX-ReARMed's GPU replay harness requires external state/command dumps, gpSP's
tests target host-side code generators with other cross-assemblers, and Mupen's
regression suite requires ROMs and reference screenshots. They are not counted
as target QEMU passes without those inputs.
