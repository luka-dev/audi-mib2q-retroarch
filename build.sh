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
      rm -f cores-src/gpsp/gpsp_libretro.so
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

echo ">> building testcore_libretro.so…"
cd /src
arm-unknown-nto-qnx6.5.0eabi-gcc -std=gnu99 -O2 -fPIC -shared \
   -include stddef.h -Isrc/libretro-common/include \
   testcore/testcore_libretro.c -o testcore/testcore_libretro.so 2>/dev/null

echo ">> staging frontend, test core and external runtime resources…"
[ -f pkg/assets/ozone/regular.ttf ] || {
   echo "!! missing external Ozone assets in pkg/assets/ozone"
   exit 1
}
for _required in \
   pkg/info/SOURCE.txt \
   pkg/database/rdb/SOURCE.txt \
   pkg/cheats/SOURCE.txt \
   pkg/rumble/qnx/SOURCE.txt; do
   [ -f "$_required" ] || { echo "!! missing external resource manifest: $_required"; exit 1; }
done
cp src/retroarch.stripped pkg/stage/root_retroarch/retroarch
cp cores-src/gpsp/gpsp_libretro.so \
   pkg/stage/root_retroarch/cores/gpsp_libretro.so
cp testcore/testcore_libretro.so pkg/stage/root_retroarch/cores/testcore_libretro.so
cp pkg/retroarch.cfg pkg/ra.sh pkg/content-rules.cfg pkg/stage/root_retroarch/
mkdir -p pkg/stage/hmi_jars
cp lsd_patch/ra_mhi2q.jar pkg/stage/hmi_jars/ra_mhi2q.jar
# Appimg contains only executable/runtime files and the initial configuration.
# All large or replaceable resources are a separate SD-card payload.
rm -rf pkg/stage/root_retroarch/info \
       pkg/stage/root_retroarch/database \
       pkg/stage/root_retroarch/cheats \
       pkg/stage/root_retroarch/shaders \
       pkg/stage/root_retroarch/rumble \
       pkg/stage/root_retroarch/autoconfig \
       pkg/stage/root_retroarch/assets
rm -rf pkg/stage/sd/retroarch
mkdir -p pkg/stage/sd/retroarch/config \
         pkg/stage/sd/retroarch/assets \
         pkg/stage/sd/retroarch/autoconfig/qnx \
         pkg/stage/sd/retroarch/info \
         pkg/stage/sd/retroarch/database/rdb \
         pkg/stage/sd/retroarch/cheats \
         pkg/stage/sd/retroarch/rumble/qnx \
         pkg/stage/sd/retroarch/roms \
         pkg/stage/sd/retroarch/ps1 \
         pkg/stage/sd/retroarch/gba \
         pkg/stage/sd/retroarch/saves \
         pkg/stage/sd/retroarch/states \
         pkg/stage/sd/retroarch/system \
         pkg/stage/sd/retroarch/playlists \
         pkg/stage/sd/retroarch/thumbnails \
         pkg/stage/sd/retroarch/logs \
         pkg/stage/sd/retroarch/screenshots \
         pkg/stage/sd/retroarch/downloads \
         pkg/stage/sd/retroarch/filters/audio \
         pkg/stage/sd/retroarch/filters/video \
         pkg/stage/sd/retroarch/wallpapers \
         pkg/stage/sd/retroarch/overlays/keyboards
cp pkg/sd/retroarch/RESOURCES.txt pkg/stage/sd/retroarch/
cp pkg/sd/retroarch/config/retroarch.cfg \
   pkg/stage/sd/retroarch/config/retroarch.cfg
cp -R pkg/assets/. pkg/stage/sd/retroarch/assets/
cp pkg/autoconfig/qnx/*.cfg pkg/autoconfig/qnx/COPYING \
   pkg/autoconfig/qnx/SOURCE.txt pkg/stage/sd/retroarch/autoconfig/qnx/
cp pkg/info/*.info pkg/info/COPYING pkg/info/SOURCE.txt \
   pkg/stage/sd/retroarch/info/
cp pkg/database/rdb/*.rdb pkg/database/rdb/COPYING \
   pkg/database/rdb/SOURCE.txt pkg/stage/sd/retroarch/database/rdb/
cp -R pkg/cheats/. pkg/stage/sd/retroarch/cheats/
cp pkg/rumble/qnx/*.cfg pkg/rumble/qnx/SOURCE.txt \
   pkg/stage/sd/retroarch/rumble/qnx/
if [ -d pkg/sd/retroarch/thumbnails ]; then
   cp -R pkg/sd/retroarch/thumbnails/. pkg/stage/sd/retroarch/thumbnails/
fi
chmod 755 pkg/stage/root_retroarch/retroarch pkg/stage/root_retroarch/ra.sh
chmod 644 pkg/stage/root_retroarch/cores/testcore_libretro.so \
   pkg/stage/root_retroarch/cores/gpsp_libretro.so \
   pkg/stage/root_retroarch/retroarch.cfg \
   pkg/stage/root_retroarch/content-rules.cfg
find pkg/stage/sd/retroarch -type f -exec chmod 644 {} +

echo
echo "=== artifacts ==="
printf "  retroarch (stripped): %s bytes  (exec ceiling 15 MB)\n" "$(stat -c%s src/retroarch.stripped)"
printf "  gpSP core (stripped) : %s bytes  (git %s)\n" \
   "$(stat -c%s cores-src/gpsp/gpsp_libretro.so)" "$GPSP_GIT_VERSION"
printf "  testcore .so        : %s bytes\n" "$(stat -c%s testcore/testcore_libretro.so)"
printf "  appimg payload      : binary + core + libs + initial config only\n"
printf "  Ozone assets (SD)   : %s files\n" "$(find pkg/stage/sd/retroarch/assets/ozone -type f | wc -l)"
printf "  joypad config (SD)  : %s files\n" "$(find pkg/stage/sd/retroarch/autoconfig/qnx -type f -name "*.cfg" | wc -l)"
printf "  core info (SD)      : %s files\n" "$(find pkg/stage/sd/retroarch/info -type f -name "*.info" | wc -l)"
printf "  game databases      : %s files / %s bytes (external)\n" \
   "$(find pkg/stage/sd/retroarch/database/rdb -type f -name "*.rdb" | wc -l)" \
   "$(du -sb pkg/stage/sd/retroarch/database/rdb | cut -f1)"
printf "  cheat database      : %s files / %s bytes (external)\n" \
   "$(find pkg/stage/sd/retroarch/cheats -type f -name "*.cht" | wc -l)" \
   "$(du -sb pkg/stage/sd/retroarch/cheats | cut -f1)"
printf "  HID rumble (SD)     : %s files\n" "$(find pkg/stage/sd/retroarch/rumble/qnx -type f -name "*.cfg" | wc -l)"
printf "  box art (SD)        : %s files\n" "$(find pkg/stage/sd/retroarch/thumbnails -type f -name "*.png" | wc -l)"
arm-unknown-nto-qnx6.5.0eabi-readelf -h src/retroarch.stripped | grep -E "Machine|Flags" | sed "s/^/  frontend /"
'
