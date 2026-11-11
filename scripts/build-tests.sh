#!/bin/zsh
# Build the Windows test/benchmark programs for every PE architecture we run.
# A source says what it needs in comment lines among its first three:
#   /* archs: x86_64 i686 */     the architectures it is built for (this one must be the first line; default: all three)
#   /* libs: d3d11 dxgi */       import libraries beyond the defaults
#   /* needs: WebView2.h */      a header that is not in the toolchain; the header is looked for under downloads/, and the
#                                program is skipped when it is not there
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
mkdir -p $ROOT/tests/bin
for src in $ROOT/tests/src/*.c; do
  archs=($(sed -n '1s|^/\* archs: \(.*\) \*/$|\1|p' $src))
  (( $#archs )) || archs=(aarch64 x86_64 i686)
  libs=($(sed -n '1,3s|^/\* libs: \(.*\) \*/$|\1|p' $src))
  inc=()
  for h in $(sed -n '1,3s|^/\* needs: \(.*\) \*/$|\1|p' $src); do
    f=$(find $ROOT/downloads -name $h -print -quit 2>/dev/null)
    [[ -n $f ]] || { echo "skipping ${src:t}: $h is not under downloads/"; continue 2; }
    inc+=(-I ${f:h})
  done
  for t in $archs; do
    # no FMA contraction, so results are bit-comparable across architectures
    $t-w64-mingw32-clang -O2 -ffp-contract=off -I $PREFIX/include $inc $src -o $ROOT/tests/bin/${src:t:r}-$t.exe ${libs/#/-l}
  done
done
