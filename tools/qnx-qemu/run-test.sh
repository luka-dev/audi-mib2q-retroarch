#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$root/../.." && pwd)"
toolchain_image=${QNX_DOCKER_IMAGE:-qnx65-armv7-toolchain:latest}
timeout_seconds=${QNX_QEMU_TIMEOUT:-45}
qemu_cpus=${QNX_QEMU_CPUS:-4}
qemu_icount=${QNX_QEMU_ICOUNT:-1}
qemu_bin=${QNX_QEMU_BIN:-$root/runtime/qemu-system-arm}
input=${1:?usage: $0 path/to/qnx-arm-test-binary [single-test-argument]}
test_arg=${2:-}

if [[ -n "$test_arg" && ! "$test_arg" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "test argument may only contain letters, digits, underscore, or hyphen" >&2
    exit 2
fi
if [[ ! "$qemu_cpus" =~ ^[1-4]$ ]]; then
    echo "QNX_QEMU_CPUS must be between 1 and 4" >&2
    exit 2
fi
if [[ "$qemu_icount" != 0 && "$qemu_icount" != 1 ]]; then
    echo "QNX_QEMU_ICOUNT must be 0 or 1" >&2
    exit 2
fi

if [[ "$input" != /* ]]; then
    input="$(cd "$(dirname "$input")" && pwd)/$(basename "$input")"
fi
[[ -f "$input" ]] || { echo "missing test binary: $input" >&2; exit 2; }
file "$input" | grep -q 'ELF 32-bit.*ARM' || {
    echo "not a QNX ARM ELF: $input" >&2
    exit 2
}

"$root/verify.sh" >/dev/null
[[ -x "$qemu_bin" ]] || { echo "missing QEMU executable: $qemu_bin" >&2; exit 2; }
docker image inspect "$toolchain_image" >/dev/null 2>&1 || {
    echo "missing Docker image: $toolchain_image" >&2
    exit 2
}

mkdir -p "$root/build"
cp "$input" "$root/build/test-bin"
chmod 0755 "$root/build/test-bin"

docker run --rm --platform=linux/amd64 \
    -v "$project_root":/src -w /src "$toolchain_image" \
    qcc -Vgcc_ntoarmv7le -O2 -Wall -Wextra \
    "-DQNX_TEST_ARG=\"$test_arg\"" \
    /src/tools/qnx-qemu/guest-runner.c \
    -o /src/tools/qnx-qemu/build/guest-runner \
    >"$root/build/guest-runner-build.log" 2>&1

docker run --rm --platform=linux/amd64 \
    -v "$project_root":/src -w /src "$toolchain_image" \
    mkifs -v /src/tools/qnx-qemu/test.build \
    /src/tools/qnx-qemu/build/test.ifs \
    >"$root/build/mkifs.log" 2>&1

serial="$root/build/serial.log"
host_log="$root/build/qemu.log"
ram_file="/tmp/retroarch-qnx-qemu-test.ram"
qemu_pid=
for old in "$serial" "$host_log" "$ram_file"; do
    [[ ! -e "$old" && ! -L "$old" ]] || /bin/unlink "$old"
done
cleanup() {
    if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    [[ ! -e "$ram_file" && ! -L "$ram_file" ]] || /bin/unlink "$ram_file"
}
trap cleanup EXIT

set +e
qemu_timing_args=()
if [[ "$qemu_icount" == 1 ]]; then
    qemu_timing_args=(-icount shift=auto,sleep=off)
fi
"$qemu_bin" \
    -L "$root/runtime/pc-bios" \
    -M virt,memory-backend=mem -m 1024 \
    -smp "$qemu_cpus" \
    -cpu cortex-a15,cntfrq=12500000 \
    "${qemu_timing_args[@]}" \
    -object memory-backend-file,id=mem,size=1024M,mem-path="$ram_file",share=on \
    -device "loader,file=$root/build/test.ifs,addr=0x40200000,force-raw=on,cpu-num=0" \
    -display none -serial "file:$serial" -monitor none \
    >"$host_log" 2>&1 &
qemu_pid=$!

for ((tick = 0; tick < timeout_seconds * 10; ++tick)); do
    if [[ -f "$serial" ]] && grep -q '__QNX_TEST_RC__=' "$serial"; then
        break
    fi
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.1
done

if kill -0 "$qemu_pid" 2>/dev/null; then
    kill "$qemu_pid" 2>/dev/null
fi
wait "$qemu_pid" 2>/dev/null
qemu_status=$?
qemu_pid=
set -e

sed -n '/retroarch-qnx MIB2Q QEMU test/,$p' "$serial" 2>/dev/null || true
guest_status=$(sed -n 's/.*__QNX_TEST_RC__=\([0-9][0-9]*\).*/\1/p' "$serial" | tail -1)
if [[ -z "$guest_status" ]]; then
    echo "guest did not return a test status within ${timeout_seconds}s (QEMU status $qemu_status)" >&2
    echo "serial: $serial" >&2
    echo "host:   $host_log" >&2
    exit 1
fi
if [[ "$guest_status" -ne 0 ]]; then
    echo "guest test failed: $guest_status" >&2
    exit "$guest_status"
fi
echo "QNX guest test: PASS"
