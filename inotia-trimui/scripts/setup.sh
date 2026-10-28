#!/bin/sh
# One-time host setup (Ubuntu/Debian x86_64 or arm64). Installs the few
# packages the build needs and writes the zig-based aarch64 cross toolchain
# wrappers into work/toolchain.
#
#   scripts/setup.sh            # apt + pip + wrappers
#   scripts/setup.sh --no-apt   # wrappers only (packages already present)
set -e
. "$(dirname "$0")/env.sh"

if [ "${1:-}" != "--no-apt" ]; then
    if command -v apt-get >/dev/null 2>&1; then
        SUDO=""; [ "$(id -u)" != 0 ] && SUDO=sudo
        $SUDO apt-get install -y --no-install-recommends \
            build-essential cmake make git curl zip unzip python3 python3-pip \
            pkg-config binutils-aarch64-linux-gnu \
            libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libasound2-dev
    else
        echo "setup: no apt-get; install cmake, make, git, curl, zip, python3, pip,"
        echo "       aarch64 binutils and the libdrm/gbm/EGL/GLES/ALSA *headers* yourself."
    fi
fi

python3 -m pip install --quiet --user ziglang pyelftools 2>/dev/null || \
    python3 -m pip install --quiet ziglang pyelftools
python3 -m ziglang version >/dev/null

mkdir -p "$TOOLCHAIN"
for t in cc c++; do
    cat > "$TOOLCHAIN/zig-aarch64-$t" <<EOF
#!/bin/sh
exec python3 -m ziglang $t -target $ZIG_TARGET "\$@"
EOF
done
cat > "$TOOLCHAIN/zig-aarch64-ar" <<'EOF'
#!/bin/sh
exec python3 -m ziglang ar "$@"
EOF
cat > "$TOOLCHAIN/zig-aarch64-ranlib" <<'EOF'
#!/bin/sh
exec python3 -m ziglang ranlib "$@"
EOF
chmod +x "$TOOLCHAIN"/zig-aarch64-*
cat > "$CMAKE_TC" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "$TOOLCHAIN/zig-aarch64-cc")
set(CMAKE_CXX_COMPILER "$TOOLCHAIN/zig-aarch64-c++")
set(CMAKE_AR "$TOOLCHAIN/zig-aarch64-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$TOOLCHAIN/zig-aarch64-ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
echo "setup: toolchain in $TOOLCHAIN (target $ZIG_TARGET)"
echo 'int main(void){return 0;}' > "$TOOLCHAIN/t.c"
"$CROSS_CC" -o "$TOOLCHAIN/t.out" "$TOOLCHAIN/t.c" && rm -f "$TOOLCHAIN/t.c" "$TOOLCHAIN/t.out"
echo "setup: ok"
