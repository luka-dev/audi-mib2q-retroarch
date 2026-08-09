#!/bin/sh
# SD-direct autoExec (toolbox-style). Removes the bad HMI override jar so LSD boots stock.
# Harmless if nothing runs it. LF line endings, QNX sh.
mount -uw /net/mmx/fs/sda0 2>/dev/null
mount -uw /fs/sda0 2>/dev/null
exec > /fs/sda0/autoExec_log.txt 2>&1
echo "=== RA autoExec start ==="
removed=0
for APP in /net/mmx/mnt/app /mnt/app; do
    JARS="$APP/eso/hmi/lsd/jars"
    [ -d "$JARS" ] || { echo "skip $JARS"; continue; }
    mount -uw "$APP" 2>/dev/null
    echo "jars in $JARS:"; ls -l "$JARS" 2>&1 | grep -i '\.jar'
    if [ -f "$JARS/ra_mhi2q.jar" ]; then
        rm -f "$JARS/ra_mhi2q.jar" && { echo "OK removed $JARS/ra_mhi2q.jar"; removed=1; } || echo "FAIL rm"
    fi
done
echo "RESULT removed=$removed"
echo "=== RA autoExec done ==="
sync
