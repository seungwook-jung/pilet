#!/bin/sh
# Build everything for the TrimUI Brick (aarch64 Linux):
#
#   1. gl4es          static libGL.a (GLES1 fixed-function -> GLES2)
#   2. SDL2           libSDL2-2.0.so.0 with KMSDRM + offscreen + ALSA
#   3. engine         Inotia3-wasm + overlay (pad mapping, mods), SDL host
#
#   scripts/build.sh            # all three
#   scripts/build.sh engine     # only the engine (after an overlay change)
#
# Requires scripts/setup.sh and scripts/fetch.sh to have run.
set -e
. "$(dirname "$0")/env.sh"
STEP=${1:-all}
mkdir -p "$BUILD"
[ -x "$CROSS_CC" ] || { echo "build: run scripts/setup.sh first"; exit 1; }
[ -d "$VENDOR/engine/src" ] || { echo "build: run scripts/fetch.sh first"; exit 1; }
NPROC=$(nproc 2>/dev/null || echo 4)

build_gl4es() {
    echo "== gl4es (aarch64)"
    rm -rf "$BUILD/gl4es"
    mkdir -p "$BUILD/gl4es"
    # NOEGL: the host (SDL) owns the context. DEFAULT_ES=2: the backend is the
    # firmware's GLES2 driver. NO_INIT_CONSTRUCTOR: the engine calls
    # initialize_gl4es() itself after setting the proc lookup.
    cmake -S "$VENDOR/gl4es" -B "$BUILD/gl4es" \
        -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-Wno-date-time" \
        -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON -DDEFAULT_ES=2 \
        -DNO_INIT_CONSTRUCTOR=ON -DNO_GBM=ON > "$BUILD/gl4es.log" 2>&1
    cmake --build "$BUILD/gl4es" -j"$NPROC" >> "$BUILD/gl4es.log" 2>&1
    cp -f "$VENDOR/gl4es/lib/libGL.a" "$BUILD/libGL-aarch64.a"
    echo "   -> $BUILD/libGL-aarch64.a"
}

build_sdl2() {
    echo "== SDL2 $SDL2_VER (aarch64)"
    # KMSDRM is gated on pkg-config finding libdrm, gbm and egl; the headers are
    # the host's (arch-independent) and the libraries are dlopen'ed on the
    # device, so the .pc files only need to exist.
    PC="$BUILD/pkgconfig"
    mkdir -p "$PC" "$BUILD/stubs"
    printf 'prefix=/usr\nincludedir=/usr/include\n\nName: libdrm\nDescription: libdrm\nVersion: 2.4.120\nCflags: -I${includedir} -I${includedir}/libdrm\nLibs: -ldrm\n' > "$PC/libdrm.pc"
    printf 'prefix=/usr\nincludedir=/usr/include\n\nName: gbm\nDescription: gbm\nVersion: 24.0\nCflags: -I${includedir}\nLibs: -lgbm\n' > "$PC/gbm.pc"
    printf 'prefix=/usr\nincludedir=/usr/include\n\nName: egl\nDescription: EGL\nVersion: 1.5\nCflags: -I${includedir}\nLibs: -lEGL\n' > "$PC/egl.pc"
    # ALSA: SDL links libasound (SDL_ALSA_SHARED needs a readable SONAME which
    # the cross detection cannot do reliably), so a stub exporting every
    # snd_* symbol SDL references stands in for the device's real library.
    grep -ohE "SDL_ALSA_SYM\((snd_[a-z0-9_]+)" "$VENDOR/SDL2-$SDL2_VER/src/audio/alsa/SDL_alsa_audio.c" \
        | sed 's/SDL_ALSA_SYM(//' | sort -u | sed 's/.*/void &(void) {}/' > "$BUILD/stubs/alsa_stub.c"
    "$CROSS_CC" -shared -Wl,-soname,libasound.so.2 -o "$BUILD/stubs/libasound.so" "$BUILD/stubs/alsa_stub.c"
    rm -rf "$BUILD/sdl2"
    PKG_CONFIG_LIBDIR="$PC" cmake -S "$VENDOR/SDL2-$SDL2_VER" -B "$BUILD/sdl2" \
        -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OBJDUMP="$(command -v aarch64-linux-gnu-objdump || command -v objdump)" \
        -DCMAKE_INSTALL_PREFIX="$BUILD/sdl2/dist" \
        -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=ON -DSDL_KMSDRM_SHARED=ON \
        -DSDL_OFFSCREEN=ON -DSDL_VULKAN=OFF \
        -DSDL_ALSA=ON -DSDL_ALSA_SHARED=OFF -DALSA_INCLUDE_DIR=/usr/include \
        -DALSA_LIBRARY="$BUILD/stubs/libasound.so" \
        -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
        -DSDL_LIBSAMPLERATE=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON \
        -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=ON -DSDL_RPATH=OFF \
        > "$BUILD/sdl2.log" 2>&1
    cmake --build "$BUILD/sdl2" -j"$NPROC" >> "$BUILD/sdl2.log" 2>&1
    cmake --install "$BUILD/sdl2" >> "$BUILD/sdl2.log" 2>&1
    LIB=$(ls "$BUILD/sdl2/dist/lib"/libSDL2-2.0.so.0.*.* | head -1)
    strings "$LIB" | grep -qx KMSDRM || { echo "build: SDL2 has no KMSDRM backend -- see $BUILD/sdl2.log"; exit 1; }
    echo "   -> $LIB (KMSDRM ok)"
}

build_engine() {
    echo "== engine (aarch64) + overlay"
    rm -rf "$BUILD/engine-src"
    mkdir -p "$BUILD/engine-src"
    cp -r "$VENDOR/engine/src" "$VENDOR/engine/third_party" "$BUILD/engine-src/"
    python3 -I "$ROOT/overlay/apply_overlay.py" "$BUILD/engine-src"
    SRC="$BUILD/engine-src/src"
    FILES=""
    for f in util elf_loader cpu host_hooks imports host_libc jni gl patches timers \
             asset_zip input text sound gl4es_bridge text_ttf platform_sdl main \
             mods pad_map; do
        FILES="$FILES $SRC/$f.c"
    done
    [ -f "$BUILD/libGL-aarch64.a" ] || { echo "build: no gl4es -- run build.sh gl4es"; exit 1; }
    [ -d "$BUILD/sdl2/dist/include/SDL2" ] || { echo "build: no SDL2 -- run build.sh sdl2"; exit 1; }
    "$CROSS_CC" -std=c11 -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-date-time \
        -DSPIKE_SDL -D_REENTRANT \
        -I"$SRC" -I"$BUILD/engine-src" -I"$VENDOR/gl-include" -I"$BUILD/sdl2/dist/include/SDL2" \
        $FILES "$BUILD/libGL-aarch64.a" \
        -L"$BUILD/sdl2/dist/lib" -lSDL2 -ldl -lm -lpthread \
        -Wl,-rpath,'$ORIGIN/libs' \
        -o "$BUILD/inotia3.aarch64"
    echo "   -> $BUILD/inotia3.aarch64"
    if command -v aarch64-linux-gnu-objdump >/dev/null 2>&1; then
        echo "   glibc floor: $(aarch64-linux-gnu-objdump -T "$BUILD/inotia3.aarch64" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    fi
}

case "$STEP" in
    all)    build_gl4es; build_sdl2; build_engine ;;
    gl4es)  build_gl4es ;;
    sdl2)   build_sdl2 ;;
    engine) build_engine ;;
    *) echo "usage: $0 [all|gl4es|sdl2|engine]"; exit 2 ;;
esac
echo "build: ok"
