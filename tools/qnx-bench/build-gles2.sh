#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QNX="$ROOT/../qnx-65-sdp-docker/host-scripts/qnx-run.sh"
OUT="$ROOT/build/qnx_gles2_bench"
SYMBOLS="$ROOT/build/qnx_gles2_bench.symbols"

mkdir -p "$ROOT/build"

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c99 -O2 -g -fno-omit-frame-pointer \
   -Wall -Wextra -Wshadow -Wformat=2 \
   /src/tools/qnx-bench/qnx_gles2_bench.c \
   -o /src/build/qnx_gles2_bench.symbols \
   -Wl,--as-needed -lEGL -lGLESv2 -lm

cp "$SYMBOLS" "$OUT"
"$QNX" arm-unknown-nto-qnx6.5.0eabi-strip /src/build/qnx_gles2_bench

echo ">> built $OUT"
ls -lh "$OUT" "$SYMBOLS"
shasum -a 256 "$OUT" "$SYMBOLS"
