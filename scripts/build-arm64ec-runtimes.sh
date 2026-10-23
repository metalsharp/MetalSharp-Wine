#!/bin/zsh
# llvm-mingw releases ship aarch64 runtimes without ARM64EC objects. Rebuild the
# aarch64 CRT, compiler-rt and libc++ as ARM64X (aarch64 + arm64ec) into our toolchain
# with llvm-mingw's own scripts at the same tag/LLVM version. Needed for FEX (C++).
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS OBJCFLAGS CC CXX OBJC SDKROOT MACOSX_DEPLOYMENT_TARGET CONFIG_SITE
export PATH=$MINGW/bin:$PATH TOOLCHAIN_ARCHS="aarch64 arm64ec" CORES=$JOBS
cd $ROOT/src/llvm-mingw
./build-mingw-w64.sh $MINGW
./build-compiler-rt.sh $MINGW
./build-libcxx.sh $MINGW
./build-mingw-w64-libraries.sh $MINGW
echo "arm64ec runtimes: ok"
