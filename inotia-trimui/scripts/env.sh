# Shared settings for the build scripts. Source it; do not run it.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${INOTIA_WORK:-$ROOT/work}
TOOLCHAIN=$WORK/toolchain
VENDOR=$WORK/vendor
BUILD=$WORK/build

# The engine: suchangv/Inotia3-wasm, pinned. The overlay in ../overlay is
# written against exactly this commit (apply_overlay.py asserts its anchors).
ENGINE_REPO=${ENGINE_REPO:-https://github.com/suchangv/Inotia3-wasm.git}
ENGINE_COMMIT=${ENGINE_COMMIT:-1d2b6b543cf22a4e7176526c50c90f56ea5aefe7}

# gl4es (MIT): GLES1 fixed-function -> GLES2, linked statically.
GL4ES_REPO=${GL4ES_REPO:-https://github.com/ptitSeb/gl4es.git}
GL4ES_COMMIT=${GL4ES_COMMIT:-c1d7d014fcf687ed33f30ce78b6480b3be1cbeee}

# SDL2 (zlib): built here with KMSDRM + offscreen + ALSA for the handheld.
SDL2_VER=${SDL2_VER:-2.30.9}
SDL2_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-$SDL2_VER.tar.gz

# Font with Hangul glyphs (SIL OFL 1.1), TrueType outlines (stb_truetype
# cannot read CFF/OTF). Shipped as font.ttf next to the binary.
FONT_URL=${FONT_URL:-https://raw.githubusercontent.com/google/fonts/main/ofl/nanumgothic/NanumGothic-Regular.ttf}
FONT_LICENSE_URL=${FONT_LICENSE_URL:-https://raw.githubusercontent.com/google/fonts/main/ofl/nanumgothic/OFL.txt}

# glibc floor for the aarch64 binaries (zig targets any version). 2.28 is
# older than every TrimUI / Knulli / muOS firmware seen so far.
ZIG_GLIBC=${ZIG_GLIBC:-2.28}
ZIG_TARGET=aarch64-linux-gnu.$ZIG_GLIBC

CROSS_CC=$TOOLCHAIN/zig-aarch64-cc
CMAKE_TC=$TOOLCHAIN/aarch64-zig.cmake
