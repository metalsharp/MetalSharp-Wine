#!/bin/zsh
# Build the Windows test/benchmark programs for every PE architecture we run.
# A source can restrict its architectures with a "/* archs: ... */" first line.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
mkdir -p $ROOT/tests/bin
for src in $ROOT/tests/src/*.c; do
  archs=($(sed -n '1s|^/\* archs: \(.*\) \*/$|\1|p' $src))
  (( $#archs )) || archs=(aarch64 x86_64 i686)
  for t in $archs; do
    # no FMA contraction, so results are bit-comparable across architectures
    $t-w64-mingw32-clang -O2 -ffp-contract=off -I $PREFIX/include $src -o $ROOT/tests/bin/${src:t:r}-$t.exe
  done
done
