#!/bin/sh
# Build and run an isolated macOS RetroArch frontend for UI development.
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR="$PROJECT_ROOT/src"
LOCAL_DIR="$PROJECT_ROOT/build/macos-local"
LOCAL_BIN="$LOCAL_DIR/bin/retroarch"
TEST_CORE="$LOCAL_DIR/cores/testcore_libretro.dylib"
GPSP_SRC="$PROJECT_ROOT/cores-src/gpsp"
GPSP_CORE="$LOCAL_DIR/cores/gpsp_libretro.dylib"

if [ "$(uname -s)" != "Darwin" ]; then
   echo "run-macos.sh must be run on macOS" >&2
   exit 1
fi

mkdir -p "$LOCAL_DIR/bin" \
         "$LOCAL_DIR/cores" \
         "$LOCAL_DIR/config" \
         "$LOCAL_DIR/games/Nintendo - Game Boy Advance" \
         "$LOCAL_DIR/games-slot2/Nintendo - Game Boy Advance" \
         "$LOCAL_DIR/games-slot2/Sony - PlayStation" \
         "$LOCAL_DIR/playlists" \
         "$LOCAL_DIR/thumbnails" \
         "$LOCAL_DIR/logs" \
         "$LOCAL_DIR/screenshots" \
         "$LOCAL_DIR/saves" \
         "$LOCAL_DIR/states" \
         "$LOCAL_DIR/system"

cd "$SRC_DIR"
if [ ! -f config.mk ] \
      || ! grep -q '^OS = Darwin$' config.mk \
      || ! grep -q '^HAVE_COCOA = 1$' config.mk \
      || ! grep -q '^HAVE_OZONE = 1$' config.mk \
      || ! grep -q '^HAVE_XMB = 1$' config.mk \
      || ! grep -q '^HAVE_COCOA_METAL = 1$' config.mk \
      || ! grep -q '^HAVE_METAL = 1$' config.mk \
      || ! grep -q '^HAVE_HID = 1$' config.mk \
      || ! grep -q '^HAVE_IOHIDMANAGER = 1$' config.mk \
      || ! grep -q '^HAVE_LIBUSB = 0$' config.mk \
      || ! grep -q '^HAVE_VULKAN = 0$' config.mk \
      || ! grep -q '^HAVE_SDL2 = 0$' config.mk \
      || ! grep -q '^HAVE_SDL3 = 0$' config.mk \
      || ! grep -q '^HAVE_FFMPEG = 0$' config.mk; then
   ./configure \
      --prefix="$LOCAL_DIR" \
      --with-assets_dir="$PROJECT_ROOT/pkg/assets" \
      --disable-qt \
      --enable-ozone \
      --enable-xmb \
      --enable-metal \
      --enable-hid \
      --disable-libusb \
      --disable-vulkan \
      --disable-sdl3 \
      --disable-sdl2 \
      --disable-ffmpeg
fi

BUILD_JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
# The repository ships the compiled stock Metal shader library, so local UI
# builds do not require Xcode's optional Metal compiler component.
make -j"$BUILD_JOBS" TARGET="$LOCAL_BIN" METALLIB=
cp "$SRC_DIR/pkg/apple/OSX/Resources/default.metallib" \
   "$LOCAL_DIR/bin/default.metallib"

if [ ! -f "$GPSP_CORE" ] \
      || find "$GPSP_SRC" -type f \
         \( -name '*.c' -o -name '*.cc' -o -name '*.h' -o -name '*.S' \
            -o -name 'Makefile' -o -name 'Makefile.common' \) \
         -newer "$GPSP_CORE" -print -quit | grep -q .; then
   make -C "$GPSP_SRC" clean platform=osx
   make -C "$GPSP_SRC" -j"$BUILD_JOBS" platform=osx
   cp "$GPSP_SRC/gpsp_libretro.dylib" "$GPSP_CORE"
fi

if [ ! -f "$TEST_CORE" ] \
      || [ "$PROJECT_ROOT/testcore/testcore_libretro.c" -nt "$TEST_CORE" ]; then
   clang -std=gnu99 -O2 -fPIC -dynamiclib \
      -I"$SRC_DIR/libretro-common/include" \
      "$PROJECT_ROOT/testcore/testcore_libretro.c" \
      -o "$TEST_CORE"
fi

cd "$PROJECT_ROOT"
export RA_CONTENT_RULES="$PROJECT_ROOT/macos-content-rules.cfg"
exec "$LOCAL_BIN" \
   --config="$PROJECT_ROOT/macos-test.cfg" \
   --log-file="$LOCAL_DIR/logs/retroarch.log" \
   --verbose \
   "$@"
