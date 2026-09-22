# MU1316 QNX GSL ABI notes

`qnx_gsl_abi.h` records the ABI boundary recovered from the exact MU1316
`libGSLUser.so`. It is intentionally smaller than a driver API and contains
only layouts and prototypes supported by local binary evidence.

## What is confirmed

- Public `gsl_memdesc` is 32 bytes on ARM32: `hostptr` at +0, an untransmitted
  dword at +4, 64-bit `gpuaddr` at +8, 64-bit `size` at +16, and 64-bit
  `flags` at +24. Allocation explicitly clears the high gpuaddr dword at +12.
- `gsl_command_issueib_sync` consumes 16-byte direct-IB elements, not the
  32-byte internal records used later inside `libGSLUser`.
- Its seven public arguments are device, context, IB array, IB count,
  timestamp pointer, flags, and sync object.
- The direct-IB wrapper copies bytes +0..+7 into `memdesc.gpuaddr`, reads
  `sizedwords` at +8, advances input by 0x10, and ignores +12 on this path.
- The `_IO_MSG` transport prefix and the listed subtype numbers are recovered
  directly from message construction in `libGSLUser`.
- `0x900` is library/client entry and `0x910` is library/client exit. Device
  open and close are separate requests `0x920` and `0x921`; device property
  lookup is `0x923`. The `0x910` value was confirmed by running the stock
  `libGSLUser.so` against the QEMU resource manager.

## What is deliberately not claimed

- The semantic name of direct-IB dword +4 is not proven. It is retained as
  `gpuaddr_hi_or_pad`; the A320 wire submit uses only the low dword.
- The meaning of every allocation and submit flag is not yet complete.
- A safe standalone command stream has not yet been submitted to the real HU.
- The simple `gsl_command_issueib` input contract is not declared. The GLES
  driver uses the better-confirmed `gsl_command_issueib_sync` path.

The current evidence report is
`docs/retroarch-qnx/re/gsl-port-boundary.md` (legacy: `docs/legacy/mu1316-gsl-port-boundary-20260908.md`). The next hardware step is a
non-submitting loader/open/getinfo probe:

```sh
tools/qnx-gsl-port/build-probe.sh
tools/qnx-gsl-port/run-probe.sh
```

The runner refuses to start while RetroArch is active. The probe performs no
context creation, allocation, cache operation, or IB submission. A separate
allocate/cache round-trip can follow after this ABI check. Command submission
must wait for a known-safe A320 packet stream and watchdog/recovery plan.
