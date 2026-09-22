#!/usr/bin/env bash
# Build, deploy and run the standalone GLES2 benchmark on the connected HU.
#
# Hidden/offscreen run (does not switch the visible display context):
#   tools/qnx-bench/run-gles2.sh --scenario all
#
# Exact RetroArch display route. The supervising remote shell restores the
# previously buffered display context even if the benchmark child fails:
#   tools/qnx-bench/run-gles2.sh --route --scenario ppsspp_like --frames 60
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SSH_SCRIPT="${AUDI_SSH:-$ROOT/../../audi_ssh.sh}"
BINARY="$ROOT/build/qnx_gles2_bench"
REMOTE_BINARY=/tmp/qnx_gles2_bench
OUT_ROOT="$ROOT/build/gles2-bench"

route=0
build=1
bench_args=()

while [ $# -gt 0 ]; do
   case "$1" in
      --route)
         route=1
         bench_args+=("$1")
         shift ;;
      --no-build)
         build=0
         shift ;;
      --out)
         [ $# -ge 2 ] || { echo "--out needs a directory" >&2; exit 2; }
         OUT_ROOT="$2"
         shift 2 ;;
      *)
         case "$1" in
            *[!A-Za-z0-9_./=:+-]*)
               echo "unsupported character in benchmark argument: $1" >&2
               exit 2 ;;
         esac
         bench_args+=("$1")
         shift ;;
   esac
done

[ -x "$SSH_SCRIPT" ] || { echo "missing SSH helper: $SSH_SCRIPT" >&2; exit 1; }
if [ "$build" -eq 1 ]; then
   "$HERE/build-gles2.sh"
fi
[ -f "$BINARY" ] || { echo "missing benchmark binary: $BINARY" >&2; exit 1; }

running_ra="$($SSH_SCRIPT exec 'pidin -P retroarch 2>/dev/null' || true)"
if printf '%s\n' "$running_ra" | grep -q retroarch; then
   echo "RetroArch is running; close it before the standalone GPU benchmark." >&2
   exit 1
fi

echo ">> deploying $REMOTE_BINARY"
"$SSH_SCRIPT" exec "cat > $REMOTE_BINARY.tmp" < "$BINARY"
"$SSH_SCRIPT" exec "chmod 755 $REMOTE_BINARY.tmp && mv $REMOTE_BINARY.tmp $REMOTE_BINARY"

remote_args=""
for arg in "${bench_args[@]}"; do
   remote_args+=" '$arg'"
done

remote_env='export LD_LIBRARY_PATH=/mnt/app/root/retroarch/lib:/mnt/app/eso/lib:/mnt/app/armle/lib:/mnt/app/armle/usr/lib:/eso/lib:/eso/lib/factories:/armle/lib:/lib:/usr/lib:/proc/boot; export GRAPHICS_ROOT=/proc/boot/; export IPL_CONFIG_DIR=/etc/eso/production; unset EGL_PLATFORM;'

if [ "$route" -eq 1 ]; then
   # The wrapper, rather than the child, owns restoration. That also covers a
   # benchmark SIGSEGV: the remote shell remains alive and runs its EXIT trap.
   remote_command="$remote_env cleanup_context() { cd /etc/eso/production; /eso/bin/apps/dmdt sb 0 >/tmp/qnx_gles2_bench_restore.log 2>&1; }; trap cleanup_context 0 1 2 15; $REMOTE_BINARY --leave-routed$remote_args"
else
   remote_command="$remote_env $REMOTE_BINARY$remote_args"
fi

stamp="$(date +%Y%m%d-%H%M%S)"
out="$OUT_ROOT/run-$stamp"
mkdir -p "$out"
printf '%s\n' "$remote_command" > "$out/remote-command.txt"

echo ">> running benchmark (route=$route)"
set +e
"$SSH_SCRIPT" exec "$remote_command" > "$out/results.csv" 2> "$out/run.log"
status=$?
set -e

echo ">> exit $status; artifacts: $out"
sed -n '/GL renderer:/p;/driver control/p;/enabled driver control/p;/binning hint/p;/write-only rendering/p;/BEGIN /p;/END /p;/WARNING/p;/failed/p' "$out/run.log"
echo
sed -n '1,40p' "$out/results.csv"
exit "$status"
