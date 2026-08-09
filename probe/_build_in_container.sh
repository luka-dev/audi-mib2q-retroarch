#!/bin/bash
# Runs INSIDE the qnx65-armv7-toolchain container, with Tools/ mounted at /src.
# Driven by build_audio_provider.sh — do not run directly.
set -eu
CC=arm-unknown-nto-qnx6.5.0eabi-g++
OBJ=arm-unknown-nto-qnx6.5.0eabi-readelf
P=/src/retroarch-qnx/probe

# The firmware libs have no .dynamic section, so ld refuses them. Generate stub
# .so's exporting the same mangled symbols under the same SONAMEs, link against
# those, and let the runtime loader bind to the real firmware libs.
rm -rf "$P/.stubs"; mkdir -p "$P/.stubs"; cd "$P/.stubs"

mkstub() {                       # $1=soname $2=outbase  $3..=symbols
  local soname=$1 out=$2 s; shift 2
  { echo '.text'
    for s in "$@"; do case $s in
      _ZTV*) : ;;
      *) printf '.globl %s\n.type %s,%%function\n%s:\n  bx lr\n' "$s" "$s" "$s" ;;
    esac; done
    echo '.data'
    for s in "$@"; do case $s in
      _ZTV*) printf '.globl %s\n.type %s,%%object\n.size %s,64\n%s:\n  .space 64\n' \
                    "$s" "$s" "$s" "$s" ;;
    esac; done
  } > "stub_$out.s"
  $CC -shared -fPIC -Wl,-soname,"$soname" -o "lib${out}stub.so" "stub_$out.s"
}

mkstub libcomm.so comm \
  _ZN4comm13LifecycleImplC1ENS_9Lifecycle5StateEPNS_17LifecycleListenerE \
  _ZN4comm19ServiceRegistration15registerServiceEv \
  _ZNK4comm19ServiceRegistration17unregisterServiceEv \
  _ZNK4comm19ServiceRegistration7isAliveEv \
  _ZN4comm20TrackedReferenceBase10releaseRefEv \
  _ZN4comm20TrackedReferenceBaseaSERKS0_ \
  _ZN4comm26DefaultActiveObjectFactoryC1Ev \
  _ZN4comm8StubBase12clientChangeEbj \
  _ZN4comm7CoreApi11getInstanceEv \
  _ZTVN4comm13LifecycleImplE _ZTVN4comm19ServiceRegistrationE \
  _ZTVN4comm20TrackedReferenceBaseE _ZTVN4comm26DefaultActiveObjectFactoryE \
  _ZTVN4comm8StubBaseE

mkstub libiplcommon.so iplcommon \
  _ZN3ipl4UUIDC1Ejjjthhhhhh _ZN3ipl4UUIDC1ERKS0_ _ZN3ipl4UUIDD1Ev \
  _ZN3ipl12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcRKS4_

mkstub libutil.so util \
  _ZN4util20SharedPtrDefaultBaseC1ERKS0_ \
  _ZN4util20SharedPtrDefaultBaseC2EPvPFvS1_S1_ES1_ \
  _ZN4util4Util4initEPKcbbbb

mkstub libdsicommon.so dsicommon \
  _ZN3dsi19ServiceProviderBaseC1EPKcj \
  _ZN3dsi19ServiceProviderBase15addNotificationEiRN4comm5ProxyE \
  _ZNK3dsi19ServiceProviderBase10isNotifiedEi

mkstub libosal.so osal _ZN4osal4OsalC1Ebb

echo "=== stubs: $(ls lib*stub.so | wc -l) ==="

echo '=== link libdsiaudioprovider.so ==='
cd "$P"
# Compile as C++ but LINK with the C driver: the target image ships no
# libstdc++.so.6, and a NEEDED on it makes the runtime loader silently skip the
# whole preload ("ldd:FATAL: Could not load library libstdc++.so.6" in the boot
# log -- which is exactly how the kp/ctul grafts fail there too). We use no C++
# runtime: no new/delete (calloc), no exceptions, no RTTI, and the only vtables
# involved are hand-built arrays.
$CC -c -fPIC -O2 -Wall -fno-exceptions -fno-rtti -include stddef.h \
    -I/src/qnx-carplay-emu/host \
    -o dsi_audio_provider.o dsi_audio_provider.cpp
arm-unknown-nto-qnx6.5.0eabi-gcc -shared -fPIC \
    -o libdsiaudioprovider.so dsi_audio_provider.o \
    -L.stubs -l:libcommstub.so -l:libiplcommonstub.so -l:libutilstub.so \
             -l:libdsicommonstub.so -l:libosalstub.so
rm -f dsi_audio_provider.o

ls -l libdsiaudioprovider.so
echo '--- runtime NEEDED (must be the REAL firmware lib names) ---'
$OBJ -d libdsiaudioprovider.so | grep -E 'NEEDED|SONAME'
echo '--- framework syms bound at runtime ---'
$OBJ -D -s libdsiaudioprovider.so | awk '$7=="UND" && $8 ~ /^_Z/ {print "   " $8}' | sort -u
rm -rf "$P/.stubs"
echo '=== BUILD OK ==='
