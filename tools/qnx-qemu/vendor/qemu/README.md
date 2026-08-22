# Reproducible MIB2Q QEMU source recipe

Git tracks the source delta needed for this MIB2Q machine, not a compiled
`qemu-system-arm` and not a 500+ MiB upstream QEMU checkout.

- Upstream: QEMU `v9.1.0` (`fd1952d814da738ed107e05583b3e02ac11e88ff`).
- `patches/mib2q-qemu-v9.1.0.patch`: keypad, shared-memory GPU, USB network,
  ARM virt wiring, and the later display/presentation fixes.
- `glpass/`: the host OpenGL command decoder and renderer sources linked into
  QEMU. Its archive is rebuilt with Apple `clang`, `/usr/bin/ar`, and
  `/usr/bin/ranlib`.

Run `../../build-qemu.sh`. It fetches the pinned upstream source into ignored
`../qemu-upstream`, stages the renderer beside that source as required by the
patch, and writes the executable to ignored `../../runtime/qemu-system-arm`.

This keeps the repository independent from sibling `Tools/qemu-src` and
`Tools/qnx-gl-passthrough` workspaces while retaining an auditable source
recipe. The build still requires normal system build dependencies and network
access for the pinned upstream checkout.
