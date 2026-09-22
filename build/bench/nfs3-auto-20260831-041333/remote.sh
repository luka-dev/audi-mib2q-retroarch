export PATH=/armle/usr/bin:/armle/bin:$PATH
nohup /mnt/app/root/retroarch/ra.sh '/fs/sda0/retroarch/ps1/Need for Speed III - Hot Pursuit/Need for Speed III - Hot Pursuit.cue' </dev/null    >/tmp/ra_bench_launch.log 2>&1 &
pid=
i=0
while [ $i -lt 30 ]; do
   sleep 1
   pid=`pidin -P retroarch 2>/dev/null | awk 'NR==2{print $1}'`
   [ -n "$pid" ] && break
   i=`expr $i + 1`
done
if [ -z "$pid" ]; then
   echo "REMOTE: retroarch never appeared"
   cat /tmp/ra_bench_launch.log 2>/dev/null
   exit 1
fi
echo "REMOTE: pid $pid"
settle=15
[ $settle -gt 0 ] && sleep $settle
if [ 20 -gt 0 ]; then
   echo "REMOTE: sampling"
   /tmp/ra_prof $pid 20 100 > /tmp/ra_bench_profile.txt 2>/dev/null
fi
rest=15
[ $rest -gt 0 ] && sleep $rest
echo "REMOTE: stopping"
slay -f -Q retroarch 2>/dev/null
sleep 3
echo "REMOTE: done"
