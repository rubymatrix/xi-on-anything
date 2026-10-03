#!/bin/sh
# The Linux kit for x86_64 (the Steam Deck and other distributions): SDL3 and glslang built static
# with zig, then tools/build_posix.py kit (every library and every source that does not name a game
# build's addresses). The launcher downloads it with this commit's sources and zig, and builds host64
# on the player's machine from their own game files (build_posix.py host64 --kit).
#
#   docker build --platform linux/amd64 -t xi-linux-amd64 tools/linux
#   docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src xi-linux-amd64 tools/linux/kit.sh
#
# Out: build/xi-kit-linux-x86_64.tar.gz (kit.json, lib/*.a).
set -e
TARGET=${XI_TARGET:-x86_64-linux-gnu.2.35} # glibc 2.35 and later: SteamOS 3.5+, Ubuntu 22.04+
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
DEPS="$ROOT/build/kit-deps"
BIN="$ROOT/build/kit-bin"
mkdir -p "$DEPS" "$BIN"

# zig as the C and C++ compilers (one command each, for cmake). A -target is a cross build to zig: it
# looks in no system folder, so the system's headers (X11, ALSA, Vulkan: what SDL loads at run time
# and gfx_vulkan.c declares) are searched after zig's own glibc
SYS="-idirafter /usr/include -idirafter /usr/include/$(uname -m)-linux-gnu"
printf '#!/bin/sh\nexec zig cc -target %s %s "$@"\n' "$TARGET" "$SYS" > "$BIN/zcc"
printf '#!/bin/sh\nexec zig c++ -target %s %s "$@"\n' "$TARGET" "$SYS" > "$BIN/zcxx"
chmod +x "$BIN/zcc" "$BIN/zcxx"

cmake_dep() { # name url sha256 cmake-flags...
    name=$1 url=$2 sha=$3
    shift 3
    [ -f "$DEPS/.$name" ] && return
    src="$ROOT/build/kit-src/$name"
    mkdir -p "$src"
    curl -fsSL "$url" -o "$src.tar.gz"
    echo "$sha  $src.tar.gz" | sha256sum -c -
    tar xzf "$src.tar.gz" -C "$src" --strip-components=1
    cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DCMAKE_C_COMPILER="$BIN/zcc" -DCMAKE_CXX_COMPILER="$BIN/zcxx" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_LIBRARY_PATH="/usr/lib/$(uname -m)-linux-gnu" "$@" # zig tells cmake of no multiarch folder
    cmake --build "$src/build"
    cmake --install "$src/build"
    touch "$DEPS/.$name"
}

# SDL3: static, its video, audio and input back ends opened at run time (X11, Wayland, PipeWire,
# PulseAudio, ALSA, udev), so the binary needs none of them to start
cmake_dep sdl3 https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
    7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_VULKAN=ON -DSDL_DEPS_SHARED=ON \
    -DSDL_CAMERA=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF
# glslang: the shaders' compiler (gfx_vulkan.c), without its optimizer
cmake_dep glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/15.1.0.tar.gz \
    4bdcd8cdb330313f0d4deed7be527b0ac1c115ff272e492853a6e98add61b4bc \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF -DENABLE_SPVREMAPPER=OFF

rm -rf build/kit
XI_CC="$BIN/zcc" XI_CXX="$BIN/zcxx" XI_DEPS="$DEPS" XI_STATIC=1 PKG_CONFIG_PATH="$DEPS/lib/pkgconfig" \
    python3 tools/build_posix.py kit --out build/kit
tar czf build/xi-kit-linux-x86_64.tar.gz -C build/kit .
echo "kit: build/xi-kit-linux-x86_64.tar.gz"
