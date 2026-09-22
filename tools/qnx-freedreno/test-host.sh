#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build/qnx-freedreno-host"

mkdir -p "$OUT"

cc -std=c11 -O2 -g \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I"$HERE" -I"$ROOT/tools/qnx-gsl-port" \
   "$HERE/qfd_winsys.c" "$HERE/qfd_winsys_selftest.c" \
   -o "$OUT/qfd_winsys_selftest"

"$OUT/qfd_winsys_selftest"
