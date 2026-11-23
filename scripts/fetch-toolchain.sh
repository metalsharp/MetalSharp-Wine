#!/bin/zsh
# llvm-mingw 20261020 (UCRT, macOS universal), the toolchain env.sh names as MINGW: Wine's, FEX's, Mullion's and
# every other PE module's compilers. Fetched from mstorsjo/llvm-mingw's release and checked against its SHA-256,
# into toolchain/. env.sh's mingw-shims (the target-prefixed tools, ahead of Homebrew's) are made again with it.
set -e
source ${0:A:h}/env.sh
VER=20261020 SHA=e9c13e4ede38e715003ffc674d4d00a1088df8235439d1f0c16188ff0704c17e
N=llvm-mingw-$VER-ucrt-macos-universal
[[ -x $MINGW/bin/arm64ec-w64-mingw32-clang ]] && { echo "toolchain: $MINGW"; exit 0; }
[[ $MINGW == $ROOT/toolchain/$N ]] || { echo "fetch-toolchain: env.sh's MINGW is not $N" >&2; exit 1; }
T=$ROOT/downloads/llvm-mingw.tar.xz
mkdir -p $ROOT/downloads $ROOT/toolchain
[[ -f $T && $(shasum -a 256 $T | cut -d' ' -f1) == $SHA ]] ||
  { curl -fL --progress-bar -o $T.part https://github.com/mstorsjo/llvm-mingw/releases/download/$VER/$N.tar.xz; mv $T.part $T; }
[[ $(shasum -a 256 $T | cut -d' ' -f1) == $SHA ]] || { echo "checksum mismatch: $T" >&2; exit 1; }
rm -rf $MINGW $ROOT/toolchain/mingw-shims
tar -C $ROOT/toolchain -xJf $T
source ${0:A:h}/env.sh   # the shims
echo "toolchain: $MINGW"
