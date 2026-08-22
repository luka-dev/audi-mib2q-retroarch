#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tag=v9.1.0
commit=fd1952d814da738ed107e05583b3e02ac11e88ff
patch="$root/vendor/qemu/patches/mib2q-qemu-v9.1.0.patch"
src="$root/vendor/qemu-upstream"
build="$src/build-mib2q"
glpass_src="$root/vendor/qemu/glpass"
glpass_build_src="$root/vendor/glpass"
render_build="$glpass_build_src/build"
output=${QNX_QEMU_OUTPUT:-$root/build/qemu-system-arm.rebuilt}

# Apple ld rejects Mach-O object archives emitted by Homebrew GNU binutils.
# Keep Homebrew libraries/pkg-config available, but force /usr/bin/ar through
# PATH for QEMU's Meson/Ninja archive rules.
PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '/binutils/' | paste -sd: -)
export PATH

[[ "$(shasum -a 256 "$patch" | awk '{print $1}')" == \
    060c3ca739c01b6f889656b89220445354a49e74c8360f775836d4b56c6e17e7 ]] || {
    echo "MIB2Q QEMU patch hash mismatch" >&2
    exit 1
}

if [[ ! -d "$src/.git" ]]; then
    git clone --depth 1 --branch "$tag" https://gitlab.com/qemu-project/qemu.git "$src"
fi
[[ "$(git -C "$src" rev-parse HEAD)" == "$commit" ]] || {
    echo "unexpected QEMU source commit" >&2
    exit 1
}

marker="$src/.mib2q-patch-sha256"
patch_hash=$(shasum -a 256 "$patch" | awk '{print $1}')
if [[ ! -f "$marker" || "$(<"$marker")" != "$patch_hash" ]]; then
    git -C "$src" apply --check "$patch"
    git -C "$src" apply "$patch"
    printf '%s\n' "$patch_hash" > "$marker"
fi

mkdir -p "$glpass_build_src"
rsync -a --delete "$glpass_src/" "$glpass_build_src/"
mkdir -p "$render_build/obj"
for unit in backend_gl decoder gl2_dispatch glpass_gpu_device_gl server_shm; do
    clang -O2 -DGL_SILENCE_DEPRECATION \
        -I"$glpass_build_src" \
        -I"$glpass_build_src/host" \
        -c "$glpass_build_src/host/$unit.c" \
        -o "$render_build/obj/$unit.o"
done
/usr/bin/ar rcs "$render_build/libglpassrender_gl.a" "$render_build"/obj/*.o
/usr/bin/ranlib "$render_build/libglpassrender_gl.a"

mkdir -p "$build"
if [[ ! -f "$build/config-host.mak" ]]; then
    python=${QEMU_PYTHON:-/opt/homebrew/bin/python3.12}
    (cd "$build" && ../configure --python="$python" \
        --target-list=arm-softmmu --disable-capstone --disable-docs \
        --disable-fuse --enable-plugins --disable-werror --disable-pie)
fi
ninja -C "$build" qemu-system-arm
mkdir -p "$(dirname "$output")"
staged="${output}.staged"
cp -X "$build/qemu-system-arm" "$staged"
chmod 0755 "$staged"
mv -f "$staged" "$output"
echo "built QEMU: $output"
echo "QEMU size: $(wc -c < "$output" | tr -d ' ')"
shasum -a 256 "$output"
