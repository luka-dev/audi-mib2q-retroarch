#!/usr/bin/env bash
# Drive a full RetroArch session on the head unit and bring the evidence back.
#
# This runs the real frontend -- real EGL, real Adreno, real QSA audio -- not a
# headless stand-in, so the numbers describe what a player would actually get.
# The point is that it needs nobody at the head unit: launch, sample, stop,
# collect.
#
#   tools/qnx-bench/run-session.sh --content /fs/sda0/retroarch/ps1/... --seconds 60
#
# Getting past the title screen without hands is what save states are for.
# `savestate_auto_load` is already true on the card, so a `.state.auto` beside
# the content drops the session straight into a chosen scene. Make one once per
# game and every later run starts from the same place.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SSH_SCRIPT="${AUDI_SSH:-$ROOT/../../audi_ssh.sh}"

HOST="root@10.173.189.1"
PASS="harman_f"
SSH_OPTS=(-T -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa
          -oServerAliveInterval=10 -oLogLevel=ERROR)

content=""
seconds=60
profile_secs=0
profile_hz=100
tag="session"
core=""
out_root="$ROOT/build/bench"

while [ $# -gt 0 ]; do
   case "$1" in
      --content)  content="$2"; shift 2 ;;
      --core)     core="$2"; shift 2 ;;
      --seconds)  seconds="$2"; shift 2 ;;
      --profile)  profile_secs="$2"; shift 2 ;;
      --hz)       profile_hz="$2"; shift 2 ;;
      --tag)      tag="$2"; shift 2 ;;
      --out)      out_root="$2"; shift 2 ;;
      -h|--help)
         sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
         exit 0 ;;
      *) echo "unknown argument: $1" >&2; exit 2 ;;
   esac
done

[ -n "$content" ] || { echo "--content is required" >&2; exit 2; }

# RetroArch is built for dynamic cores, so it needs -L; passing content alone
# fails with "path is not set". The directory-to-core map already exists in
# content-rules.cfg (the playlist scanner uses it), so read it from there
# instead of keeping a second copy of the same knowledge here.
if [ -z "$core" ]; then
   media_dir="$(printf '%s' "$content" | sed -n 's#.*/retroarch/\([^/]*\)/.*#\1#p')"
   core_base="$(awk -v want="$media_dir" '
      /_directory[ \t]*=/ { split($1, a, "_"); dir[a[1]] = $NF }
      /_core[ \t]*=/      { split($1, a, "_"); cor[a[1]] = $NF }
      END { for (r in dir) if (dir[r] == "\"" want "\"") {
               gsub(/"/, "", cor[r]); print cor[r] } }
   ' "$ROOT/pkg/content-rules.cfg" 2>/dev/null | head -1)"
   [ -n "$core_base" ] || {
      echo "!! no core rule for media directory '$media_dir'; pass --core" >&2
      exit 2; }
   core="/mnt/app/root/retroarch/cores/$core_base.so"
fi
echo ">> core: $core"

# The head unit's ssh login gets a shorter PATH than the HMI does, and
# /armle/usr/bin -- which holds date, nohup, tail, wc -- is missing from it.
# ra.sh needs date; without this it starts, logs an empty timestamp and dies.
REMOTE_PATH='export PATH=/armle/usr/bin:/armle/bin:$PATH;'

sshx() { sshpass -p "$PASS" ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_PATH $*"; }

# Sample from the middle of the run so loading is not counted as gameplay.
if [ "$profile_secs" -gt 0 ] && [ "$seconds" -gt "$profile_secs" ]; then
   settle_secs=$(( (seconds - profile_secs) / 2 ))
else
   settle_secs=0
fi
rest_secs=$(( seconds - settle_secs - profile_secs ))
[ "$rest_secs" -lt 0 ] && rest_secs=0

stamp="$(date +%Y%m%d-%H%M%S)"
out="$out_root/$tag-$stamp"
mkdir -p "$out"

echo ">> stopping any running frontend"
sshx 'slay -f -Q retroarch 2>/dev/null; exit 0' || true

# Old logs are moved rather than deleted: attributing a window to the wrong
# session has cost us a wrong diagnosis before.
echo ">> parking previous logs"
sshx 'L=/fs/sda0/retroarch/logs; mkdir -p $L/previous 2>/dev/null;
      mv $L/retroarch__1970*.log $L/previous/ 2>/dev/null;
      rm -f /tmp/qsa_perf.log; exit 0' || true

# Everything from launch to shutdown happens inside ONE ssh session. Polling
# for the pid with a connection per second got the account locked out by the
# head unit's sshd, and it is rude besides.
echo ">> launching: $content  (${seconds}s, profile ${profile_secs}s)"
cat > "$out/remote.sh" <<REMOTE
export PATH=/armle/usr/bin:/armle/bin:\$PATH
nohup /mnt/app/root/retroarch/ra.sh -L '$core' '$content' </dev/null \
   >/tmp/ra_bench_launch.log 2>&1 &
pid=
i=0
while [ \$i -lt 30 ]; do
   sleep 1
   pid=\`pidin -P retroarch 2>/dev/null | awk 'NR==2{print \$1}'\`
   [ -n "\$pid" ] && break
   i=\`expr \$i + 1\`
done
if [ -z "\$pid" ]; then
   echo "REMOTE: retroarch never appeared"
   cat /tmp/ra_bench_launch.log 2>/dev/null
   exit 1
fi
echo "REMOTE: pid \$pid"
settle=$settle_secs
[ \$settle -gt 0 ] && sleep \$settle
if [ $profile_secs -gt 0 ]; then
   echo "REMOTE: sampling"
   /tmp/ra_prof \$pid $profile_secs $profile_hz > /tmp/ra_bench_profile.txt 2>/dev/null
fi
rest=$rest_secs
[ \$rest -gt 0 ] && sleep \$rest
echo "REMOTE: stopping"
slay -f -Q retroarch 2>/dev/null
sleep 3
echo "REMOTE: done"
REMOTE
sshpass -p "$PASS" ssh "${SSH_OPTS[@]}" "$HOST" 'sh -s' \
   < "$out/remote.sh" 2>&1 | sed 's/^/   /'

echo ">> collecting"
for f in /tmp/qsa_perf.log /fs/sda0/retroarch/logs/ra_run.log \
         /fs/sda0/retroarch/logs/ra_audio.log; do
   sshx "cat $f 2>/dev/null" </dev/null > "$out/$(basename "$f")" || true
done
newest="$(sshx 'ls /fs/sda0/retroarch/logs/retroarch__1970*.log 2>/dev/null' \
          </dev/null | tail -1 || true)"
if [ -n "$newest" ]; then
   sshx "cat '$newest'" </dev/null > "$out/retroarch.log" || true
fi
sshx 'cat /fs/sda0/retroarch/logs/ppsspp_perf_*.log 2>/dev/null' </dev/null \
   > "$out/ppsspp_perf.log" || true
sshx 'cat /tmp/ra_bench_profile.txt 2>/dev/null' </dev/null \
   > "$out/profile.txt" || true
find "$out" -size 0 -delete

echo
echo "=== QSA audio windows ==="
grep -h "QSA PERF" "$out/retroarch.log" 2>/dev/null || echo "  (none logged)"
echo
echo "=== crashes this session ==="
grep -hE "SIGSEGV|Malloc|Abort|terminate called" "$out/ra_run.log" 2>/dev/null \
   || echo "  none"
echo
echo ">> saved to $out"
ls -1 "$out"
