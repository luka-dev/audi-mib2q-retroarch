# retroarch-qnx — RetroArch for the Audi MHI2Q head unit (QNX 6.5, armle-v7)

RetroArch + gpSP, PCSX-ReARMed and Mupen64Plus-Next on the MU1316 head unit
(QNX Neutrino 6.5.0, APQ8064 Krait, Adreno 320, 1024x480). A Java HMI hook adds a
**Games** entry that launches the native emulator on its own display context and
owns the OEM audio session; the native side renders via EGL/GLES2 on displayable
43, writes PCM through QSA and reads USB/Bluetooth gamepads through io-hid/io-usb.

All sources are vendored (no submodules); `VENDORED_SOURCES.md` records provenance.

## Documentation

The documentation is an Obsidian vault: **`docs/retroarch-qnx/`** — open that
folder as a vault, start at `INDEX.md`. Every note states how its facts were
verified (source / firmware / device log). Key entry points:

| Question | Note |
|---|---|
| How does it all fit together? | `architecture.md` |
| How do I build? | `build/build-pipeline.md`, `build/toolchain.md` |
| How do I install / update the unit? | `deploy/install-procedure.md`, `deploy/filesystem-layout.md` |
| What does the HMI hook do? | `hmi/games-menu-injection.md`, `hmi/session-lifecycle.md`, `hmi/audio-session.md` |
| What is proven on hardware and what is not? | `testing/hardware-validation-matrix.md` |
| Why is PPSSPP not shipped? | `cores/ppsspp-status.md`, `re/adreno-driver-hotpath.md` |

Superseded long-form documents are kept under `docs/legacy/` for history only.

## Quick start

```sh
./build.sh            # jar + frontend + 3 cores  ->  build/mnt_app, build/sd_card
./build.sh clean
./lsd_patch/build.sh  # HMI jar only
./run-macos.sh        # host UI smoke test (out/macos-test)
```

Requires the `../qnx-65-sdp-docker` toolchain image (GCC 8.5 + binutils 2.38 `as`),
JDK 8 and the MU1316 class/JCL jars from `../jxe2jar` (see `build/java-jar-build.md`).

Deploy: copy `build/mnt_app/*` to `/mnt/app` (remounted rw for the copy only) and
`build/sd_card/*` to a FAT32 SD card; reboot after a jar change.

## Layout

```
src/            RetroArch (QNX frontend, display, QSA, HID/XUSB, lifecycle, discovery task)
cores-src/      gpsp, pcsx_rearmed, mupen64plus_next (shipped); ppsspp (ported, not shipped)
java_patch/     HMI hook sources (Java 1.4)      lsd_patch/   jar build + ra_mhi2q.jar
pkg/            factory config, launcher ra.sh, assets, profiles, databases, cheats
build/          deployable trees + research artefacts     tools/   qemu, tests, bench, profiler, freedreno, gsl-port
docs/           Obsidian vault (retroarch-qnx/) + legacy/  output/r2/  RE disassembly evidence
```
