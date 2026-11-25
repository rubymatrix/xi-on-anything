#!/bin/bash
# What the iOS launcher app makes the game with on the phone, built for iOS (device and simulator):
#
#   build/ios-llvm/{ios,sim}      LLVM 18's clang and lld as static libraries (ios_toolchain.cpp)
#   build/ios-python/cross-build  CPython 3.14 for iOS (its own Apple/ build): Python.xcframework
#   build/ios-capstone/<sdk>      capstone 5 (x86 only), the translator's disassembler
#
# Then tools/build_ios.py toolchain --sdk <sdk> makes the launcher's bundle from them. The sources are
# downloaded once into build/ (shared with build_android.py's on-phone build). An hour or so, most of
# it LLVM. Usage: tools/build_ios_deps.sh [llvm|python|capstone ...] (default: all three).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
PY="${PYTHON:-python3.14}"
LLVM_VER=18.1.8 PY_VER=3.14.7 CS_VER=5.0.9
if [ $# -gt 0 ]; then WHAT=("$@"); else WHAT=(llvm python capstone); fi

fetch() { # url dest-dir
  local name; name=$(basename "$1")
  mkdir -p "$2"
  if [ ! -f "$2/$name" ]; then
    curl -fL --retry 3 -o "$2/$name.part" "$1" >&2
    mv "$2/$name.part" "$2/$name"
  fi
  echo "$2/$name"
}

llvm() {
  local src="$B/android-llvm/src/llvm-project-$LLVM_VER.src"
  if [ ! -d "$src" ]; then
    tar -xf "$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz" "$B/android-llvm/src")" -C "$B/android-llvm/src"
  fi
  # the build machine's tablegen tools first
  local native="$B/android-llvm/native"
  if [ ! -x "$native/bin/clang-tblgen" ]; then
    cmake -G Ninja -S "$src/llvm" -B "$native" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang \
      -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
    ninja -C "$native" llvm-tblgen clang-tblgen llvm-min-tblgen
  fi
  for sdk in iphoneos iphonesimulator; do
    local out="$B/ios-llvm/$([ $sdk = iphoneos ] && echo ios || echo sim)"
    local triple="arm64-apple-ios17.0$([ $sdk = iphoneos ] || echo -simulator)"
    cmake -G Ninja -S "$src/llvm" -B "$out" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=$sdk \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_MACOSX_BUNDLE=OFF -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 \
      -DLLVM_HOST_TRIPLE=$triple -DLLVM_DEFAULT_TARGET_TRIPLE=$triple \
      -DLLVM_NATIVE_TOOL_DIR="$native/bin" -DLLVM_TABLEGEN="$native/bin/llvm-tblgen" -DCLANG_TABLEGEN="$native/bin/clang-tblgen" \
      -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
      -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_PIC=ON -DLLVM_ENABLE_THREADS=ON \
      -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_UTILS=OFF \
      -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_INCLUDE_TESTS=OFF \
      -DLLVM_BUILD_TOOLS=OFF -DCLANG_BUILD_TOOLS=OFF -DLLVM_ENABLE_BINDINGS=OFF
    ninja -C "$out" clangFrontendTool clangCodeGen lldMachO lldCommon LLVMAArch64CodeGen LLVMAArch64AsmParser clang-resource-headers
  done
}

python() {
  local src="$B/android-python/src/Python-$PY_VER"
  if [ ! -d "$src" ]; then
    tar -xf "$(fetch "https://www.python.org/ftp/python/$PY_VER/Python-$PY_VER.tgz" "$B/android-python/src")" -C "$B/android-python/src"
  fi
  # CPython's own iOS build (Apple/): every slice, and the XCframework (its packaging step after is not needed)
  (cd "$src" && "$PY" Apple build iOS all --cross-build-dir "$B/ios-python/cross-build" --cache-dir "$B/ios-python/cache") || true
  [ -d "$B/ios-python/cross-build/iOS/Python.xcframework" ] || { echo "CPython for iOS did not build" >&2; exit 1; }
}

capstone() {
  local src="$B/android-python/capstone/capstone-$CS_VER"
  if [ ! -d "$src" ]; then
    tar -xf "$(fetch "https://files.pythonhosted.org/packages/source/c/capstone/capstone-$CS_VER.tar.gz" "$B/android-python/capstone")" -C "$B/android-python/capstone"
  fi
  for sdk in iphoneos iphonesimulator; do
    # ARCHFLAGS: capstone's CMakeLists makes every Apple build x86_64+arm64 without it
    ARCHFLAGS="-arch arm64" cmake -G Ninja -S "$src/src" -B "$B/ios-capstone/$sdk" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 \
      -DCMAKE_OSX_SYSROOT=$sdk -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
      -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_X86_SUPPORT=ON -DBUILD_SHARED_LIBS=ON -DCAPSTONE_BUILD_TESTS=OFF \
      -DCAPSTONE_BUILD_CSTOOL=OFF -DCMAKE_MACOSX_BUNDLE=OFF -DCMAKE_INSTALL_NAME_DIR=@rpath
    ninja -C "$B/ios-capstone/$sdk"
  done
}

for w in "${WHAT[@]}"; do "$w"; done
