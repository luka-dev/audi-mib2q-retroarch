#!/usr/bin/env bash
# Run the ARM/QNX qfd tests in a temporary derivative of the verified MHI2Q
# QEMU boot IFS.  The source QEMU images and worktree are never modified.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU_REPO="$(cd "$ROOT/../mhi2q-qemu" && pwd)"
QFD_OUT="$ROOT/build/qnx-freedreno"
QEMU_OUT="$QEMU_REPO/out"
BASE_RECIPE="$QEMU_OUT/boot.build"
META="$QEMU_OUT/boot.ifs.meta"
QEMU="$QEMU_REPO/host/qemu/build/qemu-system-arm"
PROBE_BIN="${QFD_MESA_PROBE:-$QFD_OUT/mesa-static/qnx-freedreno-screen-probe}"
PROBE_PHYS=0xd1000000
TIMEOUT_SECONDS="${QFD_QEMU_TIMEOUT_SECONDS:-240}"
RUN_STAMP=$(date +%Y%m%d-%H%M%S)
ARTIFACT_DIR="$QFD_OUT/qemu-runs/run-$RUN_STAMP"

for path in "$QFD_OUT/qfd_winsys_selftest" "$QFD_OUT/qfd_info_probe" \
            "$QFD_OUT/qfd_mem_probe" "$QFD_OUT/qfd_context_probe" \
            "$QFD_OUT/qfd_submit_probe" "$QFD_OUT/qfd_memwrite_probe" \
            "$QFD_OUT/qfd_triangle_probe" \
            "$QFD_OUT/qfd_drmif_probe" \
            "$PROBE_BIN" \
            "$BASE_RECIPE" "$META" "$QEMU" "$QEMU_OUT/app.img" \
            "$QEMU_OUT/hmi.img" "$QEMU_OUT/system.img"; do
   [ -f "$path" ] || { echo "missing: $path" >&2; exit 1; }
done
[ -x "$QEMU" ] || { echo "QEMU is not executable: $QEMU" >&2; exit 1; }

# shellcheck disable=SC1090
. "$META"
: "${GPU_TREE:?missing GPU_TREE in boot.ifs.meta}"
: "${CORE_SH:?missing CORE_SH in boot.ifs.meta}"
: "${CORE_FACTORY_TREE:?missing CORE_FACTORY_TREE in boot.ifs.meta}"
: "${CORE_TRACING_TREE:?missing CORE_TRACING_TREE in boot.ifs.meta}"

QNX_DOCKER_IMAGE=qnx65-armv7-toolchain
if [ -f "$QEMU_REPO/config.env" ]; then
   configured_image=$(sed -n 's/^QNX_DOCKER_IMAGE="\{0,1\}\([^"[:space:]]*\)"\{0,1\}$/\1/p' \
      "$QEMU_REPO/config.env" | tail -1)
   [ -z "$configured_image" ] || QNX_DOCKER_IMAGE=$configured_image
fi
: "${QNX_DOCKER_IMAGE:=qnx65-armv7-toolchain}"

MMAP_PEER=$(find "$QEMU_OUT/unpacked" -type f -name libmmap_peer.so \
   2>/dev/null | sort | tail -1)
[ -n "$MMAP_PEER" ] && [ -f "$MMAP_PEER" ] || {
   echo "missing libmmap_peer.so in QEMU unpacked tree" >&2
   exit 1
}

QFD_QEMU_WORK=$(mktemp -d /tmp/qfd-qemu.XXXXXX)
QFD_QEMU_RAM="$QFD_QEMU_WORK/qnx.ram"
QFD_QEMU_LOG="$QFD_QEMU_WORK/qfd-qemu.log"
QFD_QEMU_PID=

# A Mesa-linked binary is too large for this BSP's boot image.  Put it in a
# disposable QNX6 filesystem loaded above the release images instead.  A 25%
# margin covers qnx6 metadata while keeping the blob inside 0xd1000000-d2000000.
PROBE_BYTES=$(wc -c < "$PROBE_BIN" | tr -d ' ')
PROBE_QFS_SIZE=$(( (PROBE_BYTES + PROBE_BYTES / 4 + 4095) / 4096 * 4096 ))
[ "$PROBE_QFS_SIZE" -le 16777216 ] || {
   echo "Mesa probe QFS would overlap reserved RAM: $PROBE_QFS_SIZE bytes" >&2
   exit 1
}
PROBE_QFS_SECTORS=$((PROBE_QFS_SIZE / 512))

cleanup()
{
   if [ -n "$QFD_QEMU_PID" ] && kill -0 "$QFD_QEMU_PID" 2>/dev/null; then
      kill "$QFD_QEMU_PID" 2>/dev/null || true
      wait "$QFD_QEMU_PID" 2>/dev/null || true
   fi
   if [ "${QFD_QEMU_KEEP:-0}" = 1 ]; then
      echo "kept QEMU artifacts: $QFD_QEMU_WORK"
   else
      rm -rf "$QFD_QEMU_WORK"
   fi
}
trap cleanup EXIT HUP INT TERM

cp "$QEMU_REPO/out/guest/startup-virt" "$QFD_QEMU_WORK/startup-virt"
cp "$QEMU_REPO/out/guest/libstartup.a" "$QFD_QEMU_WORK/libstartup.a"
printf '%s\n' kgsl-paravirt-v1 > "$QFD_QEMU_WORK/kgsl-paravirt.mode"
mkdir "$QFD_QEMU_WORK/qfd-probe-tree"
cp "$PROBE_BIN" "$QFD_QEMU_WORK/qfd-probe-tree/qnx-freedreno-screen-probe"
printf '[num_sectors=%s]\n/ = qfd-probe-tree/\n' "$PROBE_QFS_SECTORS" \
   > "$QFD_QEMU_WORK/qfd-probe.bld"

# Insert the tests after the KGSL endpoint is ready, then block the original
# startup recipe before Screen/HMI launch.  Append file mappings outside the
# startup script block.
awk -v qfd_probe_size="$PROBE_QFS_SIZE" '
   /\/proc\/boot\/devb-loopback loopback/ {
      sub(/ blk cache=/,
          " loopback ro,denyno,fd=/dev/shmem/qfd-probe.img,seek blk cache=")
   }
   { print }
   /waitfor \/dev\/shmem\/system.img 20/ {
      print "    /proc/boot/shmphys4 0xd1000000 " qfd_probe_size " /qfd-probe.img &"
      print "    waitfor /dev/shmem/qfd-probe.img 20"
   }
   /waitfor \/dev\/lo2 25/ {
      print "    waitfor /dev/lo3 25"
   }
   /\/proc\/boot\/mount3 -t qnx6/ {
      print "    /proc/boot/mkdir -p /qfd"
      print "    /proc/boot/mount6 -t qnx6 /dev/lo3 /qfd"
      print "    waitfor /qfd/qnx-freedreno-screen-probe 15"
   }
   /waitfor \/dev\/kgsl-3D 8/ {
      print "    display_msg \"=== QFD QEMU TEST BEGIN ===\""
      print "    /proc/boot/qfd_winsys_selftest"
      print "    LD_LIBRARY_PATH=/gpu:/proc/boot"
      print "    GRAPHICS_ROOT=/gpu"
      print "    /proc/boot/qfd_info_probe"
      print "    /proc/boot/qfd_mem_probe"
      print "    /proc/boot/qfd_context_probe"
      print "    /proc/boot/qfd_submit_probe --qemu-only"
      print "    /proc/boot/qfd_memwrite_probe --qemu-only"
      print "    /proc/boot/qfd_triangle_probe --qemu-only"
      print "    /proc/boot/qfd_drmif_probe --qemu-only"
      print "    /qfd/qnx-freedreno-screen-probe --qemu-only"
      print "    display_msg \"=== QFD QEMU TEST DONE ===\""
      print "    waitfor /dev/qfd-test-stop 600"
   }
' "$BASE_RECIPE" > "$QFD_QEMU_WORK/boot.build"
printf '%s\n' \
   '/proc/boot/qfd_winsys_selftest=/qfd/qfd_winsys_selftest' \
   '/proc/boot/qfd_info_probe=/qfd/qfd_info_probe' \
   '/proc/boot/qfd_mem_probe=/qfd/qfd_mem_probe' \
   '/proc/boot/qfd_context_probe=/qfd/qfd_context_probe' \
   '/proc/boot/qfd_submit_probe=/qfd/qfd_submit_probe' \
   '/proc/boot/qfd_memwrite_probe=/qfd/qfd_memwrite_probe' \
   '/proc/boot/qfd_triangle_probe=/qfd/qfd_triangle_probe' \
   '/proc/boot/qfd_drmif_probe=/qfd/qfd_drmif_probe' \
   '/proc/boot/shmphys4=/built/shmphys' \
   '/proc/boot/mount6=/opt/qnx650/target/qnx6/armle-v7/bin/mount' \
   >> "$QFD_QEMU_WORK/boot.build"

docker run --rm --platform=linux/amd64 \
   -v "$QEMU_REPO/guest/blobs":/blobs:ro \
   -v "$QEMU_REPO/out/guest":/built:ro \
   -v "$QFD_QEMU_WORK":/work \
   -v "$QFD_OUT":/qfd:ro \
   -v "$GPU_TREE":/gpu_vendor:ro \
   -v "$MMAP_PEER":/mmap_peer/libmmap_peer.so:ro \
   -v "$CORE_SH":/firmware_core/sh:ro \
   -v "$CORE_FACTORY_TREE":/core_factories:ro \
   -v "$CORE_TRACING_TREE":/core_tracing:ro \
   -w /work "$QNX_DOCKER_IMAGE" sh -e -c '
      export QNX_HOST=/opt/qnx650/host/linux/x86
      export QNX_TARGET=/opt/qnx650/target/qnx6
      export PATH=$QNX_HOST/usr/bin:$PATH
      export MAKEFLAGS=-I$QNX_TARGET/usr/include
      ln -sf /opt/qnx650 /usr/qnx650 2>/dev/null || true
      ln -sf /opt/tools/qcc/bin/qcc $QNX_HOST/usr/bin/qcc
      mkqnx6fsimg /work/qfd-probe.bld /work/qfd-probe.img
      mkifs -r / /work/boot.build /work/qfd-boot.ifs
      dumpifs -v /work/qfd-boot.ifs >/work/qfd-boot.list
      grep -q "proc/boot/qfd_winsys_selftest" /work/qfd-boot.list
      grep -q "proc/boot/qfd_info_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_mem_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_context_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_submit_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_memwrite_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_triangle_probe" /work/qfd-boot.list
      grep -q "proc/boot/qfd_drmif_probe" /work/qfd-boot.list
      grep -q "proc/boot/shmphys4" /work/qfd-boot.list
      grep -q "proc/boot/mount6" /work/qfd-boot.list
      grep -q "proc/boot/kgsl_resmgr" /work/qfd-boot.list
   '

[ "$(wc -c < "$QFD_QEMU_WORK/qfd-probe.img" | tr -d ' ')" \
   -eq "$PROBE_QFS_SIZE" ] || {
   echo "unexpected Mesa probe QFS size" >&2
   exit 1
}

echo ">> temporary IFS: $(wc -c < "$QFD_QEMU_WORK/qfd-boot.ifs" | tr -d ' ') bytes"
echo ">> Mesa probe QFS: $PROBE_QFS_SIZE bytes at $PROBE_PHYS"

"$QEMU" -M virt,memory-backend=mem -m 3072 \
   -cpu cortex-a15,cntfrq=12500000 \
   -icount shift=auto,sleep=off \
   -object memory-backend-file,id=mem,size=3072M,mem-path="$QFD_QEMU_RAM",share=on \
   -device loader,file="$QFD_QEMU_WORK/qfd-boot.ifs",addr=0x40200000,force-raw=on,cpu-num=0 \
   -device loader,file="$QEMU_OUT/app.img",addr=0x80000000,force-raw=on \
   -device loader,file="$QEMU_OUT/hmi.img",addr=0xc0000000,force-raw=on \
   -device loader,file="$QEMU_OUT/system.img",addr=0xd0000000,force-raw=on \
   -device loader,file="$QFD_QEMU_WORK/qfd-probe.img",addr="$PROBE_PHYS",force-raw=on \
   -netdev user,id=n0 -device usb-net,netdev=n0 \
   -display none -serial file:"$QFD_QEMU_LOG" -monitor none \
   >>"$QFD_QEMU_LOG" 2>&1 &
QFD_QEMU_PID=$!

elapsed=0
while kill -0 "$QFD_QEMU_PID" 2>/dev/null; do
   if grep -q '=== QFD QEMU TEST DONE ===' "$QFD_QEMU_LOG" 2>/dev/null; then
      break
   fi
   if [ "$elapsed" -ge "$TIMEOUT_SECONDS" ]; then
      echo "QEMU test timed out after ${TIMEOUT_SECONDS}s" >&2
      sed -n '/QFD QEMU TEST BEGIN/,$p' "$QFD_QEMU_LOG" >&2
      exit 124
   fi
   sleep 1
   elapsed=$((elapsed + 1))
done

if ! grep -q '=== QFD QEMU TEST DONE ===' "$QFD_QEMU_LOG"; then
   echo "QEMU exited before the qfd completion marker" >&2
   tail -120 "$QFD_QEMU_LOG" >&2
   exit 1
fi

sed -n '/=== QFD QEMU TEST BEGIN ===/,/=== QFD QEMU TEST DONE ===/p' \
   "$QFD_QEMU_LOG"
grep -q 'qfd_winsys_selftest: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_device_open_status=0' "$QFD_QEMU_LOG"
grep -q 'qfd_device_close_status=0' "$QFD_QEMU_LOG"
grep -q 'qfd_mem_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_context_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_submit_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_memwrite_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_triangle_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qfd_drmif_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'qnx_mesa_clear_submit: PASS' "$QFD_QEMU_LOG"
grep -q 'qnx_mesa_clear_readback: SKIP' "$QFD_QEMU_LOG"
grep -q 'qnx_screen_probe: PASS' "$QFD_QEMU_LOG"
grep -q 'CP_DRAW_INDX_2(0x36)' "$QFD_QEMU_LOG"
mkdir -p "$ARTIFACT_DIR"
cp "$QFD_QEMU_LOG" "$ARTIFACT_DIR/qfd-qemu.log"
cp "$QFD_QEMU_WORK/qfd-boot.list" "$ARTIFACT_DIR/qfd-boot.list"
(
   cd "$QFD_QEMU_WORK"
   shasum -a 256 qfd-boot.ifs > "$ARTIFACT_DIR/qfd-boot.sha256"
   shasum -a 256 qfd-probe.img > "$ARTIFACT_DIR/qfd-probe-qfs.sha256"
)
wc -c "$QFD_QEMU_WORK/qfd-boot.ifs" \
   > "$ARTIFACT_DIR/qfd-boot.size"
wc -c "$QFD_QEMU_WORK/qfd-probe.img" \
   > "$ARTIFACT_DIR/qfd-probe-qfs.size"
probe_hash=$(shasum -a 256 "$PROBE_BIN" | awk '{print $1}')
printf '%s  qnx-freedreno-screen-probe\n' "$probe_hash" \
   > "$ARTIFACT_DIR/mesa-probe.sha256"
cat > "$ARTIFACT_DIR/validation-summary.txt" <<'EOF'
qfd_winsys_selftest=PASS
qfd_mem_probe=PASS
qfd_context_probe=PASS
qfd_submit_probe=PASS
qfd_memwrite_probe=PASS
qfd_triangle_probe=PASS
qfd_drmif_probe=PASS
qnx_screen_name=FD320
qnx_screen_vendor=freedreno
qnx_mesa_clear_submit=PASS
qnx_mesa_clear_readback=SKIP_QEMU_HOST_FBO
qnx_screen_probe=PASS
qfd_qemu_validation=PASS
EOF
echo ">> qfd QEMU validation PASS (${elapsed}s)"
echo ">> artifacts: $ARTIFACT_DIR"
