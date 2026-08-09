#!/bin/sh
# Build RetroArch + a test libretro core for MHI2Q (QNX 6.5 armle-v7).
# Uses the consolidated toolchain image via ../qnx-65-sdp-docker/host-scripts/qnx-run.sh
# (mounts the retroarch-qnx dir as /src). Run from anywhere.
#
#   ./build.sh          # frontend (griffin) + testcore, strip, report sizes
#   ./build.sh clean    # remove build products
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
QNX="$HERE/../qnx-65-sdp-docker/host-scripts/qnx-run.sh"
GPSP_SRC="$HERE/cores-src/gpsp"

[ -f "$GPSP_SRC/Makefile" ] || {
   echo "!! missing gpSP source tree: $GPSP_SRC" >&2
   exit 1
}
GPSP_GIT_VERSION=$(git -C "$GPSP_SRC" rev-parse --short HEAD)

if [ "${1:-}" = clean ]; then
   cd "$HERE" && "$QNX" env GPSP_GIT_VERSION="$GPSP_GIT_VERSION" bash -c '
      cd /src
      rm -f src/griffin/griffin.o src/retroarch src/retroarch.stripped \
         testcore/testcore_libretro.so
      make -C cores-src/gpsp clean platform=qnx GIT_VERSION="$GPSP_GIT_VERSION"
      make -C cores-src/pcsx_rearmed -f Makefile.libretro clean platform=qnx \
         CC=arm-unknown-nto-qnx6.5.0eabi-gcc
      rm -f cores-src/gpsp/gpsp_libretro.so
      rm -f build/pcsx_rearmed_libretro.so
      rm -rf build/mnt_app build/sd_card
   '
   exit 0
fi

cd "$HERE"
"$QNX" env GPSP_GIT_VERSION="$GPSP_GIT_VERSION" bash -c '
set -e
cd /src/src
echo ">> building retroarch (griffin, platform=qnx)…"
rm -f griffin/griffin.o retroarch retroarch.stripped
make -f Makefile.griffin platform=qnx 2>&1 | grep -E "Error|error:|undefined reference" || true
[ -f retroarch ] || { echo "!! link failed"; exit 1; }
arm-unknown-nto-qnx6.5.0eabi-strip retroarch -o retroarch.stripped

echo ">> building gpsp_libretro.so (clean, ARM dynarec + NEON)…"
cd /src/cores-src/gpsp
make clean platform=qnx GIT_VERSION="$GPSP_GIT_VERSION"
make -j4 platform=qnx \
   GIT_VERSION="$GPSP_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc \
   CXX=arm-unknown-nto-qnx6.5.0eabi-g++ \
   AR=arm-unknown-nto-qnx6.5.0eabi-ar \
   CODE_DEFINES="-mfpu=neon -B/opt/tools/neon-as/bin"
arm-unknown-nto-qnx6.5.0eabi-strip \
   gpsp_libretro_qnx.so -o gpsp_libretro.so

echo ">> building pcsx_rearmed_libretro.so (clean, ARM dynarec + NEON)…"
cd /src/cores-src/pcsx_rearmed
make -f Makefile.libretro clean platform=qnx \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc
make -j4 -f Makefile.libretro platform=qnx \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc
arm-unknown-nto-qnx6.5.0eabi-strip pcsx_rearmed_libretro_qnx.so \
   -o /src/build/pcsx_rearmed_libretro.so

echo ">> building testcore_libretro.so…"
cd /src
arm-unknown-nto-qnx6.5.0eabi-gcc -std=gnu99 -O2 -fPIC -shared \
   -include stddef.h -Isrc/libretro-common/include \
   testcore/testcore_libretro.c -o testcore/testcore_libretro.so 2>/dev/null

echo ">> staging deployable mnt_app + sd_card trees…"
[ -f pkg/assets/ozone/regular.ttf ] || {
   echo "!! missing external Ozone assets in pkg/assets/ozone"
   exit 1
}
PCSX_CORE=build/pcsx_rearmed_libretro.so
RUNTIME_LIBS=pkg/runtime-libs
for _required in \
   "$PCSX_CORE" \
   "$RUNTIME_LIBS/libstdc++.so.6" \
   "$RUNTIME_LIBS/libhiddi.so.1" \
   "$RUNTIME_LIBS/SOURCE.txt" \
   lsd_patch/ra_mhi2q.jar \
   pkg/info/SOURCE.txt \
   pkg/database/rdb/SOURCE.txt \
   pkg/cheats/SOURCE.txt \
   pkg/rumble/qnx/SOURCE.txt; do
   [ -f "$_required" ] || { echo "!! missing external resource manifest: $_required"; exit 1; }
done

MNT_STAGE=build/mnt_app
APP_DIR="$MNT_STAGE/root/retroarch"
JAR_DIR="$MNT_STAGE/eso/hmi/lsd/jars"
SD_STAGE=build/sd_card
SD_DIR="$SD_STAGE/retroarch"

rm -rf "$MNT_STAGE" "$SD_STAGE"
mkdir -p "$APP_DIR/cores" "$APP_DIR/lib" \
         "$APP_DIR/assets/ozone" "$APP_DIR/assets/audi" \
         "$APP_DIR/assets/pkg" "$APP_DIR/autoconfig/qnx" \
         "$APP_DIR/rumble/qnx" "$APP_DIR/info" "$JAR_DIR"

# /mnt/app: complete immutable/safe application layer. UI and controller
# resources are kept here so RetroArch remains controllable with a blank SD.
cp src/retroarch.stripped "$APP_DIR/retroarch"
cp cores-src/gpsp/gpsp_libretro.so "$APP_DIR/cores/gpsp_libretro.so"
cp "$PCSX_CORE" "$APP_DIR/cores/pcsx_rearmed_libretro.so"
cp testcore/testcore_libretro.so "$APP_DIR/cores/testcore_libretro.so"
cp "$RUNTIME_LIBS/libstdc++.so.6" "$RUNTIME_LIBS/libhiddi.so.1" \
   "$RUNTIME_LIBS/SOURCE.txt" "$APP_DIR/lib/"
cp pkg/retroarch.cfg pkg/ra.sh pkg/content-rules.cfg "$APP_DIR/"
cp pkg/assets/COPYING "$APP_DIR/assets/"
cp -R pkg/assets/ozone/. "$APP_DIR/assets/ozone/"
cp -R pkg/assets/audi/. "$APP_DIR/assets/audi/"
cp -R pkg/assets/pkg/. "$APP_DIR/assets/pkg/"
cp pkg/autoconfig/qnx/*.cfg pkg/autoconfig/qnx/COPYING \
   pkg/autoconfig/qnx/SOURCE.txt "$APP_DIR/autoconfig/qnx/"
cp pkg/rumble/qnx/*.cfg pkg/rumble/qnx/SOURCE.txt "$APP_DIR/rumble/qnx/"
cp pkg/info/gpsp_libretro.info pkg/info/pcsx_rearmed_libretro.info \
   pkg/info/COPYING pkg/info/SOURCE.txt "$APP_DIR/info/"
# The source collection calls this test_libretro.info, while our diagnostic
# core binary is testcore_libretro.so. Core Info is matched by basename.
cp pkg/info/test_libretro.info "$APP_DIR/info/testcore_libretro.info"
cp lsd_patch/ra_mhi2q.jar "$JAR_DIR/ra_mhi2q.jar"
cp pkg/mnt_app.README.txt "$MNT_STAGE/README_INSTALL.txt"

# /fs/sda0: portable writable card. The config starts as an exact factory copy;
# ra.sh also creates it from appimg when a physically new/blank card is used.
mkdir -p "$SD_DIR/config/remaps" "$SD_DIR/info" \
         "$SD_DIR/database/rdb" "$SD_DIR/cheats" \
         "$SD_DIR/roms" "$SD_DIR/ps1" "$SD_DIR/gba" \
         "$SD_DIR/saves" "$SD_DIR/states" "$SD_DIR/system" \
         "$SD_DIR/playlists" "$SD_DIR/thumbnails" \
         "$SD_DIR/logs" "$SD_DIR/screenshots" "$SD_DIR/downloads" \
         "$SD_DIR/filters/audio" "$SD_DIR/filters/video" \
         "$SD_DIR/wallpapers" "$SD_DIR/overlays/keyboards"
cp pkg/retroarch.cfg "$SD_DIR/config/retroarch.cfg"
cp pkg/sd/retroarch/RESOURCES.txt "$SD_DIR/"
cp pkg/sd/retroarch/RESOURCES.txt "$SD_STAGE/README_INSTALL.txt"
cp "$APP_DIR/info/"*.info pkg/info/COPYING pkg/info/SOURCE.txt "$SD_DIR/info/"
cp pkg/database/rdb/*.rdb pkg/database/rdb/COPYING \
   pkg/database/rdb/SOURCE.txt "$SD_DIR/database/rdb/"
cp -R pkg/cheats/. "$SD_DIR/cheats/"
if [ -d pkg/sd/retroarch/thumbnails ]; then
   cp -R pkg/sd/retroarch/thumbnails/. "$SD_DIR/thumbnails/"
fi

# Current local test content becomes the ready-to-copy SD image. Saves/states
# are deliberately not copied: a new card must start with clean user state.
if [ -d "build/macos-local/games/Sony - PlayStation" ]; then
   cp -R "build/macos-local/games/Sony - PlayStation/." "$SD_DIR/ps1/"
fi
if [ -d "build/macos-local/games/Nintendo - Game Boy Advance" ]; then
   cp -R "build/macos-local/games/Nintendo - Game Boy Advance/." "$SD_DIR/gba/"
fi
if [ -f build/macos-local/system/scph1001.bin ]; then
   cp build/macos-local/system/scph1001.bin "$SD_DIR/system/scph1001.bin"
fi

# macOS metadata has no place on FAT/QNX installation media.
find "$MNT_STAGE" "$SD_STAGE" -type f -name .DS_Store -delete

chmod 755 "$APP_DIR/retroarch" "$APP_DIR/ra.sh"
find "$APP_DIR" -type f ! -name retroarch ! -name ra.sh -exec chmod 644 {} +
find "$SD_DIR" -type f -exec chmod 644 {} +

echo
echo "=== artifacts ==="
printf "  retroarch (stripped): %s bytes  (exec ceiling 15 MB)\n" "$(stat -c%s src/retroarch.stripped)"
printf "  gpSP core (stripped) : %s bytes  (git %s)\n" \
   "$(stat -c%s cores-src/gpsp/gpsp_libretro.so)" "$GPSP_GIT_VERSION"
printf "  PCSX core (stripped) : %s bytes  (fresh cores-src build)\n" \
   "$(stat -c%s "$PCSX_CORE")"
printf "  testcore .so        : %s bytes\n" "$(stat -c%s testcore/testcore_libretro.so)"
printf "  mnt_app image       : %s bytes -> build/mnt_app\n" "$(du -sb "$MNT_STAGE" | cut -f1)"
printf "  SD-card image       : %s bytes -> build/sd_card\n" "$(du -sb "$SD_STAGE" | cut -f1)"
printf "  Ozone/Audi assets   : %s files (mnt_app)\n" "$(find "$APP_DIR/assets" -type f | wc -l)"
printf "  joypad autoconfig   : %s profiles (mnt_app)\n" "$(find "$APP_DIR/autoconfig/qnx" -type f -name "*.cfg" | wc -l)"
printf "  HID rumble          : %s profiles (mnt_app)\n" "$(find "$APP_DIR/rumble/qnx" -type f -name "*.cfg" | wc -l)"
printf "  core info           : %s files (app seed + writable SD copy)\n" "$(find "$APP_DIR/info" -type f -name "*.info" | wc -l)"
printf "  game databases      : %s files / %s bytes (SD)\n" \
   "$(find "$SD_DIR/database/rdb" -type f -name "*.rdb" | wc -l)" \
   "$(du -sb "$SD_DIR/database/rdb" | cut -f1)"
printf "  cheat database      : %s files / %s bytes (SD)\n" \
   "$(find "$SD_DIR/cheats" -type f -name "*.cht" | wc -l)" \
   "$(du -sb "$SD_DIR/cheats" | cut -f1)"
printf "  PS1/GBA content     : %s / %s files (SD)\n" \
   "$(find "$SD_DIR/ps1" -type f | wc -l)" \
   "$(find "$SD_DIR/gba" -type f | wc -l)"
printf "  box art             : %s files (SD)\n" "$(find "$SD_DIR/thumbnails" -type f -name "*.png" | wc -l)"
arm-unknown-nto-qnx6.5.0eabi-readelf -h src/retroarch.stripped | grep -E "Machine|Flags" | sed "s/^/  frontend /"
'
