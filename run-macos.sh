#!/bin/sh
# Build and run an isolated 1024x480 macOS RetroArch UI test.
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_VERSIONS="$PROJECT_ROOT/VENDORED_SOURCES.env"
[ -f "$SOURCE_VERSIONS" ] || {
   echo "missing vendored source manifest: $SOURCE_VERSIONS" >&2
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
SRC_DIR="$PROJECT_ROOT/src"
OUT_DIR=${RA_MACOS_OUT:-"$PROJECT_ROOT/out/macos-test"}
APP_DIR="$OUT_DIR/RetroArchTest.app"
LOCAL_BIN="$APP_DIR/Contents/MacOS/retroarch"
GPSP_SRC="$PROJECT_ROOT/cores-src/gpsp"
PCSX_SRC="$PROJECT_ROOT/cores-src/pcsx_rearmed"
MUPEN_SRC="$PROJECT_ROOT/cores-src/mupen64plus_next"
GPSP_CORE="$OUT_DIR/cores/gpsp_libretro.dylib"
PCSX_CORE="$OUT_DIR/cores/pcsx_rearmed_libretro.dylib"
MUPEN_CORE="$OUT_DIR/cores/mupen64plus_next_libretro.dylib"
CONFIG_CACHE="$OUT_DIR/obj/frontend-config"

if [ "$(uname -s)" != Darwin ]; then
   echo "run-macos.sh must be run on macOS" >&2
   exit 1
fi

mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources" \
         "$OUT_DIR/cores" "$OUT_DIR/info" \
         "$OUT_DIR/obj/frontend" \
         "$OUT_DIR/config/remaps" "$OUT_DIR/playlists" "$OUT_DIR/logs" \
         "$OUT_DIR/screenshots" "$OUT_DIR/saves" "$OUT_DIR/states"

# Configure in the source tree because upstream RetroArch does not support a
# separate configure directory. All heavy objects and final products still go
# to out/macos-test; the three generated config files are removed before run.
cd "$SRC_DIR"
if [ -f "$CONFIG_CACHE/config.mk" ] \
      && grep -q '^OS = Darwin$' "$CONFIG_CACHE/config.mk" \
      && grep -q '^HAVE_OZONE = 1$' "$CONFIG_CACHE/config.mk" \
      && grep -q '^HAVE_XMB = 0$' "$CONFIG_CACHE/config.mk" \
      && grep -q '^HAVE_7ZIP = 0$' "$CONFIG_CACHE/config.mk"; then
   cp -p "$CONFIG_CACHE/config.h" "$CONFIG_CACHE/config.log" \
      "$CONFIG_CACHE/config.mk" "$SRC_DIR/"
else
   ./configure \
      --prefix="$OUT_DIR" \
      --with-assets_dir="$PROJECT_ROOT/pkg/assets" \
      --disable-qt \
      --enable-ozone \
      --disable-xmb \
      --enable-metal \
      --enable-hid \
      --disable-libusb \
      --disable-vulkan \
      --disable-sdl3 \
      --disable-sdl2 \
      --disable-7zip \
      --disable-ffmpeg
   mkdir -p "$CONFIG_CACHE"
   cp -p config.h config.log config.mk "$CONFIG_CACHE/"
fi

BUILD_JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
make -j"$BUILD_JOBS" TARGET="$LOCAL_BIN" \
   GIT_VERSION="$RETROARCH_GIT_VERSION" \
   OBJDIR_BASE="$OUT_DIR/obj/frontend" METALLIB=
cp "$SRC_DIR/pkg/apple/OSX/Resources/default.metallib" \
   "$APP_DIR/Contents/Resources/default.metallib"
cp "$PROJECT_ROOT/pkg/macos/Info.plist" "$APP_DIR/Contents/Info.plist"
clang -std=c99 -Os "$PROJECT_ROOT/pkg/macos/launcher.c" \
   -o "$APP_DIR/Contents/MacOS/RetroArchTest"
cp "$PROJECT_ROOT/pkg/info/gpsp_libretro.info" \
   "$PROJECT_ROOT/pkg/info/pcsx_rearmed_libretro.info" "$OUT_DIR/info/"
cp "$PROJECT_ROOT/pkg/info/mupen64plus_next_gles2_libretro.info" \
   "$OUT_DIR/info/mupen64plus_next_libretro.info"

if [ ! -f "$GPSP_CORE" ] || find "$GPSP_SRC" -type f \
      \( -name '*.c' -o -name '*.cc' -o -name '*.h' -o -name '*.S' \
         -o -name 'Makefile' -o -name 'Makefile.common' \) \
      -newer "$GPSP_CORE" -print -quit | grep -q .; then
   make -C "$GPSP_SRC" clean platform=osx \
      GIT_VERSION="$GPSP_GIT_VERSION" >/dev/null
   make -C "$GPSP_SRC" -j"$BUILD_JOBS" platform=osx \
      GIT_VERSION="$GPSP_GIT_VERSION"
   cp "$GPSP_SRC/gpsp_libretro.dylib" "$GPSP_CORE"
   make -C "$GPSP_SRC" clean platform=osx \
      GIT_VERSION="$GPSP_GIT_VERSION" >/dev/null
fi

if [ ! -f "$PCSX_CORE" ] || find "$PCSX_SRC" -type f \
      \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.S' \
         -o -name 'Makefile' -o -name 'Makefile.libretro' \) \
      -newer "$PCSX_CORE" -print -quit | grep -q .; then
   make -C "$PCSX_SRC" -f Makefile.libretro clean platform=osx \
      GIT_VERSION="$PCSX_GIT_VERSION" >/dev/null
   make -C "$PCSX_SRC" -f Makefile.libretro -j"$BUILD_JOBS" platform=osx \
      GIT_VERSION="$PCSX_GIT_VERSION"
   cp "$PCSX_SRC/pcsx_rearmed_libretro.dylib" "$PCSX_CORE"
   make -C "$PCSX_SRC" -f Makefile.libretro clean platform=osx \
      GIT_VERSION="$PCSX_GIT_VERSION" >/dev/null
fi

if [ ! -f "$MUPEN_CORE" ] || find "$MUPEN_SRC" -type f \
      \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.S' \
         -o -name 'Makefile' -o -name 'Makefile.common' \) \
      -newer "$MUPEN_CORE" -print -quit | grep -q .; then
   make -C "$MUPEN_SRC" clean platform=osx \
      GIT_VERSION="$MUPEN_GIT_VERSION" \
      HAVE_PARALLEL_RSP=0 HAVE_PARALLEL_RDP=0 LLE=0 >/dev/null
   make -C "$MUPEN_SRC" -j"$BUILD_JOBS" platform=osx \
      GIT_VERSION="$MUPEN_GIT_VERSION" \
      HAVE_PARALLEL_RSP=0 HAVE_PARALLEL_RDP=0 LLE=0
   cp "$MUPEN_SRC/mupen64plus_next_libretro.dylib" "$MUPEN_CORE"
   make -C "$MUPEN_SRC" clean platform=osx \
      GIT_VERSION="$MUPEN_GIT_VERSION" \
      HAVE_PARALLEL_RSP=0 HAVE_PARALLEL_RDP=0 LLE=0 >/dev/null
fi

rm -f "$SRC_DIR/config.h" "$SRC_DIR/config.log" "$SRC_DIR/config.mk"

cd "$PROJECT_ROOT"
exec open -n -W "$APP_DIR" --args "$@"
