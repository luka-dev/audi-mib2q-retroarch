#!/bin/sh
# Build RetroArch + production libretro cores for MHI2Q (QNX 6.5 armle-v7).
# Uses the consolidated toolchain image via ../qnx-65-sdp-docker/host-scripts/qnx-run.sh
# (mounts the retroarch-qnx dir as /src). Run from anywhere.
#
#   ./build.sh          # frontend + gpSP + PCSX-ReARMed + Mupen64Plus-Next
#   ./build.sh clean    # remove compiled/app products; preserve SD games/BIOS
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
QNX="$HERE/../qnx-65-sdp-docker/host-scripts/qnx-run.sh"
GPSP_SRC="$HERE/cores-src/gpsp"
MUPEN_SRC="$HERE/cores-src/mupen64plus_next"
SOURCE_VERSIONS="$HERE/VENDORED_SOURCES.env"

[ -f "$SOURCE_VERSIONS" ] || {
   echo "!! missing vendored source manifest: $SOURCE_VERSIONS" >&2
   exit 1
}
. "$SOURCE_VERSIONS"
: "${RETROARCH_SOURCE_COMMIT:?missing RETROARCH_SOURCE_COMMIT}"
: "${GPSP_SOURCE_COMMIT:?missing GPSP_SOURCE_COMMIT}"
: "${PCSX_REARMED_SOURCE_COMMIT:?missing PCSX_REARMED_SOURCE_COMMIT}"
: "${MUPEN64PLUS_NEXT_SOURCE_COMMIT:?missing MUPEN64PLUS_NEXT_SOURCE_COMMIT}"
RETROARCH_GIT_VERSION=$(printf '%.7s' "$RETROARCH_SOURCE_COMMIT")
GPSP_GIT_VERSION=$(printf '%.7s' "$GPSP_SOURCE_COMMIT")
PCSX_GIT_VERSION=$(printf '%.7s' "$PCSX_REARMED_SOURCE_COMMIT")
MUPEN_GIT_VERSION=$(printf '%.7s' "$MUPEN64PLUS_NEXT_SOURCE_COMMIT")

[ -f "$GPSP_SRC/Makefile" ] || {
   echo "!! missing gpSP source tree: $GPSP_SRC" >&2
   exit 1
}
[ -f "$MUPEN_SRC/Makefile" ] || {
   echo "!! missing Mupen64Plus-Next source tree: $MUPEN_SRC" >&2
   exit 1
}
if [ "${1:-}" = clean ]; then
   cd "$HERE" && "$QNX" env \
      RETROARCH_GIT_VERSION="$RETROARCH_GIT_VERSION" \
      GPSP_GIT_VERSION="$GPSP_GIT_VERSION" \
      PCSX_GIT_VERSION="$PCSX_GIT_VERSION" \
      MUPEN_GIT_VERSION="$MUPEN_GIT_VERSION" bash -c '
      cd /src
      rm -f src/griffin/griffin.o src/retroarch src/retroarch.stripped
      make -C cores-src/gpsp clean platform=qnx GIT_VERSION="$GPSP_GIT_VERSION"
      rm -f cores-src/gpsp/cpu_threaded.o cores-src/gpsp/arm/arm_stub.o
      make -C cores-src/pcsx_rearmed -f Makefile.libretro clean platform=qnx \
         GIT_VERSION="$PCSX_GIT_VERSION" \
         CC=arm-unknown-nto-qnx6.5.0eabi-gcc
      make -C cores-src/mupen64plus_next clean platform=qnx \
         GIT_VERSION="$MUPEN_GIT_VERSION" \
         CC=arm-unknown-nto-qnx6.5.0eabi-gcc \
         CXX=arm-unknown-nto-qnx6.5.0eabi-g++
      rm -f cores-src/gpsp/gpsp_libretro.so
      rm -f build/pcsx_rearmed_libretro.so build/mupen64plus_next_gles2_libretro.so
      rm -rf build/mnt_app
   '
   exit 0
fi

cd "$HERE"
echo ">> building MU1316 Java runtime injector…"
"$HERE/lsd_patch/build.sh"

"$QNX" env \
   RETROARCH_GIT_VERSION="$RETROARCH_GIT_VERSION" \
   GPSP_GIT_VERSION="$GPSP_GIT_VERSION" \
   PCSX_GIT_VERSION="$PCSX_GIT_VERSION" \
   MUPEN_GIT_VERSION="$MUPEN_GIT_VERSION" bash -c '
set -e
cd /src/src
echo ">> building retroarch (griffin, platform=qnx)…"
rm -f griffin/griffin.o retroarch retroarch.stripped
make -f Makefile.griffin platform=qnx \
   GIT_VERSION="$RETROARCH_GIT_VERSION" 2>&1 | \
   grep -E "Error|error:|undefined reference" || true
[ -f retroarch ] || { echo "!! link failed"; exit 1; }
arm-unknown-nto-qnx6.5.0eabi-strip retroarch -o retroarch.stripped

echo ">> building gpsp_libretro.so (clean, stable ARM interpreter)…"
cd /src/cores-src/gpsp
make clean platform=qnx GIT_VERSION="$GPSP_GIT_VERSION"
# The interpreter build no longer enumerates dynarec objects in `make clean`.
# Remove any leftovers from an older QNX dynarec build explicitly.
rm -f cpu_threaded.o arm/arm_stub.o
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
   GIT_VERSION="$PCSX_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc
make -j4 -f Makefile.libretro platform=qnx \
   GIT_VERSION="$PCSX_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc
arm-unknown-nto-qnx6.5.0eabi-strip pcsx_rearmed_libretro_qnx.so \
   -o /src/build/pcsx_rearmed_libretro.so

echo ">> building mupen64plus_next_gles2_libretro.so (ARM dynarec + GLES2)…"
cd /src/cores-src/mupen64plus_next
make clean platform=qnx GIT_VERSION="$MUPEN_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc \
   CXX=arm-unknown-nto-qnx6.5.0eabi-g++
make -j4 platform=qnx \
   GIT_VERSION="$MUPEN_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc \
   CXX=arm-unknown-nto-qnx6.5.0eabi-g++ \
   AR=arm-unknown-nto-qnx6.5.0eabi-ar \
   STRINGS=arm-unknown-nto-qnx6.5.0eabi-strings
arm-unknown-nto-qnx6.5.0eabi-strip \
   mupen64plus_next_gles2_libretro_qnx.so \
   -o /src/build/mupen64plus_next_gles2_libretro.so

cd /src
echo ">> staging deployable mnt_app + sd_card trees…"
[ -f pkg/assets/ozone/regular.ttf ] || {
   echo "!! missing external Ozone assets in pkg/assets/ozone"
   exit 1
}
[ -f pkg/assets/xmb/monochrome/png/default.png ] || {
   echo "!! missing Ozone icon dependency in pkg/assets/xmb/monochrome"
   exit 1
}
PCSX_CORE=build/pcsx_rearmed_libretro.so
MUPEN_CORE=build/mupen64plus_next_gles2_libretro.so
RUNTIME_LIBS=pkg/runtime-libs
for _required in \
   "$PCSX_CORE" \
   "$MUPEN_CORE" \
   "$RUNTIME_LIBS/libstdc++.so.6" \
   "$RUNTIME_LIBS/libhiddi.so.1" \
   "$RUNTIME_LIBS/SOURCE.txt" \
   lsd_patch/ra_mhi2q.jar \
   pkg/info/mupen64plus_next_gles2_libretro.info \
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
SD_PRESERVE=build/.sd-content-preserve

# The ready-to-deploy SD tree is also the canonical local content store. Keep
# games and BIOS across a rebuild, while recreating all factory/runtime state.
# If a previous build was interrupted after the move, recover it first.
if [ -d "$SD_PRESERVE" ]; then
   mkdir -p "$SD_DIR"
   for _content_dir in ps1 gba n64 roms system autoconfig; do
      if [ -d "$SD_PRESERVE/$_content_dir" ]; then
         if [ -d "$SD_DIR/$_content_dir" ]; then
            [ "$(find "$SD_DIR/$_content_dir" -print | wc -l)" -eq 1 ] || {
               echo "!! both live and preserved SD content exist: $_content_dir"
               exit 1
            }
            rmdir "$SD_DIR/$_content_dir"
         elif [ -e "$SD_DIR/$_content_dir" ]; then
            echo "!! non-directory SD content collision: $_content_dir"
            exit 1
         fi
         mv "$SD_PRESERVE/$_content_dir" "$SD_DIR/$_content_dir"
      fi
   done
   rmdir "$SD_PRESERVE"
fi

mkdir -p "$SD_PRESERVE"
for _content_dir in ps1 gba n64 roms system autoconfig; do
   if [ -d "$SD_DIR/$_content_dir" ]; then
      mv "$SD_DIR/$_content_dir" "$SD_PRESERVE/$_content_dir"
   fi
done

rm -rf "$MNT_STAGE" "$SD_STAGE"
mkdir -p "$APP_DIR/cores" "$APP_DIR/lib" \
         "$APP_DIR/assets/ozone" "$APP_DIR/assets/audi" \
         "$APP_DIR/assets/pkg" "$APP_DIR/assets/xmb/monochrome" \
         "$APP_DIR/autoconfig/qnx" \
         "$APP_DIR/rumble/qnx" "$APP_DIR/info" "$JAR_DIR"

# /mnt/app: complete immutable/safe application layer. UI and controller
# resources are kept here so RetroArch remains controllable with a blank SD.
cp src/retroarch.stripped "$APP_DIR/retroarch"
cp cores-src/gpsp/gpsp_libretro.so "$APP_DIR/cores/gpsp_libretro.so"
cp "$PCSX_CORE" "$APP_DIR/cores/pcsx_rearmed_libretro.so"
cp "$MUPEN_CORE" "$APP_DIR/cores/mupen64plus_next_gles2_libretro.so"
cp "$RUNTIME_LIBS/libstdc++.so.6" "$RUNTIME_LIBS/libhiddi.so.1" \
   "$RUNTIME_LIBS/SOURCE.txt" "$APP_DIR/lib/"
cp pkg/retroarch.cfg pkg/retroarch-core-options.cfg pkg/ra.sh \
   pkg/content-rules.cfg "$APP_DIR/"
cp pkg/assets/COPYING "$APP_DIR/assets/"
cp -R pkg/assets/ozone/. "$APP_DIR/assets/ozone/"
cp -R pkg/assets/audi/. "$APP_DIR/assets/audi/"
cp -R pkg/assets/pkg/. "$APP_DIR/assets/pkg/"
cp -R pkg/assets/xmb/monochrome/. "$APP_DIR/assets/xmb/monochrome/"
cp pkg/autoconfig/qnx/*.cfg pkg/autoconfig/qnx/COPYING \
   pkg/autoconfig/qnx/SOURCE.txt "$APP_DIR/autoconfig/qnx/"
cp pkg/rumble/qnx/*.cfg pkg/rumble/qnx/SOURCE.txt "$APP_DIR/rumble/qnx/"
cp pkg/info/gpsp_libretro.info pkg/info/pcsx_rearmed_libretro.info \
   pkg/info/mupen64plus_next_gles2_libretro.info \
   pkg/info/COPYING pkg/info/SOURCE.txt "$APP_DIR/info/"
cp lsd_patch/ra_mhi2q.jar "$JAR_DIR/ra_mhi2q.jar"
cp pkg/mnt_app.README.txt "$MNT_STAGE/README_INSTALL.txt"

# /fs/sda0: portable writable card. The config starts as an exact factory copy;
# ra.sh also creates it from appimg when a physically new/blank card is used.
mkdir -p "$SD_DIR/config/remaps" "$SD_DIR/info" \
         "$SD_DIR/database/rdb" "$SD_DIR/cheats" \
         "$SD_DIR/roms" "$SD_DIR/ps1" "$SD_DIR/gba" "$SD_DIR/n64" \
         "$SD_DIR/saves" "$SD_DIR/states" "$SD_DIR/system" \
         "$SD_DIR/autoconfig" \
         "$SD_DIR/playlists" "$SD_DIR/thumbnails" \
         "$SD_DIR/logs" "$SD_DIR/screenshots" "$SD_DIR/downloads" \
         "$SD_DIR/filters/audio" "$SD_DIR/filters/video" \
         "$SD_DIR/wallpapers" "$SD_DIR/overlays/keyboards"
for _content_dir in ps1 gba n64 roms system autoconfig; do
   if [ -d "$SD_PRESERVE/$_content_dir" ]; then
      rmdir "$SD_DIR/$_content_dir"
      mv "$SD_PRESERVE/$_content_dir" "$SD_DIR/$_content_dir"
   fi
done
rmdir "$SD_PRESERVE"
cp pkg/retroarch.cfg "$SD_DIR/config/retroarch.cfg"
cp pkg/retroarch-core-options.cfg \
   "$SD_DIR/config/retroarch-core-options.cfg"
cp pkg/sd/retroarch/RESOURCES.txt "$SD_DIR/"
cp pkg/sd/retroarch/RESOURCES.txt "$SD_STAGE/README_INSTALL.txt"
cp "$APP_DIR/info/"*.info pkg/info/COPYING pkg/info/SOURCE.txt "$SD_DIR/info/"
cp pkg/database/rdb/*.rdb pkg/database/rdb/COPYING \
   pkg/database/rdb/SOURCE.txt "$SD_DIR/database/rdb/"
cp -R pkg/cheats/. "$SD_DIR/cheats/"
if [ -d pkg/sd/retroarch/thumbnails ]; then
   cp -R pkg/sd/retroarch/thumbnails/. "$SD_DIR/thumbnails/"
fi

# macOS metadata has no place on FAT/QNX installation media.
find "$MNT_STAGE" "$SD_STAGE" -type f -name .DS_Store -delete

chmod 755 "$APP_DIR/retroarch" "$APP_DIR/ra.sh"
find "$APP_DIR" -type f ! -name retroarch ! -name ra.sh -exec chmod 644 {} +
find "$SD_DIR" -type f -exec chmod 644 {} +

echo
echo "=== artifacts ==="
printf "  retroarch (stripped): %s bytes  (exec ceiling 15 MB)\n" "$(stat -c%s "$APP_DIR/retroarch")"
printf "  gpSP core (stripped) : %s bytes  (git %s)\n" \
   "$(stat -c%s "$APP_DIR/cores/gpsp_libretro.so")" "$GPSP_GIT_VERSION"
printf "  PCSX core (stripped) : %s bytes  (fresh cores-src build)\n" \
   "$(stat -c%s "$APP_DIR/cores/pcsx_rearmed_libretro.so")"
printf "  Mupen core (stripped): %s bytes  (git %s; GLES2 + ARM dynarec)\n" \
   "$(stat -c%s "$APP_DIR/cores/mupen64plus_next_gles2_libretro.so")" \
   "$MUPEN_GIT_VERSION"
rm -f "$PCSX_CORE" "$MUPEN_CORE"
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
printf "  PS1/GBA/N64 content : %s / %s / %s files (SD)\n" \
   "$(find "$SD_DIR/ps1" -type f | wc -l)" \
   "$(find "$SD_DIR/gba" -type f | wc -l)" \
   "$(find "$SD_DIR/n64" -type f | wc -l)"
printf "  box art             : %s files (SD)\n" "$(find "$SD_DIR/thumbnails" -type f -name "*.png" | wc -l)"
arm-unknown-nto-qnx6.5.0eabi-readelf -h "$APP_DIR/retroarch" | grep -E "Machine|Flags" | sed "s/^/  frontend /"

# Deployment output is canonical. Do not leave compiler intermediates or
# duplicate binaries mixed into the source trees after a successful build.
rm -f src/griffin/griffin.o src/retroarch src/retroarch.stripped \
   cores-src/gpsp/gpsp_libretro.so
make -C cores-src/gpsp clean platform=qnx GIT_VERSION="$GPSP_GIT_VERSION" >/dev/null
rm -f cores-src/gpsp/cpu_threaded.o cores-src/gpsp/arm/arm_stub.o
make -C cores-src/pcsx_rearmed -f Makefile.libretro clean platform=qnx \
   GIT_VERSION="$PCSX_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc >/dev/null
make -C cores-src/mupen64plus_next clean platform=qnx \
   GIT_VERSION="$MUPEN_GIT_VERSION" \
   CC=arm-unknown-nto-qnx6.5.0eabi-gcc \
   CXX=arm-unknown-nto-qnx6.5.0eabi-g++ >/dev/null
'
