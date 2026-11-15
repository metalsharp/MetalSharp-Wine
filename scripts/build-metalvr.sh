#!/bin/zsh
# MetalVR (src/metalvr: metalsharp/MetalVR): VR for Windows games on the Mullion lane. Built with its own scripts
# and installed as
#   install/bin/metalvrd                            the server: composes the game's frames, encodes them with
#                                                   VideoToolbox, and streams them to a headset through ALVR's
#                                                   server_core, which is linked into it (arm64)
#   install/lib/metalvr/x86_64-windows/
#     metalvr_openxr.dll, metalvr_openxr.json       the OpenXR runtime a game's loader finds through the prefix's
#                                                   ActiveRuntime (scripts/metalvr-prefix.sh), and its manifest
#     openvr_api.dll                                OpenComposite: OpenVR (SteamVR) on OpenXR, which replaces a
#                                                   game's own openvr_api.dll. GPL-3.0, its own DLL
# The runtime is x86_64 Windows code, which FEX runs: VR games are x86_64. It hands Direct3D 11 swapchain images to
# metalvrd as Mullion shared textures, so a game has to be on the Mullion lane.
#
# Needs a Rust toolchain (cargo) for ALVR, beside what the rest of the build needs.
#   build-metalvr.sh
set -e
source ${0:A:h}/env.sh
V=$ROOT/src/metalvr
R=$ROOT/install/lib/metalvr
[[ -f $V/CMakeLists.txt ]] || { echo >&2 "!! src/metalvr is not checked out: zsh scripts/fetch-sources.sh"; exit 1; }
# rustup installs cargo outside the build's PATH
[[ -x $HOME/.cargo/bin/cargo ]] && export PATH=$PATH:$HOME/.cargo/bin
command -v cargo >/dev/null || { echo >&2 "!! cargo is needed for ALVR's server_core: https://rustup.rs"; exit 1; }

# The Khronos OpenXR headers, then ALVR's server_core: both at the commits MetalVR pins
sh $V/scripts/fetch-deps.sh
bash $V/scripts/build-alvr.sh

# metalvrd: arm64, held to the release's macOS floor by env.sh's flags and deployment target
cmake -S $V -B $V/build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET -DMETALVR_WITH_ALVR=ON
cmake --build $V/build/native -j$JOBS

# The Windows side, with llvm-mingw: env.sh puts its x86_64-w64-mingw32 tools first in PATH, which is what
# MetalVR's toolchain file and OpenComposite's build name
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compiler
cmake -S $V -B $V/build/win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$V/cmake/mingw-x86_64.cmake
cmake --build $V/build/win64 -j$JOBS
bash $V/scripts/build-opencomposite.sh

for f in $V/build/native/daemon/metalvrd $V/build/win64/runtime/metalvr_openxr.dll \
         $V/build/win64/runtime/metalvr_openxr.json $V/build/opencomposite/openvr_api.dll; do
  [[ -f $f ]] || { echo >&2 "!! not built: $f"; exit 1; }
done
# metalvrd without ALVR has only the simulated headset
$V/build/native/daemon/metalvrd --help 2>&1 | grep -q -- '--trust' ||
  { echo >&2 "!! metalvrd was built without the ALVR driver"; exit 1; }

rm -rf $R; mkdir -p $R/x86_64-windows $ROOT/install/bin
cp $V/build/native/daemon/metalvrd $ROOT/install/bin/metalvrd
cp $V/build/win64/runtime/metalvr_openxr.dll $V/build/win64/runtime/metalvr_openxr.json $R/x86_64-windows/
cp $V/build/opencomposite/openvr_api.dll $R/x86_64-windows/
# What is redistributed, with its license: MetalVR (MIT), ALVR's server_core inside metalvrd (MIT), and
# OpenComposite (GPL-3.0) with the Khronos loader and OpenVR headers compiled into it
cp $V/LICENSE $R/LICENSE
cp $V/external/ALVR/LICENSE $R/LICENSE.ALVR
O=$V/external/OpenComposite
mkdir -p $R/OpenComposite
cp $O/LICENSE.txt $O/LICENSE_*.txt $O/LICENCE_*.txt $R/OpenComposite/
cp $O/libs/openxr-sdk/LICENSE $R/OpenComposite/LICENSE_OPENXR_SDK.txt
echo "MetalVR $(git -C $V rev-parse --short HEAD) (metalsharp/MetalVR), ALVR $(git -C $V/external/ALVR describe --tags --always), OpenComposite $(git -C $O rev-parse --short HEAD)" > $R/VERSION
echo "metalvr: $R ($(cat $R/VERSION))"
