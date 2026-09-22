#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QNX="$ROOT/../qnx-65-sdp-docker/host-scripts/qnx-run.sh"
OUT="$ROOT/build/qnx-freedreno"

mkdir -p "$OUT"

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fPIC -fno-omit-frame-pointer \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
   -c /src/tools/qnx-freedreno/qfd_winsys.c \
   -o /src/build/qnx-freedreno/qfd_winsys.o

"$QNX" arm-unknown-nto-qnx6.5.0eabi-ar rcs \
   /src/build/qnx-freedreno/libqfd_winsys.a \
   /src/build/qnx-freedreno/qfd_winsys.o

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fPIC -fno-omit-frame-pointer \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
   -c /src/tools/qnx-freedreno/qfd_drmif_bridge.c \
   -o /src/build/qnx-freedreno/qfd_drmif_bridge.o

"$QNX" arm-unknown-nto-qnx6.5.0eabi-ar rcs \
   /src/build/qnx-freedreno/libqfd_drmif_bridge.a \
   /src/build/qnx-freedreno/qfd_drmif_bridge.o

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fno-omit-frame-pointer \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
   /src/tools/qnx-freedreno/qfd_winsys.c \
   /src/tools/qnx-freedreno/qfd_winsys_selftest.c \
   -o /src/build/qnx-freedreno/qfd_winsys_selftest.symbols

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fno-omit-frame-pointer \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
   /src/tools/qnx-freedreno/qfd_winsys.c \
   /src/tools/qnx-freedreno/qfd_info_probe.c \
   -o /src/build/qnx-freedreno/qfd_info_probe.symbols

for probe in qfd_mem_probe qfd_context_probe qfd_submit_probe \
             qfd_memwrite_probe qfd_triangle_probe; do
   "$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
      -std=c11 -O2 -g -fno-omit-frame-pointer \
      -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
      -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
      /src/tools/qnx-freedreno/qfd_winsys.c \
      "/src/tools/qnx-freedreno/$probe.c" \
      -o "/src/build/qnx-freedreno/$probe.symbols"
done

"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
   -std=c11 -O2 -g -fno-omit-frame-pointer \
   -Wall -Wextra -Werror -Wshadow -Wconversion -Wformat=2 \
   -I/src/tools/qnx-freedreno -I/src/tools/qnx-gsl-port \
   /src/tools/qnx-freedreno/qfd_winsys.c \
   /src/tools/qnx-freedreno/qfd_drmif_bridge.c \
   /src/tools/qnx-freedreno/qfd_drmif_probe.c \
   -o /src/build/qnx-freedreno/qfd_drmif_probe.symbols

cp "$OUT/qfd_winsys_selftest.symbols" "$OUT/qfd_winsys_selftest"
cp "$OUT/qfd_info_probe.symbols" "$OUT/qfd_info_probe"
for probe in qfd_mem_probe qfd_context_probe qfd_submit_probe \
             qfd_memwrite_probe qfd_triangle_probe qfd_drmif_probe; do
   cp "$OUT/$probe.symbols" "$OUT/$probe"
done
"$QNX" arm-unknown-nto-qnx6.5.0eabi-strip \
   /src/build/qnx-freedreno/qfd_winsys_selftest \
   /src/build/qnx-freedreno/qfd_info_probe \
   /src/build/qnx-freedreno/qfd_mem_probe \
   /src/build/qnx-freedreno/qfd_context_probe \
   /src/build/qnx-freedreno/qfd_submit_probe \
   /src/build/qnx-freedreno/qfd_memwrite_probe \
   /src/build/qnx-freedreno/qfd_triangle_probe \
   /src/build/qnx-freedreno/qfd_drmif_probe

echo ">> built QNX Freedreno transport probes"
file "$OUT/qfd_winsys.o" "$OUT/libqfd_winsys.a" \
   "$OUT/qfd_drmif_bridge.o" "$OUT/libqfd_drmif_bridge.a" \
   "$OUT/qfd_winsys_selftest" "$OUT/qfd_info_probe" \
   "$OUT/qfd_mem_probe" "$OUT/qfd_context_probe" \
   "$OUT/qfd_submit_probe" "$OUT/qfd_memwrite_probe" \
   "$OUT/qfd_triangle_probe" "$OUT/qfd_drmif_probe"
(
   cd "$OUT"
   shasum -a 256 libqfd_winsys.a libqfd_drmif_bridge.a \
      qfd_winsys_selftest qfd_info_probe qfd_mem_probe qfd_context_probe \
      qfd_submit_probe qfd_memwrite_probe qfd_triangle_probe qfd_drmif_probe \
      > SHA256SUMS
   cat SHA256SUMS
)
