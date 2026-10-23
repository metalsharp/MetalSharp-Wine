#!/bin/zsh
# Configure, build and install Wine 11.20 as native arm64 macOS (minos 15.0)
# with PE archs arm64ec (x86_64 via ARM64EC), aarch64 and i386 (WoW64).
# Usage: build-wine.sh [configure|build|install|all]   (default: all)
set -e
source ${0:A:h}/env.sh
# In-tree build: --with-gettextpo (po rebuilding) is unsupported out of tree.
B=$ROOT/src/wine; I=$ROOT/install
step=${1:-all}
mkdir -p $B

if [[ $step == configure || $step == all ]]; then
  # Requested features are passed as explicit --with so a missing one is a hard error.
  (cd $B && ./configure --prefix=$I \
     --enable-archs=arm64ec,aarch64,i386 --with-mingw=llvm-mingw \
     --with-vulkan --with-opengl --with-gnutls --with-gstreamer --with-ffmpeg --with-opencl \
     --with-coreaudio --with-cups --with-freetype --with-gettext --with-gettextpo \
     --with-pthread --with-sdl \
     --without-x --without-wayland \
     LDFLAGS="$LDFLAGS -Wl,-rpath,$PREFIX/lib") > $ROOT/logs/wine-configure.log 2>&1
  grep -E "^configure: (WARNING|error)|won't be supported" $ROOT/logs/wine-configure.log || true
fi
if [[ $step == build || $step == all ]]; then
  make -C $B -j$JOBS > $ROOT/logs/wine-make.log 2>&1
fi
if [[ $step == install || $step == all ]]; then
  make -C $B -j$JOBS install > $ROOT/logs/wine-install.log 2>&1
fi
