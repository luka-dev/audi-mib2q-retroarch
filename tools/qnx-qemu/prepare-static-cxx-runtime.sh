#!/bin/sh
set -eu

output_dir=${1:?usage: prepare-static-cxx-runtime.sh OUTPUT_DIR}
cxx=${CXX:-arm-unknown-nto-qnx6.5.0eabi-g++}
ar=${AR:-arm-unknown-nto-qnx6.5.0eabi-ar}
ranlib=${RANLIB:-arm-unknown-nto-qnx6.5.0eabi-ranlib}
nm=${NM:-arm-unknown-nto-qnx6.5.0eabi-nm}

mkdir -p "$output_dir"
source_archive=$($cxx -print-file-name=libstdc++.a)
cp "$source_archive" "$output_dir/libstdc++.a"

# This GCC 8/QNX 6.5 build incorrectly ships compatibility wrappers for
# float/long-double libm functions already present in the target libm.  They
# recurse when libstdc++ is linked statically.  Duplicate archive members are
# possible, and the QNX ar removes only one match per invocation.
for member in math_stubs_float.o math_stubs_long_double.o; do
   while "$ar" t "$output_dir/libstdc++.a" | grep -qx "$member"; do
      "$ar" d "$output_dir/libstdc++.a" "$member"
   done
done
"$ranlib" "$output_dir/libstdc++.a"

if "$nm" -A --defined-only "$output_dir/libstdc++.a" 2>/dev/null |
      grep -Eq " [TW] (ceilf|expf|floorf|powf|sqrtf)$"; then
   echo "sanitized libstdc++.a still defines QNX libm functions" >&2
   exit 1
fi

echo "$output_dir/libstdc++.a"
