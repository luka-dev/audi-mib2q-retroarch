#!/bin/sh
# Build the phase-1 DSI audio factory probe for QNX 6.5 armle-v7.
# Phase 1 needs nothing but libc + dlfcn, so it builds with the plain toolchain -
# no reconstructed comm:: ABI, no firmware import libs. That is the whole point:
# it validates the foundation before any risky ABI is introduced.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
QNX="$HERE/../../qnx-65-sdp-docker/host-scripts/qnx-run.sh"

cd "$HERE"
"$QNX" arm-unknown-nto-qnx6.5.0eabi-gcc \
    -std=gnu99 -O2 -Wall -include stddef.h \
    dsi_audio_probe.c -o dsi_audio_probe
# note: no -ldl — on QNX 6.5 dlopen/dlsym live in libc

"$QNX" bash -c '
cd /src
arm-unknown-nto-qnx6.5.0eabi-strip dsi_audio_probe -o dsi_audio_probe.stripped 2>/dev/null || true
echo "=== built ==="
ls -l dsi_audio_probe*
echo "--- ABI ---"
arm-unknown-nto-qnx6.5.0eabi-readelf -h dsi_audio_probe | grep -E "Machine|Flags"
echo "--- NEEDED ---"
arm-unknown-nto-qnx6.5.0eabi-readelf -d dsi_audio_probe | grep NEEDED
'
echo
echo "Copy dsi_audio_probe to the unit and run it. It only READS - it constructs"
echo "no comm:: objects and makes no virtual calls, so a wrong guess can at worst"
echo "print a mismatch."
