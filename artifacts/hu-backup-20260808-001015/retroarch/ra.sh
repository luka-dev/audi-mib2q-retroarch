#!/bin/sh
# On-device RetroArch launcher for MHI2Q. Lives at /mnt/app/root/retroarch/ra.sh.
# Invoked by the HMI "Games" menu hook. `exec`s the binary so the running process
# IS retroarch (it writes its own PID into the lockfile for the HMI to signal).

RA_DIR=/mnt/app/root/retroarch     # ro appimg: binary + cores + libs + base cfg
RA_MEDIA=/fs/sda0/retroarch        # SD card (M.I.B. toolbox, FAT32): ROMs + saves + states + BIOS + config

# make the SD writable (mounted ro by default) + create our media tree there
mount -uw /fs/sda0 2>/dev/null
mkdir -p "$RA_MEDIA/roms" "$RA_MEDIA/saves" "$RA_MEDIA/states" \
         "$RA_MEDIA/system" "$RA_MEDIA/cheats" "$RA_MEDIA/playlists" \
         "$RA_MEDIA/config" "$RA_MEDIA/logs" 2>/dev/null

# GL-stack ORDER matches gpSP's working launcher: the Adreno libEGL/libGLESv2
# from the appimg/eso (/mnt/app/eso/lib, /armle/lib) must win over /proc/boot's
# (the /proc/boot ones fault egl14.so in eglGetDisplay). Our lib dir first
# (libstdc++.so.6 4.9.4 + libhiddi.so.1); /proc/boot only as a libc/libm fallback.
LD_LIBRARY_PATH="$RA_DIR/lib:/mnt/app/eso/lib:/mnt/app/armle/lib:/mnt/app/armle/usr/lib:/eso/lib:/eso/lib/factories:/armle/lib:/lib:/usr/lib:/proc/boot:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

# lifecycle/display/audio knobs (platform_qnx.c / qnx_ctx.c / qnx_qsa.c)
export RA_DATA_DIR="$RA_DIR"
export RA_USER_DIR="$RA_MEDIA"
export RA_LOCK_PATH="/tmp/retroarch.lock"  # tmpfs root, NO subfolder; MUST match the HMI hook's LOCK constant
export RA_QNX_DISPLAYABLE_ID=43            # DIGITAL_VIDEOPLAYER_1 (chrome-free layer)
export RA_QNX_AUDIO_DEV=/dev/snd/mpl1_int_ent
# ★ THE EGL FIX ★ libOSUser opens "$GRAPHICS_ROOT/graphics.conf" to learn the
# eglsub-dlls list (libscreen.so.1 eglsub-screen.so). UNSET -> egl14.so's
# eglInitialize assumes the array handle format for a raw dlopen handle and
# SIGSEGVs at egl14.so+0x16428 (OpenSubDriver). Set it BEFORE EGL init.
export GRAPHICS_ROOT=/proc/boot/
[ -n "$IPL_CONFIG_DIR" ] || export IPL_CONFIG_DIR=/etc/eso/production
unset EGL_PLATFORM

# Gamepad HID (gpSP's crutch): io-hid isn't running on this unit, so
# hidd_connect fails. io-usb IS up (/dev/io-usb/io-usb), so start io-hid on it.
# It's a resource manager -> stays resident; creates /dev/io-hid/io-hid.
if [ ! -e /dev/io-hid/io-hid ]; then
    /armle/sbin/io-hid -d usb upath=/dev/io-usb/io-usb &
    _i=0
    while [ ! -e /dev/io-hid/io-hid ] && [ $_i -lt 25 ]; do sleep 0.2; _i=$((_i+1)); done
fi

cd "$RA_DIR" 2>/dev/null

# --config = base (ro) cfg; --appendconfig = writable overrides on the SD.
exec "$RA_DIR/retroarch" \
    --config "$RA_DIR/retroarch.cfg" \
    --appendconfig "$RA_MEDIA/config/retroarch.cfg" \
    "$@"
