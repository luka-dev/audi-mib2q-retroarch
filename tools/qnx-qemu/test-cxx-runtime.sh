#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$root/../.." && pwd)"
toolchain_image=${QNX_DOCKER_IMAGE:-qnx65-armv7-toolchain:latest}
output="$project_root/build/qnx-cxx-runtime-smoke"
runtime_dir="$project_root/build/qnx-static-runtime"

mkdir -p "$(dirname "$output")"
docker run --rm --platform=linux/amd64 \
    -v "$project_root":/src -w /src "$toolchain_image" \
    bash -c '
set -eu
/src/tools/qnx-qemu/prepare-static-cxx-runtime.sh /src/build/qnx-static-runtime
arm-unknown-nto-qnx6.5.0eabi-g++ \
    -O2 -g -std=gnu++17 \
    -include /src/cores-src/ppsspp/Common/QnxCompat.h \
    -B/opt/tools/gas-compat/bin \
    /src/tools/qnx-cxx-runtime-smoke.cpp \
    -L/src/build/qnx-static-runtime -lstdc++fs -lm -lc \
    -static-libstdc++ -static-libgcc \
    -o /src/build/qnx-cxx-runtime-smoke
'

needed=$(docker run --rm --platform=linux/amd64 \
    -v "$project_root":/src -w /src "$toolchain_image" \
    arm-unknown-nto-qnx6.5.0eabi-readelf -d \
    /src/build/qnx-cxx-runtime-smoke |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' |
    sort | tr '\n' ' ')

if [[ "$needed" != "libc.so.3 libm.so.2 " ]]; then
    echo "unexpected dynamic dependencies: $needed" >&2
    exit 1
fi

echo "dynamic dependencies: $needed"
"$root/run-test.sh" "$output"
