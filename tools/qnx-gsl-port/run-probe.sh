#!/usr/bin/env bash
# Build, deploy, and run the non-submitting GSL ABI probe on the connected HU.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SSH_SCRIPT="${AUDI_SSH:-$ROOT/../../audi_ssh.sh}"
BINARY="$ROOT/build/qnx_gsl_probe"
REMOTE_BINARY=/tmp/qnx_gsl_probe
OUT_ROOT="$ROOT/build/gsl-probe"
build=1
probe_args=()

while [ $# -gt 0 ]; do
   case "$1" in
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
               echo "unsupported character in probe argument: $1" >&2
               exit 2 ;;
         esac
         probe_args+=("$1")
         shift ;;
   esac
done

[ -x "$SSH_SCRIPT" ] || { echo "missing SSH helper: $SSH_SCRIPT" >&2; exit 1; }
if [ "$build" -eq 1 ]; then
   "$HERE/build-probe.sh"
fi
[ -f "$BINARY" ] || { echo "missing probe binary: $BINARY" >&2; exit 1; }

running_ra="$($SSH_SCRIPT exec 'pidin -P retroarch 2>/dev/null' || true)"
if printf '%s\n' "$running_ra" | grep -q retroarch; then
   echo "RetroArch is running; close it before probing the GSL device." >&2
   exit 1
fi

echo ">> deploying $REMOTE_BINARY"
"$SSH_SCRIPT" exec "cat > $REMOTE_BINARY.tmp" < "$BINARY"
"$SSH_SCRIPT" exec "chmod 755 $REMOTE_BINARY.tmp && mv $REMOTE_BINARY.tmp $REMOTE_BINARY"

remote_args=""
for arg in "${probe_args[@]}"; do
   remote_args+=" '$arg'"
done

remote_env='export LD_LIBRARY_PATH=/mnt/app/root/retroarch/lib:/mnt/app/eso/lib:/mnt/app/armle/lib:/mnt/app/armle/usr/lib:/eso/lib:/eso/lib/factories:/armle/lib:/lib:/usr/lib:/proc/boot; export GRAPHICS_ROOT=/proc/boot/; export IPL_CONFIG_DIR=/etc/eso/production;'
remote_command="$remote_env $REMOTE_BINARY$remote_args"

stamp="$(date +%Y%m%d-%H%M%S)"
out="$OUT_ROOT/run-$stamp"
mkdir -p "$out"
printf '%s\n' "$remote_command" > "$out/remote-command.txt"

echo ">> running non-submitting probe"
set +e
"$SSH_SCRIPT" exec "$remote_command" > "$out/probe.log" 2> "$out/probe.stderr"
status=$?
set -e

echo ">> exit $status; artifacts: $out"
sed -n '1,120p' "$out/probe.log"
sed -n '1,80p' "$out/probe.stderr" >&2
exit "$status"
