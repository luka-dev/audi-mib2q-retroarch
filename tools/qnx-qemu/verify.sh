#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$root"

status=0
while read -r expected path; do
    [ -n "$expected" ] || continue
    if [ ! -f "$path" ]; then
        echo "MISSING $path" >&2
        status=1
        continue
    fi
    actual=$(shasum -a 256 "$path" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "MISMATCH $path: $actual != $expected" >&2
        status=1
    else
        echo "OK $path"
    fi
done < runtime.sha256

exit "$status"
