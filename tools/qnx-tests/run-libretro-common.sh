#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$root/../.." && pwd)"
output_root="$project_root/build/tests/qnx-libretro-common"
toolchain_image=${QNX_DOCKER_IMAGE:-qnx65-armv7-toolchain:latest}

mkdir -p "$output_root"

build_test() {
   local name=$1
   shift
   docker run --rm --platform=linux/amd64 \
      -v "$project_root":/src -w /src "$toolchain_image" \
      qcc -Vgcc_ntoarmv7le -O2 -g -Wall -Wextra \
      -Wno-unused-parameter -Wno-unused-variable \
      -D__QNX__ -include stddef.h \
      -I/src/src/libretro-common/include \
      -I/src/tools/qnx-tests/minicheck \
      /src/tools/qnx-tests/minicheck/minicheck.c \
      "$@" -o "/src/build/tests/qnx-libretro-common/$name"
}

build_test test_stdstring \
   /src/src/libretro-common/test/string/test_stdstring.c \
   /src/src/libretro-common/string/stdstring.c \
   /src/src/libretro-common/encodings/encoding_utf.c \
   /src/src/libretro-common/compat/compat_strl.c \
   /src/src/libretro-common/compat/compat_strldup.c

build_test test_utils \
   /src/src/libretro-common/test/utils/test_utils.c \
   /src/src/libretro-common/utils/md5.c \
   /src/src/libretro-common/encodings/encoding_crc32.c \
   /src/src/libretro-common/streams/file_stream.c \
   /src/src/libretro-common/vfs/vfs_implementation.c \
   /src/src/libretro-common/file/file_path.c \
   /src/src/libretro-common/file/file_path_io.c \
   /src/src/libretro-common/compat/compat_strl.c \
   /src/src/libretro-common/time/rtime.c \
   /src/src/libretro-common/string/stdstring.c \
   /src/src/libretro-common/encodings/encoding_utf.c

build_test test_hash \
   /src/src/libretro-common/test/hash/test_hash.c \
   /src/src/libretro-common/hash/lrc_hash.c \
   /src/src/libretro-common/streams/file_stream.c \
   /src/src/libretro-common/vfs/vfs_implementation.c \
   /src/src/libretro-common/file/file_path.c \
   /src/src/libretro-common/file/file_path_io.c \
   /src/src/libretro-common/compat/compat_strl.c \
   /src/src/libretro-common/time/rtime.c \
   /src/src/libretro-common/string/stdstring.c \
   /src/src/libretro-common/encodings/encoding_utf.c

build_test test_linked_list \
   /src/src/libretro-common/test/lists/test_linked_list.c \
   /src/src/libretro-common/lists/linked_list.c

build_test test_generic_queue \
   /src/src/libretro-common/test/queues/test_generic_queue.c \
   /src/src/libretro-common/queues/generic_queue.c

qnx_libz=/opt/qnx650/target/qnx6/armle-v7/usr/lib/libz.a
build_test test_rpng \
   /src/src/libretro-common/test/formats/test_rpng.c \
   /src/src/libretro-common/formats/png/rpng.c \
   /src/src/libretro-common/streams/trans_stream.c \
   /src/src/libretro-common/streams/trans_stream_zlib.c \
   /src/src/libretro-common/streams/trans_stream_deflate.c \
   /src/src/libretro-common/encodings/encoding_deflate.c \
   /src/src/libretro-common/streams/trans_stream_pipe.c \
   "$qnx_libz"

for test_binary in "$output_root"/test_*; do
   echo "=== $(basename "$test_binary") ==="
   "$project_root/tools/qnx-qemu/run-test.sh" "$test_binary"
done
