#!/bin/zsh
# DXVK-MacOS 3.1 (src/dxvk-macos: metalsharp/DXVK-MacOS, DXVK 3.1.1 ported to MoltenVK) for GRAPHICS_BACKEND=dxvk:
# D3D7 to D3D11 and DXGI on MoltenVK (ddraw.dll, D3D7/6/5/3, is d7vk's). Built as ARM64X PE modules
# (build-arm64x.txt), which x86_64 programs load as ARM64EC under FEX and native arm64 programs load as arm64, and
# as i386 (build-win32.txt) for 32-bit programs, which FEX runs. stage-dxvk.sh installs them.
#   build-dxvk-macos.sh
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compilers
D=$ROOT/src/dxvk-macos
[[ -f $D/build-arm64x.txt ]] || { echo "src/dxvk-macos missing (fetch-sources.sh)" >&2; exit 1; }
export PATH=$MINGW/bin:$PATH
# Its MoltenVK patch to dxbc-spirv, as its release workflow applies it (once: a tree that has it is left alone).
P=$D/patches/dxbc-spirv-moltenvk.patch
if [[ -f $P ]] && git -C $D/subprojects/dxbc-spirv apply --check $P 2>/dev/null; then
  git -C $D/subprojects/dxbc-spirv apply $P
fi
[[ -f $D/build-arm64x/build.ninja ]] || meson setup $D/build-arm64x $D --cross-file $D/build-arm64x.txt \
    --buildtype release -Db_ndebug=if-release
ninja -C $D/build-arm64x -j$JOBS
# i386, with llvm-mingw's i686 compilers, which build-win32.txt names as i686-w64-mingw32-gcc/g++
[[ -f $D/build-i386/build.ninja ]] || meson setup $D/build-i386 $D --cross-file $D/build-win32.txt \
    --buildtype release -Db_ndebug=if-release
ninja -C $D/build-i386 -j$JOBS
ls -la $D/build-arm64x/src/{ddraw/ddraw,d3d8/d3d8,d3d9/d3d9,d3d10/d3d10core,d3d11/d3d11,dxgi/dxgi}.dll \
       $D/build-i386/src/{ddraw/ddraw,d3d8/d3d8,d3d9/d3d9}.dll
