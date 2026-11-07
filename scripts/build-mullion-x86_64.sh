#!/bin/zsh
# Mullion for the x86_64 MetalSharp Wine (Wine 11.17 under Rosetta, MetalSharp's regular build): x86_64 and i386 PE
# modules and the x86_64 unix side, built against that Wine's build tree, laid out as the regular build's DXMT lane
# (runtime/wine/lib/dxmt):
#   install-x86_64/lib/dxmt/{x86_64-windows,i386-windows,x86_64-unix}
#   x86_64-windows also has nvngx.dll (NGX core for DLSS on MetalFX) and nvapi64.dll, and d3d12.dll/d3d12core.dll.
# Mullion's DXGI trusts Mullion's NGX certificate (src/nvngx/signing/mullion-ngx.pem), as on the arm64 build. NGX checks
# the signature of System32's nvngx.dll, which the x86_64 MetalSharp Wine takes from VKD3D-Proton-MacOS's release
# (signed by its CI), and then Wine loads this lane's core by name; MULLION_NGX_SIGNING_KEY_FILE signs this one too.
#   X86_WINE_BUILD=<Wine 11.17 x86_64 build tree> build-mullion-x86_64.sh
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compilers
M=$ROOT/src/mullion
W=${X86_WINE_BUILD:?set X86_WINE_BUILD to the x86_64 Wine build tree (see build-wine-x86_64.sh)}
LLVM=$ROOT/deps/llvm15-x86_64
OUT=$ROOT/install-x86_64/lib/dxmt
# The Metal shader compiler (Mullion's own .metal kernels) comes with Xcode, not the command line tools.
xcrun -sdk macosx -f metal >/dev/null 2>&1 || export DEVELOPER_DIR=${DEVELOPER_DIR:-$HOME/Downloads/Xcode-beta.app/Contents/Developer}
[[ -f $W/dlls/winemac.drv/winemac.so ]] || { echo "no x86_64 Wine build tree at $W" >&2; exit 1; }
[[ -f $LLVM/lib/libLLVMCore.a ]] || { echo "no x86_64 LLVM 15 at $LLVM" >&2; exit 1; }

build() {  # build <dir> <cross-file> [meson options]
  local dir=$1 cross=$2; shift 2
  if [[ -f $M/$dir/build.ninja ]]; then
    (( $# )) && meson configure $M/$dir "$@" >/dev/null
  else
    meson setup $M/$dir $M --cross-file $M/$cross --buildtype release --strip \
      -Dwine_build_path=$W -Dnative_llvm_path=$LLVM "$@"
  fi
  ninja -C $M/$dir -j$JOBS
}
build build-x86mac-win64 build-win64.txt -Denable_nvngx=true -Denable_nvapi=true -Denable_d3d12=true
build build-x86mac-win32 build-win32.txt -Denable_d3d12=false

rm -rf $OUT; mkdir -p $OUT/x86_64-windows $OUT/i386-windows $OUT/x86_64-unix
B=$M/build-x86mac-win64/src
cp $B/d3d10/d3d10core.dll $B/d3d11/d3d11.dll $B/d3d12/d3d12.dll $B/d3d12/d3d12core.dll $B/dxgi/dxgi.dll \
   $B/winemetal/winemetal.dll $B/nvapi/nvapi64.dll $OUT/x86_64-windows/
cp $B/winemetal/unix/winemetal.so $OUT/x86_64-unix/
if [[ -n $MULLION_NGX_SIGNING_KEY_FILE ]]; then
  osslsigncode sign -certs $M/src/nvngx/signing/mullion-ngx.pem -key $MULLION_NGX_SIGNING_KEY_FILE -n "Mullion NGX" \
    -h sha256 -in $B/nvngx/nvngx.dll -out $OUT/x86_64-windows/nvngx.dll >/dev/null
  osslsigncode verify -CAfile $M/src/nvngx/signing/mullion-ngx.pem -in $OUT/x86_64-windows/nvngx.dll >/dev/null
else
  cp $B/nvngx/nvngx.dll $OUT/x86_64-windows/
fi
B=$M/build-x86mac-win32/src
cp $B/d3d10/d3d10core.dll $B/d3d11/d3d11.dll $B/dxgi/dxgi.dll $B/winemetal/winemetal.dll $OUT/i386-windows/
cp $M/LICENSE $OUT/LICENSE
echo "Mullion $(git -C $M rev-parse --short HEAD) (x86_64)" > $OUT/VERSION
file $OUT/*/*
echo "mullion x86_64: $OUT ($(cat $OUT/VERSION))"
