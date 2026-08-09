#!/bin/sh
# Build libdsiaudioprovider.so (LD_PRELOAD graft) for QNX 6.5 armle-v7.
# Mounts Tools/ (not just probe/) so comm_abi.h from qnx-carplay-emu is reachable.
set -eu
TOOLS=$(cd "$(dirname "$0")/../.." && pwd)
cd "$TOOLS"
exec ./qnx-65-sdp-docker/host-scripts/qnx-run.sh \
     bash /src/retroarch-qnx/probe/_build_in_container.sh
