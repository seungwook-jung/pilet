#!/bin/sh
# Assemble the PortMaster-style package for the TrimUI Brick:
#
#   dist/Inotia3.sh            the launcher (shows up in the Ports list)
#   dist/inotia3/              the port directory
#       inotia3.aarch64        engine (interpreter + gl4es, SDL host)
#       libs/libSDL2-2.0.so.0  bundled SDL2 (used when PortMaster has none)
#       font.ttf               Hangul-capable font (OFL)
#       pad.txt env.txt mods.txt port.json README.txt
#       data/                  saves + installed game data (created at run time)
#       libgame.so, assets.apk <- NOT included: the player's own files
#
#   scripts/package.sh [--zip]
set -e
. "$(dirname "$0")/env.sh"
DIST="$ROOT/dist"
NAME=Inotia3
OUT="$DIST/inotia3"
[ -f "$BUILD/inotia3.aarch64" ] || { echo "package: build first (scripts/build.sh)"; exit 1; }

rm -rf "$OUT" "$DIST/$NAME.sh" "$DIST/$NAME.zip"
mkdir -p "$OUT/libs" "$OUT/data"
cp "$BUILD/inotia3.aarch64" "$OUT/inotia3.aarch64"
if command -v aarch64-linux-gnu-strip >/dev/null 2>&1; then
    aarch64-linux-gnu-strip "$OUT/inotia3.aarch64" || true
fi
# A real file named by SONAME (zips do not keep symlinks).
cp -f "$(ls "$BUILD/sdl2/dist/lib"/libSDL2-2.0.so.0.*.* | head -1)" "$OUT/libs/libSDL2-2.0.so.0"
[ -f "$VENDOR/fonts/font.ttf" ] && cp -f "$VENDOR/fonts/font.ttf" "$OUT/font.ttf"
[ -f "$VENDOR/fonts/FONT-LICENSE.txt" ] && cp -f "$VENDOR/fonts/FONT-LICENSE.txt" "$OUT/FONT-LICENSE.txt"
cp "$ROOT/port/port.json" "$ROOT/port/README.txt" "$ROOT/port/pad.txt" "$ROOT/port/env.txt" "$OUT/"
cp "$ROOT/mods/mods.txt" "$OUT/mods.txt"
cp "$ROOT/port/launcher.sh" "$DIST/$NAME.sh"
chmod +x "$DIST/$NAME.sh" "$OUT/inotia3.aarch64"
echo "   -> $DIST/$NAME.sh"
echo "   -> $OUT ($(du -sh "$OUT" | cut -f1))"
if [ "${1:-}" = "--zip" ]; then
    ( cd "$DIST" && zip -qr "$NAME.zip" "$NAME.sh" inotia3 )
    echo "   -> $DIST/$NAME.zip ($(du -h "$DIST/$NAME.zip" | cut -f1))"
fi
echo
echo "Copy $NAME.sh and inotia3/ into the Ports folder of the SD card, then put"
echo "libgame.so (lib/armeabi/libgame.so from the APK) and assets.apk (the APK"
echo "itself, renamed) into inotia3/.  tools/inotia_tool.py prepare does both."
