# QNX 6.5 / Audi MHI2Q port

Target: Qualcomm APQ8064 (ARMv7 Krait), QNX 6.5, GLES2, softfp ABI.

Build from the repository root with `./build.sh`, or build only the core inside
the project toolchain container with:

    . ./VENDORED_SOURCES.env
    make -C cores-src/ppsspp/libretro platform=qnx -j4 \
        GIT_VERSION="$(printf '%.7s' "$PPSSPP_SOURCE_COMMIT")"

The QNX build uses GCC 8.5/C++17, ARM JIT, NEON, GLES2 and the vendored
QNX-built FFmpeg archives. Vulkan, OpenXR and their shader translation stack
are excluded because the MHI2Q exposes none of those APIs. PPSSPP system assets
are staged on the SD card at `retroarch/system/PPSSPP`; saves and emulated
Memory Stick data remain under RetroArch's SD save directory.

The generated core requires the GCC 8.5 `libstdc++.so.6` staged by `build.sh`.
It remains ABI-compatible with the older GCC-built cores in the same process.
