#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$root/../.." && pwd)"
output_root="$project_root/build/tests/qnx-ppsspp"
toolchain_image=${QNX_DOCKER_IMAGE:-qnx65-armv7-toolchain:latest}

. "$project_root/VENDORED_SOURCES.env"
ppsspp_git_version=$(printf '%.7s' "$PPSSPP_SOURCE_COMMIT")

mkdir -p "$output_root"

docker run --rm --platform=linux/amd64 \
   -e PPSSPP_GIT_VERSION="$ppsspp_git_version" \
   -v "$project_root":/src -w /src "$toolchain_image" \
   bash -c '
set -euo pipefail

core_dir=/src/cores-src/ppsspp
libretro_dir=$core_dir/libretro
output_dir=/src/build/tests/qnx-ppsspp
runtime_dir=/src/build/qnx-static-runtime
cxx=arm-unknown-nto-qnx6.5.0eabi-g++
ar=arm-unknown-nto-qnx6.5.0eabi-ar

mkdir -p "$output_dir" "$runtime_dir"
/src/tools/qnx-qemu/prepare-static-cxx-runtime.sh "$runtime_dir"

cd "$libretro_dir"
make -j4 platform=qnx GIT_VERSION="$PPSSPP_GIT_VERSION" \
   QNX_STATIC_CXX_LIBDIR="$runtime_dir"

cxxflags=$(make -s platform=qnx GIT_VERSION="$PPSSPP_GIT_VERSION" \
   print-CXXFLAGS | sed "s/^CXXFLAGS=//")
# UnitTest is a standalone executable, not a libretro frontend.  Keeping this
# define would remap stdio calls such as fprintf() to the libretro VFS API.
cxxflags=${cxxflags//-D__LIBRETRO__/}
cxxflags=${cxxflags//-DHAVE_LIBRETRO_VFS/}
cxxflags+=" -DPPSSPP_QNX_UNITTEST"
objects=$(make -s platform=qnx GIT_VERSION="$PPSSPP_GIT_VERSION" \
   print-OBJECTS | sed "s/^OBJECTS=//")

archive_stage="$output_dir/archive-objects"
unit_stage="$output_dir/unit-objects"
rm -rf "$archive_stage" "$unit_stage"
mkdir -p "$archive_stage" "$unit_stage"

for object in $objects; do
   unique_name=$(printf "%s" "$object" | sed "s|^\.\./||; s|/|_|g")
   cp "$object" "$archive_stage/$unique_name"
done
$ar rcs "$output_dir/libppsspp-qnx-test.a" "$archive_stage"/*.o

unit_sources=(
   unittest/UnitTest.cpp
   unittest/TestArmEmitter.cpp
   unittest/TestArm64Emitter.cpp
   unittest/TestIRPassSimplify.cpp
   unittest/TestX64Emitter.cpp
   unittest/TestVertexJit.cpp
   unittest/TestVFS.cpp
   unittest/TestRiscVEmitter.cpp
   unittest/TestLoongArch64Emitter.cpp
   unittest/TestSoftwareGPUJit.cpp
   unittest/TestThreadManager.cpp
   unittest/JitHarness.cpp
   Core/MIPS/MIPSAsm.cpp
)

unit_objects=()
for source in "${unit_sources[@]}"; do
   object="$unit_stage/$(basename "${source%.cpp}").o"
   $cxx $cxxflags -c "$core_dir/$source" -o "$object"
   unit_objects+=("$object")
done

$cxx -o "$output_dir/PPSSPPUnitTest" \
   "${unit_objects[@]}" \
   -Wl,--start-group \
   "$output_dir/libppsspp-qnx-test.a" \
   "$core_dir/ffmpeg/blackberry/armv7/lib/libavformat.a" \
   "$core_dir/ffmpeg/blackberry/armv7/lib/libavcodec.a" \
   "$core_dir/ffmpeg/blackberry/armv7/lib/libavutil.a" \
   "$core_dir/ffmpeg/blackberry/armv7/lib/libswresample.a" \
   "$core_dir/ffmpeg/blackberry/armv7/lib/libswscale.a" \
   -Wl,--end-group \
   -L"$runtime_dir" -lstdc++fs -lm -lc \
   -static-libstdc++ -static-libgcc -Wl,--gc-sections
'

file "$output_root/PPSSPPUnitTest"

tests=(
   ArmEmitter
   VertexJit
   Asin
   SinCos
   VFPUSinCos
   MathUtil
   Parsers
   IRPassSimplify
   Jit
   VFPUMatrixTranspose
   ParseLBN
   QuickTexHash
   CLZ
   MemMap
   SoftwareGPUJit
   ThreadManager
   Buffer
   SIMD
   CrossSIMD
   Path
   VFS
   IniFile
   CharQueue
   FastVec
)

if (($#)); then
   tests=("$@")
fi

failed=0
for test_name in "${tests[@]}"; do
   echo "=== PPSSPPUnitTest $test_name ==="
   if ! "$project_root/tools/qnx-qemu/run-test.sh" \
         "$output_root/PPSSPPUnitTest" "$test_name"; then
      failed=$((failed + 1))
   fi
done

if ((failed)); then
   echo "PPSSPP QNX/QEMU: $failed of ${#tests[@]} test groups failed" >&2
   exit 1
fi
echo "PPSSPP QNX/QEMU: all ${#tests[@]} test groups passed"
