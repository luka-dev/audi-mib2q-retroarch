#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MESA_COMMIT="479773c7e4264506f2d9ec4bf15c6bf677f0d67a"
BUILDER_IMAGE="${QFD_MESA_BUILDER_IMAGE:-qnx65-mesa-builder:1.11}"
MESA_SRC="${QFD_MESA_SRC:-$ROOT/build/mesa-freedreno-src}"
MESA_BUILD="${QFD_MESA_BUILD:-$ROOT/build/mesa-qnx-a3xx}"
ARTIFACTS="${QFD_MESA_ARTIFACTS:-$ROOT/build/qnx-freedreno/mesa-static}"
JOBS="${QFD_JOBS:-4}"

die() {
   echo "error: $*" >&2
   exit 1
}

container_path() {
   local path="$1"
   case "$path" in
      "$ROOT"/*) printf '/src/%s' "${path#"$ROOT"/}" ;;
      *) die "path must stay inside workspace: $path" ;;
   esac
}

install_overlay() {
   local source="$1"
   local target="$2"
   mkdir -p "$(dirname "$target")"
   if [[ -e "$target" ]]; then
      cmp -s "$source" "$target" ||
         die "refusing to overwrite a divergent Mesa overlay file: $target"
      return
   fi
   cp "$source" "$target"
}

command -v docker >/dev/null || die "docker is required"
docker image inspect "$BUILDER_IMAGE" >/dev/null 2>&1 ||
   die "missing builder image $BUILDER_IMAGE (build it from mesa-builder.Dockerfile)"

if [[ ! -e "$MESA_SRC/.git" ]]; then
   mkdir -p "$(dirname "$MESA_SRC")"
   git clone --filter=blob:none https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_SRC"
   git -C "$MESA_SRC" checkout --detach "$MESA_COMMIT"
fi

actual_commit="$(git -C "$MESA_SRC" rev-parse HEAD)"
[[ "$actual_commit" == "$MESA_COMMIT" ]] ||
   die "Mesa commit mismatch: expected $MESA_COMMIT, got $actual_commit"

if git -C "$MESA_SRC" apply --reverse --check "$HERE/mesa-qnx.patch" >/dev/null 2>&1; then
   echo ">> Mesa QNX patch already applied"
elif git -C "$MESA_SRC" apply --check "$HERE/mesa-qnx.patch" >/dev/null 2>&1; then
   git -C "$MESA_SRC" apply "$HERE/mesa-qnx.patch"
   echo ">> applied Mesa QNX patch"
else
   die "Mesa tree is neither pristine nor exactly patched; inspect $MESA_SRC"
fi

for file in qnx_bo.c qnx_device.c qnx_pipe.c qnx_priv.h qnx_ringbuffer_sp.c; do
   install_overlay "$HERE/mesa-overlay/qnx/$file" \
      "$MESA_SRC/src/freedreno/drm/qnx/$file"
done
install_overlay "$HERE/qfd_winsys.c" \
   "$MESA_SRC/src/freedreno/drm/qnx/qfd_winsys.c"
install_overlay "$HERE/qfd_winsys.h" \
   "$MESA_SRC/src/freedreno/drm/qnx/qfd_winsys.h"
install_overlay "$ROOT/tools/qnx-gsl-port/qnx_gsl_abi.h" \
   "$MESA_SRC/src/freedreno/drm/qnx/qnx_gsl_abi.h"
for file in qnx_compat.c qnx_compat.h; do
   install_overlay "$HERE/mesa-overlay/util/$file" "$MESA_SRC/src/util/$file"
done
install_overlay "$HERE/mesa-overlay/gallium/qnx_screen_probe.c" \
   "$MESA_SRC/src/gallium/drivers/freedreno/qnx_screen_probe.c"

mesa_src_container="$(container_path "$MESA_SRC")"
mesa_build_container="$(container_path "$MESA_BUILD")"
cross_file_container="$(container_path "$HERE/mesa-qnx-armv7.ini")"

meson_options=(
   -Dgallium-drivers=freedreno
   -Dvulkan-drivers=[]
   -Dplatforms=[]
   -Degl=disabled
   -Dglx=disabled
   -Dgbm=disabled
   -Dgles1=disabled
   -Dgles2=enabled
   -Dopengl=true
   -Dllvm=disabled
   -Dshared-glapi=disabled
   -Dshader-cache=disabled
   -Dbuild-tests=false
   -Dtools=[]
   -Dvideo-codecs=[]
   -Dlibunwind=disabled
   -Dvalgrind=disabled
   -Dzstd=disabled
)

if [[ -f "$MESA_BUILD/build.ninja" ]]; then
   docker run --rm --platform=linux/amd64 \
      -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" \
      meson configure "$mesa_build_container" "${meson_options[@]}"
else
   [[ ! -e "$MESA_BUILD" || -d "$MESA_BUILD" ]] ||
      die "build path exists and is not a directory: $MESA_BUILD"
   docker run --rm --platform=linux/amd64 \
      -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" \
      meson setup "$mesa_build_container" "$mesa_src_container" \
      --cross-file="$cross_file_container" "${meson_options[@]}"
fi

targets=(
   src/freedreno/drm/libfreedreno_drm.a
   src/freedreno/ir3/libfreedreno_ir3.a
   src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a
   src/gallium/drivers/freedreno/libfreedreno.a
   src/gallium/drivers/freedreno/qnx-freedreno-screen-probe
)

docker run --rm --platform=linux/amd64 \
   -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" \
   ninja -C "$mesa_build_container" -j"$JOBS" "${targets[@]}"

mkdir -p "$ARTIFACTS"
cp "$MESA_BUILD/src/freedreno/drm/libfreedreno_drm.a" \
   "$ARTIFACTS/libfreedreno_qnx_drm.a"
cp "$MESA_BUILD/src/freedreno/ir3/libfreedreno_ir3.a" \
   "$ARTIFACTS/libfreedreno_ir3.a"
cp "$MESA_BUILD/src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a" \
   "$ARTIFACTS/libfreedreno_qnx_winsys.a"
cp "$MESA_BUILD/src/gallium/drivers/freedreno/libfreedreno.a" \
   "$ARTIFACTS/libfreedreno_a3xx_gallium.a"
cp "$MESA_BUILD/src/gallium/drivers/freedreno/qnx-freedreno-screen-probe" \
   "$ARTIFACTS/qnx-freedreno-screen-probe.debug"
cp "$ARTIFACTS/qnx-freedreno-screen-probe.debug" \
   "$ARTIFACTS/qnx-freedreno-screen-probe"

docker run --rm --platform=linux/amd64 \
   -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" \
   /opt/qnx650/host/linux/x86/usr/bin/ntoarmv7-strip \
   "$(container_path "$ARTIFACTS/qnx-freedreno-screen-probe")"

shasum -a 256 "$ARTIFACTS"/*.a \
   "$ARTIFACTS/qnx-freedreno-screen-probe" \
   "$ARTIFACTS/qnx-freedreno-screen-probe.debug" \
   > "$ARTIFACTS/SHA256SUMS"
printf '%s\n' "$MESA_COMMIT" > "$ARTIFACTS/MESA_COMMIT"

nm_output="$(docker run --rm --platform=linux/amd64 \
   -v "$ROOT:/src" -w /src "$BUILDER_IMAGE" \
   /opt/qnx650/host/linux/x86/usr/bin/ntoarmv7-nm \
   "$(container_path "$ARTIFACTS/libfreedreno_qnx_drm.a")")"
grep -q ' T qnx_device_new$' <<<"$nm_output" || die "qnx_device_new missing"
grep -q ' T qnx_pipe_new$' <<<"$nm_output" || die "qnx_pipe_new missing"
grep -q ' T qfd_pipe_submit$' <<<"$nm_output" || die "qfd_pipe_submit missing"

echo ">> QNX/ARM A3xx static driver + screen probe build PASS"
file "$ARTIFACTS"/*.a "$ARTIFACTS"/qnx-freedreno-screen-probe
cat "$ARTIFACTS/SHA256SUMS"
