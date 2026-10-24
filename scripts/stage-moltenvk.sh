#!/bin/zsh
# MetalSharp's MoltenVK 1.4.3 (src/vkd3d-proton/sources/MoltenVK: moltenvk-vkmt with the VKD3D-Proton work,
# scripts/build-moltenvk.sh there), arm64 slice (this Wine is arm64-only); package.sh uses it instead of the
# 1.4.2 from build-deps.sh.
#   stage-moltenvk.sh [path/to/libMoltenVK.dylib]
set -e
source ${0:A:h}/env.sh
MVK=$ROOT/src/vkd3d-proton/sources/MoltenVK
SRC=${1:-$ROOT/src/vkd3d-proton/artifacts/mvk-new/libMoltenVK.dylib}
strings $SRC | grep -qx '1.4.3' || { echo "no MoltenVK 1.4.3 version string in $SRC" >&2; exit 1; }
O=$ROOT/deps/moltenvk
rm -rf $O; mkdir -p $O
if [[ $(lipo -archs $SRC) == arm64 ]]; then cp $SRC $O/libMoltenVK.dylib; else lipo $SRC -thin arm64 -output $O/libMoltenVK.dylib; fi
install_name_tool -id @rpath/libMoltenVK.dylib $O/libMoltenVK.dylib 2>/dev/null
codesign -f -s - $O/libMoltenVK.dylib 2>/dev/null
echo "MoltenVK 1.4.3 (MetalSharp build $(git -C $MVK rev-parse --short HEAD 2>/dev/null), SPIRV-Cross $(git -C $MVK/External/SPIRV-Cross rev-parse --short HEAD 2>/dev/null), sha256 $(shasum -a 256 $SRC | cut -d' ' -f1), arm64 slice)" > $O/VERSION
echo "moltenvk: $O/libMoltenVK.dylib ($(lipo -archs $O/libMoltenVK.dylib)); $(cat $O/VERSION)"
