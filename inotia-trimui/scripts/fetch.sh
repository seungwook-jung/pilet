#!/bin/sh
# Fetch every third-party source the build needs into work/vendor. Nothing
# from the game is fetched: the APK is the player's own.
set -e
. "$(dirname "$0")/env.sh"
mkdir -p "$VENDOR"

if [ ! -d "$VENDOR/engine/.git" ]; then
    echo "== engine: $ENGINE_REPO @ $ENGINE_COMMIT"
    git clone -q "$ENGINE_REPO" "$VENDOR/engine"
fi
git -C "$VENDOR/engine" fetch -q origin "$ENGINE_COMMIT" 2>/dev/null || git -C "$VENDOR/engine" fetch -q origin
git -C "$VENDOR/engine" checkout -q "$ENGINE_COMMIT"

if [ ! -d "$VENDOR/gl4es/.git" ]; then
    echo "== gl4es: $GL4ES_REPO @ $GL4ES_COMMIT"
    git clone -q "$GL4ES_REPO" "$VENDOR/gl4es"
fi
git -C "$VENDOR/gl4es" checkout -q "$GL4ES_COMMIT" 2>/dev/null || {
    git -C "$VENDOR/gl4es" fetch -q origin; git -C "$VENDOR/gl4es" checkout -q "$GL4ES_COMMIT"; }

if [ ! -d "$VENDOR/SDL2-$SDL2_VER" ]; then
    echo "== SDL2 $SDL2_VER"
    curl -sSL -o "$VENDOR/sdl2.tar.gz" "$SDL2_URL"
    tar xzf "$VENDOR/sdl2.tar.gz" -C "$VENDOR"
    rm -f "$VENDOR/sdl2.tar.gz"
fi

mkdir -p "$VENDOR/fonts"
if [ ! -f "$VENDOR/fonts/font.ttf" ]; then
    echo "== font: $FONT_URL"
    curl -sSL -o "$VENDOR/fonts/font.ttf" "$FONT_URL"
    curl -sSL -o "$VENDOR/fonts/FONT-LICENSE.txt" "$FONT_LICENSE_URL" || true
fi

# GL/EGL headers are arch-independent; keep them away from /usr/include so
# the host's libc headers never reach the cross compiler.
mkdir -p "$VENDOR/gl-include"
for d in EGL GLES GLES2 GLES3 KHR; do
    [ -d "/usr/include/$d" ] && cp -r "/usr/include/$d" "$VENDOR/gl-include/" || true
done
[ -f "$VENDOR/gl-include/GLES2/gl2.h" ] || { echo "fetch: GLES2/gl2.h missing -- install libgles2-mesa-dev"; exit 1; }
echo "fetch: ok ($VENDOR)"
