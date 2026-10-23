#!/bin/zsh
# MetalSharp's MoltenVK 1.4.3 build (moltenvk-vkmt, universal x86_64 + arm64, minimum macOS 14),
# from the metalsharp-runtime bundle (release "bundles" of metalsharp/MetalSharp). This Wine is
# arm64-only, so only the arm64 slice is staged; package.sh uses it instead of the 1.4.2 from
# build-deps.sh.
#   stage-moltenvk.sh [path/to/universal/libMoltenVK.dylib]
set -e
source ${0:A:h}/env.sh
SRC=${1:-$ROOT/downloads/metalsharp-bundles/extract/metalsharp-runtime/runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib}
SHA=8249d81ebf2d46f82b16ca166c2e5cca5d76d91d0a412cd6d3db1aaa6e8430bf  # tools/bundles/moltenvk-runtime-hashes.tsv
[[ $(shasum -a 256 $SRC | cut -d' ' -f1) == $SHA ]] || { echo "not the MetalSharp MoltenVK 1.4.3 build: $SRC" >&2; exit 1; }
strings $SRC | grep -qx '1.4.3' || { echo "no MoltenVK 1.4.3 version string in $SRC" >&2; exit 1; }
O=$ROOT/deps/moltenvk
rm -rf $O; mkdir -p $O
lipo $SRC -thin arm64 -output $O/libMoltenVK.dylib
install_name_tool -id @rpath/libMoltenVK.dylib $O/libMoltenVK.dylib 2>/dev/null
codesign -f -s - $O/libMoltenVK.dylib 2>/dev/null
echo "MoltenVK 1.4.3 (MetalSharp build, sha256 $SHA, arm64 slice)" > $O/VERSION
echo "moltenvk: $O/libMoltenVK.dylib ($(lipo -archs $O/libMoltenVK.dylib))"
