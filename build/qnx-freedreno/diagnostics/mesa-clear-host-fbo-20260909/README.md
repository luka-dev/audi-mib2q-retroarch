# Mesa clear host-FBO diagnostic

This is diagnostic evidence from a QEMU run with `GLP_RESOLVE_TRACE` enabled.
It is not a hardware render pass.

- The Mesa/QNX probe created a 16x16 RGBA8 target and successfully submitted
  and retired its A3xx command stream.
- `before` is black (`00 00 00` pixels).
- `src` and `after` are white (`ff ff ff` pixels), not the requested
  `{64,128,191,255}` clear.
- The guest BO readback in `qfd-qemu.log` is `{0,0,0,0}`.

The matching `src`/`after` hashes show that QEMU changed and resolved a host
OpenGL FBO.  The wrong host color and unchanged guest memory identify the
remaining gap as the custom QEMU PM4/resolve translation, so this trace must
not be used to claim visual correctness or physical-GPU performance.
