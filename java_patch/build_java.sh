#!/bin/bash
# Compatibility entry point.  The canonical build lives in lsd_patch/build.sh
# and enforces the exact MU1316 JCL plus the no-core-shadow safety checks.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_JAR="${OUT_JAR:-$SCRIPT_DIR/ra_games_hook.jar}"

OUT_JAR="$OUTPUT_JAR" "$PROJECT_DIR/lsd_patch/build.sh"
