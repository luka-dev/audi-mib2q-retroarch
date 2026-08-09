#!/bin/sh
# On-device RetroArch launcher for MHI2Q. Lives at /mnt/app/root/retroarch/ra.sh.
# Invoked by the HMI "Games" state. `exec`s the binary so the running process IS
# RetroArch (it writes its own PID into the lockfile for the HMI to signal).

RA_DIR=${RA_APP_DIR:-/mnt/app/root/retroarch}
RA_SD_MOUNT=${RA_SD_MOUNT:-/fs/sda0}
RA_VOLATILE_DIR=${RA_VOLATILE_DIR:-/tmp/retroarch}
RA_FACTORY_CONFIG="$RA_DIR/retroarch.cfg"
RA_MEDIA="$RA_SD_MOUNT/retroarch"
RA_HAVE_SD=0

# The app image is the immutable factory layer.  Every writable path belongs to
# the removable card.  A card may initially be mounted read-only, so remount it
# and verify that its RetroArch directory can actually be created before using
# it as the live user layer.
if [ -d "$RA_SD_MOUNT" ]; then
    mount -uw "$RA_SD_MOUNT" 2>/dev/null
    if mkdir -p "$RA_MEDIA/config" 2>/dev/null; then
        _ra_write_probe="$RA_MEDIA/config/.ra-write-test.$$"
        if (umask 077 && : > "$_ra_write_probe") 2>/dev/null; then
            rm -f "$_ra_write_probe"
            RA_HAVE_SD=1
        fi
    fi
fi

if [ "$RA_HAVE_SD" -eq 0 ]; then
    # Safe no-card fallback: keep the complete UI/cores available from appimg,
    # but redirect the active config and all platform defaults to volatile RAM.
    RA_MEDIA="$RA_VOLATILE_DIR"
fi

mkdir -p "$RA_MEDIA/roms" "$RA_MEDIA/ps1" "$RA_MEDIA/gba" \
         "$RA_MEDIA/saves" "$RA_MEDIA/states" \
         "$RA_MEDIA/system" "$RA_MEDIA/cheats" "$RA_MEDIA/playlists" \
         "$RA_MEDIA/config/remaps" "$RA_MEDIA/logs" "$RA_MEDIA/screenshots" \
         "$RA_MEDIA/info" "$RA_MEDIA/database/rdb" \
         "$RA_MEDIA/downloads" "$RA_MEDIA/filters/audio" \
         "$RA_MEDIA/filters/video" "$RA_MEDIA/wallpapers" \
         "$RA_MEDIA/overlays/keyboards" 2>/dev/null

RA_USER_CONFIG="$RA_MEDIA/config/retroarch.cfg"

# A fresh/replacement SD starts from the known-good factory configuration.
# RetroArch then uses this writable copy as its primary config, so explicit
# Save Configuration and save-on-exit can never target read-only /mnt/app.
if [ ! -s "$RA_USER_CONFIG" ]; then
    _ra_config_tmp="$RA_USER_CONFIG.tmp.$$"
    rm -f "$_ra_config_tmp"
    if [ "$RA_HAVE_SD" -eq 1 ]; then
        cp "$RA_FACTORY_CONFIG" "$_ra_config_tmp" 2>/dev/null
    else
        # Factory paths intentionally name the production SD mount. Rebase the
        # volatile copy so *every* runtime write stays in tmp when no card is
        # present, including saves/states/logs requested by explicit settings.
        sed "s#/fs/sda0/retroarch#$RA_MEDIA#g" \
            "$RA_FACTORY_CONFIG" > "$_ra_config_tmp" 2>/dev/null
    fi
    if [ ! -s "$_ra_config_tmp" ] || ! mv "$_ra_config_tmp" "$RA_USER_CONFIG"; then
        rm -f "$_ra_config_tmp"
        echo "RetroArch: cannot create writable runtime config" >&2
        exit 1
    fi
    sync
fi

# Core-info is small but its cache is writable. Seed it only when the card has
# no .info files; large databases/cheats remain part of the SD installation
# image and are intentionally not duplicated inside appimg.
_ra_info_found=0
for _ra_info in "$RA_MEDIA/info/"*.info; do
    if [ -f "$_ra_info" ]; then
        _ra_info_found=1
        break
    fi
done
if [ "$_ra_info_found" -eq 0 ]; then
    cp "$RA_DIR/info/"*.info "$RA_MEDIA/info/" 2>/dev/null
fi

# GL-stack ORDER matches gpSP's working launcher: the Adreno libEGL/libGLESv2
# from the appimg/eso (/mnt/app/eso/lib, /armle/lib) must win over /proc/boot's
# (the /proc/boot ones fault egl14.so in eglGetDisplay). Our lib dir first
# (libstdc++.so.6 4.9.4 + libhiddi.so.1); /proc/boot only as a libc/libm fallback.
LD_LIBRARY_PATH="$RA_DIR/lib:/mnt/app/eso/lib:/mnt/app/armle/lib:/mnt/app/armle/usr/lib:/eso/lib:/eso/lib/factories:/armle/lib:/lib:/usr/lib:/proc/boot:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

# lifecycle/display/audio knobs (platform_qnx.c / qnx_ctx.c / qnx_qsa.c)
# Static application data and HID profiles are always available without a card;
# mutable state is isolated to the current card (or /tmp in no-card mode).
export RA_DATA_DIR="$RA_DIR"
export RA_USER_DIR="$RA_MEDIA"
export RA_CONFIG_PATH="$RA_USER_CONFIG"
export RA_CONTENT_RULES="$RA_DIR/content-rules.cfg"
export LIBRETRO_AUTOCONFIG_DIRECTORY="$RA_DIR/autoconfig"
export RA_LOCK_PATH="/tmp/retroarch.lock"  # tmpfs root, NO subfolder; MUST match the HMI hook's LOCK constant
export RA_QNX_DISPLAYABLE_ID=43            # DIGITAL_VIDEOPLAYER_1 (chrome-free layer)
export RA_QNX_CONTEXT_ID=90                 # private chrome-free context {43}
export RA_QNX_DISPLAY_ID=0
export RA_QNX_SCREEN_W=1024                 # fixed Ozone render surface
export RA_QNX_SCREEN_H=480
export RA_QNX_AUDIO_DEV=/dev/snd/mpl1_int_ent
# ★ THE EGL FIX ★ libOSUser opens "$GRAPHICS_ROOT/graphics.conf" to learn the
# eglsub-dlls list (libscreen.so.1 eglsub-screen.so). UNSET -> egl14.so's
# eglInitialize assumes the array handle format for a raw dlopen handle and
# SIGSEGVs at egl14.so+0x16428 (OpenSubDriver). Set it BEFORE EGL init.
export GRAPHICS_ROOT=/proc/boot/
[ -n "$IPL_CONFIG_DIR" ] || export IPL_CONFIG_DIR=/etc/eso/production
unset EGL_PLATFORM

# Host/package validation hook. It exercises media detection and factory
# seeding but deliberately stops before touching the QNX HID/display stack.
if [ "${RA_LAUNCH_VALIDATE_ONLY:-0}" -eq 1 ]; then
    echo "RA_HAVE_SD=$RA_HAVE_SD"
    echo "RA_DATA_DIR=$RA_DATA_DIR"
    echo "RA_USER_DIR=$RA_USER_DIR"
    echo "RA_CONFIG_PATH=$RA_CONFIG_PATH"
    exit 0
fi

# Gamepad HID service: io-hid isn't running on this unit, so
# hidd_connect fails. io-usb IS up (/dev/io-usb/io-usb), so start io-hid on it.
# It's a resource manager -> stays resident; creates /dev/io-hid/io-hid.
if [ ! -e /dev/io-hid/io-hid ]; then
    /armle/sbin/io-hid -d usb upath=/dev/io-usb/io-usb &
    _i=0
    while [ ! -e /dev/io-hid/io-hid ] && [ $_i -lt 25 ]; do sleep 0.2; _i=$((_i+1)); done
fi

# Bounded HID diagnostics: topology plus only the first eight packets from
# each report. They are captured by RetroArch's file logger on the SD card.
export RA_QNX_HID_DEBUG=1
export RA_QNX_HID_DUMP=1

cd "$RA_DIR" 2>/dev/null

# The active config is always writable SD state (or a volatile /tmp copy when
# no card is present). The factory file is only a seed and is never modified.
exec "$RA_DIR/retroarch" --config "$RA_USER_CONFIG" "$@"
