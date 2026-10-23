# Common build environment: native arm64, macOS 15.0 minimum.
# Usage: source scripts/env.sh
export ROOT=${ROOT:-${${(%):-%x}:A:h:h}}  # the checkout this file is in
export PREFIX=$ROOT/deps/prefix
export MINGW=$ROOT/toolchain/llvm-mingw-20261020-ucrt-macos-universal
export JOBS=10

export MACOSX_DEPLOYMENT_TARGET=15.0
export SDKROOT=$(xcrun --show-sdk-path)

export CC="/usr/bin/clang -arch arm64"
export CXX="/usr/bin/clang++ -arch arm64"
export OBJC="/usr/bin/clang -arch arm64"
export CFLAGS="-O2 -mmacosx-version-min=15.0 -Werror=unguarded-availability-new"
export CXXFLAGS="$CFLAGS"
export OBJCFLAGS="$CFLAGS"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib -mmacosx-version-min=15.0 -Wl,-headerpad_max_install_names"

# Only our own prefix (plus SDK-provided bits) is visible to pkg-config.
export PKG_CONFIG=/opt/homebrew/bin/pkgconf
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH

# Host build tools from Homebrew (bison/flex/gettext are keg-only), then llvm-mingw. Its unprefixed
# clang must not shadow Apple's, but its target-prefixed tools must win over Homebrew's GNU mingw
# binutils: GNU dlltool writes long-format import libraries, which lld cannot delay-load from, so
# every i386 delay import silently became a load-time import (and an import cycle via ucrtbase).
SHIMS=$ROOT/toolchain/mingw-shims
if [[ ! -d $SHIMS ]]; then
  mkdir -p $SHIMS
  for t in $MINGW/bin/*-w64-mingw32-*; do ln -sf $t $SHIMS/; done
fi
export PATH=$SHIMS:$PREFIX/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/gettext/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:$MINGW/bin

# Keep autoconf from picking up libc functions newer than the deployment target.
export CONFIG_SITE=$ROOT/scripts/config.site
