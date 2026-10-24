#!/bin/zsh
# Build the FEX fork (src/FEX, branch metalsharp) as Wine PE modules with llvm-mingw:
#   libarm64ecfex.dll  x86_64 emulation through ARM64EC
#   libwow64fex.dll    i386 emulation through WoW64
# Generic armv8 host code (-mcpu=apple-m1 trips an LLVM ARM64EC SEH unwind bug); the JIT targets the detected CPU. No unix library: hardware TSO is never used.
# Usage: build-fex.sh [arm64ec|wow64]...   (default: both)
set -e
source ${0:A:h}/env.sh
S=$ROOT/src/FEX

build() {  # build <name> <mingw triple> <lto>
  local B=$ROOT/build/fex-$1
  # host (macOS) flags must not leak into the PE cross build
  local CFLAGS= CXXFLAGS= CPPFLAGS= LDFLAGS= OBJCFLAGS= CC= CXX= OBJC= SDKROOT= MACOSX_DEPLOYMENT_TARGET=
  cmake -S $S -B $B -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$S/Data/CMake/toolchain_mingw.cmake -DMINGW_TRIPLE=$2 \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=$3 -DENABLE_CCACHE=FALSE \
    -DBUILD_TESTING=FALSE -DBUILD_FEXCONFIG=FALSE -DENABLE_JEMALLOC_GLIBC_ALLOC=FALSE \
    -DTUNE_ARCH=generic -DTUNE_CPU=none -DRANGES_NATIVE=OFF -DCMAKE_INSTALL_PREFIX=$ROOT/build/fex-install \
    > $ROOT/logs/fex-$1-cmake.log 2>&1
  ninja -C $B -j$JOBS > $ROOT/logs/fex-$1-build.log 2>&1
  ninja -C $B install > $ROOT/logs/fex-$1-install.log 2>&1
}

targets=(${@:-arm64ec wow64}); (( $# )) || targets=(arm64ec wow64)
for t in $targets; do
  case $t in
    arm64ec) build arm64ec arm64ec-w64-mingw32 FALSE ;;  # lld can't resolve EC symbols from archives under LTO
    wow64)   build wow64 aarch64-w64-mingw32 FALSE ;;  # LTO drops the asm-defined BTCpuSimulateImpl
    *)       echo "unknown target $t"; exit 1 ;;
  esac
  echo "== fex $t: ok"
done

# Wine finds the emulators as builtin DLLs (wine.inf points Wow64\amd64 and \x86 at them).
mkdir -p $ROOT/install/lib/wine/aarch64-windows
cp $ROOT/build/fex-install/lib/libarm64ecfex.dll(N) $ROOT/build/fex-install/lib/libwow64fex.dll(N) $ROOT/install/lib/wine/aarch64-windows/

# FEX's unix side (file mapping for its disk cache, process id; the Linux prctl requests report
# failure on macOS), loaded by Wine by name from aarch64-unix for each of the two DLLs.
mkdir -p $ROOT/install/lib/wine/aarch64-unix
for n in arm64ecfex wow64fex; do
  clang++ -std=c++20 -O2 -shared -fPIC -arch arm64 -mmacosx-version-min=15.0 -I$S/Source/Windows/UnixLib \
    $S/Source/Windows/UnixLib/FEXUnixLib.cpp -o $ROOT/install/lib/wine/aarch64-unix/lib$n.so
done
echo "== fex unixlib: ok"
