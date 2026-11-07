#!/bin/zsh
# Builds the x86_64 MetalSharp Wine's modules (Wine 11.17 + WineForge + MetalSharp, src/wine-x86_64) in an x86_64
# build tree, for package-x86_64.sh, which lays them over the bundles release's runtime (see docs/X86_64.md):
#
#   X86_WINE_BUILD=<build tree configured from src/wine-x86_64> [X86_WINE_MODULES="ntdll win32u ..."] \
#     zsh scripts/build-wine-x86_64.sh
#
# The tree is configured for x86_64 (--enable-archs=i386,x86_64, clang -arch x86_64, minimum macOS 15.0); its PE
# side needs Homebrew's mingw-w64. Only the modules named are built, each as its own make target: they are the ones
# MetalSharp's x86_64 commits change, and what package-x86_64.sh takes from the tree (WINE_MODULES).
set -e
# not env.sh: that sets up the arm64 build's llvm-mingw, and this tree's PE side is built with Homebrew's mingw-w64
ROOT=${0:A:h:h}
B=${X86_WINE_BUILD:?set X86_WINE_BUILD to an x86_64 build tree configured from src/wine-x86_64}
MODULES=(${=X86_WINE_MODULES:-kernelbase ntdll win32u winemac.drv wined3d winevulkan wow64 server})
S=$ROOT/src/wine-x86_64

[[ -f $B/Makefile ]] || { echo "no configured build tree at $B" >&2; exit 1; }
whence -p x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc not found (brew install mingw-w64)" >&2; exit 1; }
# the source the tree was configured from
src=$(sed -n 's/^srcdir *= *//p' $B/Makefile | head -1)
[[ $src = /* ]] || src=$B/$src
if [[ -d $S && ${src:A} != ${S:A} ]]; then
  echo "note: $B was configured from $src, not $S; building that source" >&2
fi

targets=()
for m in $MODULES; do
  if [[ $m = server ]]; then targets+=(server/wineserver); else targets+=(dlls/$m); fi
done
make -C $B -j${JOBS:-$(sysctl -n hw.ncpu)} $targets
echo "built in $B: $MODULES"
