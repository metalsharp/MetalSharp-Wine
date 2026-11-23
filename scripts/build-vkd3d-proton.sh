#!/bin/zsh
# The GRAPHICS_BACKEND=vkd3d lane and MetalSharp's MoltenVK, built in src/metalvulkan with its own scripts, then
# staged (stage-moltenvk.sh, stage-vkd3d-proton.sh):
#   1. sources/ (scripts/fetch-sources.sh), when they are not there yet; FRESH=1 fetches them again
#   2. MoltenVK (scripts/build-moltenvk.sh), with its SPIRV-Cross and SPIRV-Tools XCFrameworks when sources/MoltenVK
#      has none yet: fetch-sources.sh leaves External unbuilt. Xcode's derived data for MoltenVK is cleared with
#      them, as a stale one keeps failing with "no XCFramework found".
#   3. VKD3D-Proton, ARM64X (scripts/build-vkd3d-proton-arm64x.sh). The lane's DXGI and D3D11 to D3D3 are the DXVK
#      lane's DXVK (build-dxvk-macos.sh, stage-dxvk.sh), which ntdll loads for it: one DXVK in the process
#   4. the DXBC to DXIL converter MoltenVK loads for Metal Shader Converter (scripts/build-dxbcconverter.sh), from
#      DXC's checked-in generated sources: regenerating them runs Homebrew's clang-format, whose output differs from
#      the checked-in files, and DXC's build stops on that
set -e
source ${0:A:h}/env.sh
# src/metalvulkan's scripts set up their own compilers (xcodebuild takes CC as a tool path, and env.sh's has flags)
unset CC CXX OBJC CFLAGS CXXFLAGS OBJCFLAGS CPPFLAGS LDFLAGS PKG_CONFIG PKG_CONFIG_LIBDIR CONFIG_SITE SDKROOT
V=$ROOT/src/metalvulkan
cd $V
[[ -f scripts/build-moltenvk.sh ]] || { echo "build-vkd3d-proton: src/metalvulkan is empty (zsh scripts/fetch-sources.sh)" >&2; exit 1; }

if [[ $FRESH == 1 || ! -d sources/MoltenVK || ! -d sources/vkd3d-proton ||
      ! -d sources/DirectXShaderCompiler ]]; then
  rm -rf sources
  bash scripts/fetch-sources.sh vkd3d-proton moltenvk nvapi dxc
fi

DD=${TMPDIR:-/tmp}/metalsharp-moltenvk-dd
if [[ ! -d sources/MoltenVK/External/build/Latest/SPIRVCross.xcframework ||
      ! -d sources/MoltenVK/External/build/Latest/SPIRVTools.xcframework ]]; then
  rm -rf ${DD:?} $DD-ext
  bash scripts/build-moltenvk.sh --externals
else
  bash scripts/build-moltenvk.sh
fi

bash scripts/build-vkd3d-proton-arm64x.sh

DXC_BUILD_DIR=$V/artifacts/build/dxbcconverter-universal
mkdir -p $DXC_BUILD_DIR
grep -q '^HLSL_DISABLE_SOURCE_GENERATION:BOOL=ON' $DXC_BUILD_DIR/CMakeCache.txt 2>/dev/null ||
  { rm -rf $DXC_BUILD_DIR; mkdir -p $DXC_BUILD_DIR; echo 'HLSL_DISABLE_SOURCE_GENERATION:BOOL=ON' > $DXC_BUILD_DIR/CMakeCache.txt; }
DXC_BUILD_DIR=$DXC_BUILD_DIR bash scripts/build-dxbcconverter.sh

cd $ROOT
zsh scripts/stage-moltenvk.sh
zsh scripts/stage-vkd3d-proton.sh
