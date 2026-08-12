#!/bin/sh
# On-device RetroArch launcher for MHI2Q. Lives at /mnt/app/root/retroarch/ra.sh.
# Invoked by the HMI "Games" state. `exec`s the binary so the running process IS
# RetroArch (it writes its own PID into the lockfile for the HMI to signal).

RA_DIR=${RA_APP_DIR:-/mnt/app/root/retroarch}
RA_SD_MOUNT=${RA_SD_MOUNT:-/fs/sda0}
RA_VOLATILE_DIR=${RA_VOLATILE_DIR:-/tmp/retroarch}
RA_FACTORY_CONFIG="$RA_DIR/retroarch.cfg"
RA_FACTORY_CORE_OPTIONS="$RA_DIR/retroarch-core-options.cfg"
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
         "$RA_MEDIA/n64" \
         "$RA_MEDIA/saves" "$RA_MEDIA/states" \
         "$RA_MEDIA/system" "$RA_MEDIA/cheats" "$RA_MEDIA/playlists" \
         "$RA_MEDIA/config/remaps" "$RA_MEDIA/logs" "$RA_MEDIA/screenshots" \
         "$RA_MEDIA/info" "$RA_MEDIA/database/rdb" \
         "$RA_MEDIA/autoconfig" \
         "$RA_MEDIA/downloads" "$RA_MEDIA/filters/audio" \
         "$RA_MEDIA/filters/video" "$RA_MEDIA/wallpapers" \
         "$RA_MEDIA/overlays/keyboards" 2>/dev/null

RA_USER_CONFIG="$RA_MEDIA/config/retroarch.cfg"
RA_USER_CORE_OPTIONS="$RA_MEDIA/config/retroarch-core-options.cfg"

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

# Seed only a missing options file. Once created, core option changes remain
# with this SD card and are never overwritten by an app update.
if [ ! -s "$RA_USER_CORE_OPTIONS" ]; then
    _ra_options_tmp="$RA_USER_CORE_OPTIONS.tmp.$$"
    rm -f "$_ra_options_tmp"
    cp "$RA_FACTORY_CORE_OPTIONS" "$_ra_options_tmp" 2>/dev/null
    if [ ! -s "$_ra_options_tmp" ] || ! mv "$_ra_options_tmp" "$RA_USER_CORE_OPTIONS"; then
        rm -f "$_ra_options_tmp"
        echo "RetroArch: cannot create writable core options" >&2
        exit 1
    fi
fi

# Factory controller profiles are immutable app data, while profiles created
# by "Update Controller Profile" belong to the current SD card. RetroArch has
# one autoconfig search/save directory, so seed a versioned factory `qnx/`
# subtree into the writable layer and leave user overrides at its root.
RA_AUTOCONFIG_DIR="$RA_MEDIA/autoconfig"
RA_AUTOCONFIG_VERSION=3
_ra_autoconfig_stamp="$RA_AUTOCONFIG_DIR/.qnx-factory-version"
_ra_autoconfig_current=
if [ -f "$_ra_autoconfig_stamp" ]; then
    _ra_autoconfig_current=`cat "$_ra_autoconfig_stamp" 2>/dev/null`
fi
if [ "$_ra_autoconfig_current" != "$RA_AUTOCONFIG_VERSION" ]; then
    _ra_autoconfig_tmp="$RA_AUTOCONFIG_DIR/.qnx-factory.$$"
    rm -rf "$_ra_autoconfig_tmp"
    mkdir -p "$_ra_autoconfig_tmp"
    cp "$RA_DIR/autoconfig/qnx/"*.cfg "$_ra_autoconfig_tmp/" 2>/dev/null
    cp "$RA_DIR/autoconfig/qnx/COPYING" \
       "$RA_DIR/autoconfig/qnx/SOURCE.txt" \
       "$_ra_autoconfig_tmp/" 2>/dev/null
    if [ ! -s "$_ra_autoconfig_tmp/8BitDo_Ultimate_2C_Wireless_Controller_2.4G.cfg" ]; then
        rm -rf "$_ra_autoconfig_tmp"
        echo "RetroArch: cannot seed controller profiles" >&2
        exit 1
    fi
    rm -rf "$RA_AUTOCONFIG_DIR/qnx"
    mv "$_ra_autoconfig_tmp" "$RA_AUTOCONFIG_DIR/qnx"
    _ra_autoconfig_stamp_tmp="$_ra_autoconfig_stamp.tmp.$$"
    echo "$RA_AUTOCONFIG_VERSION" > "$_ra_autoconfig_stamp_tmp"
    mv "$_ra_autoconfig_stamp_tmp" "$_ra_autoconfig_stamp"
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
export LIBRETRO_AUTOCONFIG_DIRECTORY="$RA_AUTOCONFIG_DIR"
export RA_LOCK_PATH="/tmp/retroarch.lock"  # tmpfs root, NO subfolder; MUST match the HMI hook's LOCK constant
export RA_QNX_DISPLAYABLE_ID=43            # DIGITAL_VIDEOPLAYER_1 (chrome-free layer)
export RA_QNX_CONTEXT_ID=90                 # private chrome-free context {43}
export RA_QNX_DISPLAY_ID=0
export RA_QNX_SCREEN_W=1024                 # fixed Ozone render surface
export RA_QNX_SCREEN_H=480
export RA_QNX_AUDIO_DEV=/dev/snd/mpl1_int_ent
export RA_QNX_AUDIO_READY_PATH=/tmp/retroarch.pcm.ready
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
    echo "LIBRETRO_AUTOCONFIG_DIRECTORY=$LIBRETRO_AUTOCONFIG_DIRECTORY"
    exit 0
fi

# Rebind stdout/stderr only after the SD/no-SD decision and directory creation.
# This makes first boot on a blank card persistent too: the Java wrapper has to
# choose its bootstrap redirection before ra.sh has mounted/initialized the SD.
if [ "$RA_HAVE_SD" -eq 1 ]; then
    RA_RUN_LOG="$RA_MEDIA/logs/ra_run.log"
else
    RA_RUN_LOG=/tmp/ra_run.log
fi

# Keep diagnostics useful but strictly bounded. QNX 6.5 on this image has no
# `wc`, `find` or `tail`, so use the stable size field from `ls -l` and perform
# positional/glob work in subshells (preserves the ROM arguments in "$@").
_ra_rotate_log() {
    (
        _ra_rotate_path=$1
        _ra_rotate_limit=$2
        [ -f "$_ra_rotate_path" ] || exit 0
        set -- `ls -l "$_ra_rotate_path" 2>/dev/null`
        _ra_rotate_size=$5
        case "$_ra_rotate_size" in
            ''|*[!0-9]*) exit 0 ;;
        esac
        if [ "$_ra_rotate_size" -ge "$_ra_rotate_limit" ]; then
            rm -f "$_ra_rotate_path.1"
            mv "$_ra_rotate_path" "$_ra_rotate_path.1" 2>/dev/null ||
                : > "$_ra_rotate_path"
        fi
    )
}

_ra_rotate_log "$RA_RUN_LOG" 524288
_ra_rotate_log "$RA_MEDIA/logs/ra_hook.log" 262144
_ra_rotate_log "$RA_MEDIA/logs/ra_audio.log" 262144
_ra_rotate_log /tmp/ra_run.log 524288
_ra_rotate_log /tmp/ra_hook.log 262144
_ra_rotate_log /tmp/ra_audio.log 262144
_ra_rotate_log /tmp/ra_bootstrap.log 262144

# RetroArch creates a timestamped frontend log for each launch. Leave room for
# the file about to be created so the steady-state maximum is eight sessions.
(
    set -- "$RA_MEDIA/logs"/retroarch__*.log
    [ -f "$1" ] || exit 0
    while [ "$#" -gt 7 ]; do
        rm -f "$1"
        shift
    done
)

exec >> "$RA_RUN_LOG" 2>&1
echo "--- RetroArch session `date` ---"

# Bounded HID diagnostics: topology plus only the first eight packets from
# each report. They are captured by RetroArch's file logger on the SD card.
export RA_QNX_HID_DEBUG=1
export RA_QNX_HID_DUMP=1

cd "$RA_DIR" 2>/dev/null

# The active config is always writable SD state (or a volatile /tmp copy when
# no card is present). The factory file is only a seed and is never modified.
#
# Keep this shell as the explicit native-process supervisor. A QNX process
# whose final thread has exited remains in /proc as a zombie until its parent
# calls waitpid(); using `exec` here left the asynchronous Java wrapper as that
# parent, and the stock /bin/sh did not reliably reap signal-driven exits.
#
# io-hid is also owned by this one RetroArch session. A previous persistent
# instance deadlocked inside its USB/mutex graph; even `test -e` on its resource
# manager path then blocked forever. Never stat /dev/io-hid here. Retire only
# our exact io-usb-backed instance, start a fresh child, and reap it on exit.
_ra_hid_pid=
_ra_native_pid=

_ra_signal_owned_hid() {
    _ra_hid_signal="$1"
    pidin ar 2>/dev/null |
        grep '/armle/sbin/io-[h]id -d usb upath=/dev/io-usb/io-usb' |
        while read _ra_hid_old_pid _ra_hid_old_command; do
            case "$_ra_hid_old_pid" in
                ''|*[!0-9]*) continue ;;
            esac
            kill -"$_ra_hid_signal" "$_ra_hid_old_pid" 2>/dev/null
        done
}

_ra_stop_session_hid() {
    if [ -n "$_ra_hid_pid" ]; then
        kill -TERM "$_ra_hid_pid" 2>/dev/null
        sleep 0.2
        kill -KILL "$_ra_hid_pid" 2>/dev/null
        wait "$_ra_hid_pid" 2>/dev/null
        _ra_hid_pid=
    fi
}

_ra_forward_term() {
    if [ -n "$_ra_native_pid" ]; then
        kill -TERM "$_ra_native_pid" 2>/dev/null
    fi
    if [ -n "$_ra_hid_pid" ]; then
        kill -TERM "$_ra_hid_pid" 2>/dev/null
    fi
}

trap '_ra_forward_term' HUP INT TERM

_ra_signal_owned_hid TERM
sleep 0.2
_ra_signal_owned_hid KILL
sleep 0.2

/armle/sbin/io-hid -d usb upath=/dev/io-usb/io-usb &
_ra_hid_pid=$!
sleep 0.5
if ! kill -0 "$_ra_hid_pid" 2>/dev/null; then
    echo "io-hid failed to start (pid=$_ra_hid_pid)"
    wait "$_ra_hid_pid" 2>/dev/null
    _ra_hid_pid=
    trap - HUP INT TERM 2>/dev/null || true
    exit 1
fi

"$RA_DIR/retroarch" --config "$RA_USER_CONFIG" "$@" &
_ra_native_pid=$!
_ra_finished_pid=$_ra_native_pid
wait "$_ra_native_pid"
_ra_native_status=$?
_ra_native_pid=

# SIGTERM can bypass the frontend's normal platform cleanup. The supervisor
# has just reaped this exact child, so clear only its matching PID lock plus
# the session-scoped PCM-ready latch. Never delete another process's lock.
if [ -f "$RA_LOCK_PATH" ]; then
    _ra_lock_pid=`cat "$RA_LOCK_PATH" 2>/dev/null`
    if [ "$_ra_lock_pid" = "$_ra_finished_pid" ]; then
        rm -f "$RA_LOCK_PATH"
    fi
fi
rm -f "$RA_QNX_AUDIO_READY_PATH"
_ra_stop_session_hid
trap - HUP INT TERM 2>/dev/null || true
exit "$_ra_native_status"
