#!/bin/zsh
# Builds the x86_64 MetalSharp Wine (Wine 11.22 + WineForge + MetalSharp, src/wine-x86_64) in an x86_64 build tree
# (configure-wine-x86_64.sh) and installs it for package-x86_64.sh (WINE_INSTALL), which puts it in place of the
# bundles release's Wine (see docs/X86_64.md):
#
#   X86_WINE_BUILD=<build tree> [X86_WINE_INSTALL=<dir>, default install-x86_64/wine] zsh scripts/build-wine-x86_64.sh
#
# With X86_WINE_MODULES="ntdll win32u ...", only those modules are built (each as its own make target) and nothing is
# installed: for package-x86_64.sh's WINE_BUILD and WINE_MODULES, over a bundle's Wine of the same version.
set -e
# not env.sh: that sets up the arm64 build's llvm-mingw, and this tree's PE side is built with Homebrew's mingw-w64
ROOT=${0:A:h:h}
B=${X86_WINE_BUILD:?set X86_WINE_BUILD to an x86_64 build tree configured from src/wine-x86_64}
I=${X86_WINE_INSTALL:-$ROOT/install-x86_64/wine}
S=$ROOT/src/wine-x86_64
export PATH=${HOMEBREW_PREFIX:-/opt/homebrew}/opt/bison/bin:${HOMEBREW_PREFIX:-/opt/homebrew}/opt/flex/bin:$PATH

[[ -f $B/Makefile ]] || { echo "no configured build tree at $B (configure-wine-x86_64.sh)" >&2; exit 1; }
whence -p x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc not found (brew install mingw-w64)" >&2; exit 1; }
# the source the tree was configured from
src=$(sed -n 's/^srcdir *= *//p' $B/Makefile | head -1)
[[ $src = /* ]] || src=$B/$src
if [[ -d $S && ${src:A} != ${S:A} ]]; then
  echo "note: $B was configured from $src, not $S; building that source" >&2
fi

if [[ -n $X86_WINE_MODULES ]]; then
  targets=()
  for m in ${=X86_WINE_MODULES}; do
    if [[ $m = server ]]; then targets+=(server/wineserver); else targets+=(dlls/$m); fi
  done
  make -C $B -j${JOBS:-$(sysctl -n hw.ncpu)} $targets
  echo "built in $B: $X86_WINE_MODULES"
  exit 0
fi

make -C $B -j${JOBS:-$(sysctl -n hw.ncpu)}
rm -rf $I
make -C $B -j${JOBS:-$(sysctl -n hw.ncpu)} install prefix=$I > $B/install.log
echo "built $B and installed it in $I: $($I/bin/wine --version)"
