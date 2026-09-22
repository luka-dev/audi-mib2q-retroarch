#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QNX="$ROOT/../qnx-65-sdp-docker/host-scripts/qnx-run.sh"
OUT="$ROOT/build/qnx_gsl_probe"
SYMBOLS="$ROOT/build/qnx_gsl_probe.symbols"

mkdir -p "$ROOT/build"

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fno-omit-frame-pointer \
   -Wall -Wextra -Wshadow -Wformat=2 \
   -I/src/tools/qnx-gsl-port \
   /src/tools/qnx-gsl-port/qnx_gsl_probe.c \
   -o /src/build/qnx_gsl_probe.symbols

cp "$SYMBOLS" "$OUT"
"$QNX" arm-unknown-nto-qnx6.5.0eabi-strip /src/build/qnx_gsl_probe

echo ">> built $OUT"
ls -lh "$OUT" "$SYMBOLS"
shasum -a 256 "$OUT" "$SYMBOLS"
