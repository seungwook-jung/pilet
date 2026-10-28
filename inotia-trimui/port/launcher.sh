#!/bin/sh
# Inotia 3: Children of Carnia -- PortMaster-style launcher for the TrimUI
# Brick / Smart Pro (and any other aarch64 Linux handheld with PortMaster).
#
# Lives next to the port directory:
#
#     Inotia3.sh              this file
#     inotia3/inotia3.aarch64 the engine (ARM interpreter + gl4es + SDL2 host)
#     inotia3/libgame.so      <- player-supplied: lib/armeabi/libgame.so from the APK
#     inotia3/assets.apk      <- player-supplied: the APK itself, renamed
#
# Derived from the Inotia3-wasm project's PortMaster launcher; changes for
# the TrimUI family: more PortMaster folder candidates (MinUI/NextUI pak,
# Knulli, muOS, stock), PortMaster's device SDL2 preferred over the bundled
# one when it exists, and a Korean default locale (env.txt).

SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || SELFDIR=.
PORTDIR="$SELFDIR/inotia3"
DEBUG=${INOTIA_DEBUG:-${SPIKE_DEBUG:-}}

# A zip unpacked by Windows' "Extract All" lands one level down.
if [ ! -f "$PORTDIR/inotia3.aarch64" ]; then
    for d in "$SELFDIR"/*/inotia3; do
        [ -f "$d/inotia3.aarch64" ] && PORTDIR=$d && break
    done
fi

LOG="$PORTDIR/inotia3.log"
{ : > "$LOG"; } 2>/dev/null || LOG="$SELFDIR/inotia3.log"
HAVE3=
if ( : >&1 ) 2>/dev/null; then
    exec 3>&1 2>/dev/null && HAVE3=1
fi
exec >> "$LOG" 2>&1

say() {
    printf '%s\n' "$*"
    [ -n "$HAVE3" ] && printf '%s\n' "$*" >&3 2>/dev/null
    return 0
}
fail() {
    say "Inotia 3: $*"
    if [ -w /dev/tty0 ]; then
        printf '\033[2J\033[H  Inotia 3: %s\n\n  (details in inotia3.log)\n' "$*" > /dev/tty0 2>/dev/null
    fi
    [ -n "$HAVE3" ] && sleep 10
    exit 1
}

say "--- $(date) ---"
[ -n "$DEBUG" ] && set -x

[ -d "$PORTDIR" ] || fail "no inotia3 directory next to the launcher"
cd "$PORTDIR" || fail "cannot enter $PORTDIR"
ENGINE=./inotia3.aarch64
[ -f "$ENGINE" ] || fail "no inotia3.aarch64 in $PORTDIR"
chmod 755 "$ENGINE" 2>/dev/null
chmod 644 libs/*.so* 2>/dev/null

missing=
[ -f libgame.so ] || missing="$missing libgame.so"
[ -f assets.apk ] || missing="$missing assets.apk"
if [ -n "$missing" ]; then
    say "  libgame.so  = lib/armeabi/libgame.so from your own Inotia 3 APK (v1.3.2)"
    say "  assets.apk  = the APK itself, renamed"
    say "  (tools/inotia_tool.py prepare <apk> <this dir> does both)"
    fail "missing$missing in $PORTDIR"
fi

# env.txt: KEY=VALUE per line. SPIKE_LANG=ko-KR, SPIKE_ROTATE=0/90/180/270,
# SDL_VIDEODRIVER=KMSDRM, INOTIA_USE_BUNDLED_SDL=1, INOTIA_DEBUG=1 ...
if [ -f env.txt ]; then
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        export "$line" 2>/dev/null || say "  env.txt: ignoring '$line'"
    done < env.txt
fi

# PortMaster's control folder, wherever this firmware keeps it.
controlfolder=""
for c in "/roms/ports/PortMaster" "/roms/ports/portmaster" "/roms/ports/PortMaster-master" \
         "/userdata/roms/ports/PortMaster" \
         "/mnt/SDCARD/Roms/Ports (PORTS)/PortMaster" "/mnt/SDCARD/Roms/PORTS/PortMaster" \
         "/mnt/SDCARD/Roms/Ports/PortMaster" \
         "/mnt/mmc/MUOS/PortMaster" "/mnt/sdcard/MUOS/PortMaster" \
         "$SELFDIR/PortMaster" "$SELFDIR/../PortMaster"; do
    [ -f "$c/control.txt" ] && controlfolder=$c && break
done
if [ -n "$controlfolder" ]; then
    say "portmaster: $controlfolder"
    . "$controlfolder/control.txt"
    get_controls 2>/dev/null || true
else
    say "portmaster: control.txt not found (running standalone)"
fi

# SDL2: PortMaster's device build first (it knows this firmware's KMSDRM /
# GLES quirks), the bundled one otherwise. INOTIA_USE_BUNDLED_SDL=1 forces ours.
SDL_DIR=""
if [ -z "${INOTIA_USE_BUNDLED_SDL:-}" ] && [ -n "$controlfolder" ]; then
    for d in "$controlfolder/libs.${DEVICE_ARCH:-aarch64}" "$controlfolder/libs"; do
        [ -f "$d/libSDL2-2.0.so.0" ] && SDL_DIR=$d && break
    done
fi
if [ -n "$SDL_DIR" ]; then
    export LD_LIBRARY_PATH="$SDL_DIR:$PORTDIR/libs:${LD_LIBRARY_PATH:-}"
    say "sdl2: $SDL_DIR (PortMaster), bundled as fallback"
else
    export LD_LIBRARY_PATH="$PORTDIR/libs:${LD_LIBRARY_PATH:-}"
    say "sdl2: bundled"
fi

say "device: ${DEVICE_NAME:-?} cfw: ${CFW_NAME:-?} panel: ${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?} locale: ${SPIKE_LANG:-${LC_ALL:-${LANG:-?}}}"

# Video backend: let the engine probe which SDL driver can open a window and
# take the first one (KMSDRM on a bare framebuffer firmware; wayland/x11 if a
# compositor is running). SDL_VIDEODRIVER in env.txt overrides.
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
    probe=$("$ENGINE" --vdprobe 2>&1)
    SDL_VIDEODRIVER=$(printf '%s\n' "$probe" \
        | grep -E '^[[:space:]]*[A-Za-z0-9_]+[[:space:]]+.*window ok' | head -1 | awk '{print $1}')
    if [ -n "$DEBUG" ] || [ -z "$SDL_VIDEODRIVER" ]; then
        printf '%s\n' "$probe" | sed 's/^/    /'
    fi
    [ -n "$SDL_VIDEODRIVER" ] && export SDL_VIDEODRIVER
fi
say "video: ${SDL_VIDEODRIVER:-auto}"

[ -n "${pm_platform_helper:-}" ] && $pm_platform_helper "$PORTDIR/$ENGINE" 2>/dev/null
say "--- starting ---"
"$ENGINE" "$@"
rc=$?
say "--- exit $rc ---"
[ -n "${pm_finish:-}" ] && $pm_finish 2>/dev/null

if [ "$LOG" != "$SELFDIR/inotia3.log" ]; then
    cp -f "$LOG" "$SELFDIR/inotia3.log" 2>/dev/null
fi
if [ $rc -ne 0 ]; then
    say "dmesg (tail):"
    dmesg 2>/dev/null | tail -15 | sed 's/^/    /'
    case $rc in
        126) say "126 = not executable (card mounted noexec?)" ;;
        127) say "127 = the dynamic loader refused the binary:"
             ( LD_TRACE_LOADED_OBJECTS=1 "$ENGINE" ) 2>&1 | sed 's/^/    /' ;;
    esac
    fail "the engine exited with code $rc"
fi
exit 0
