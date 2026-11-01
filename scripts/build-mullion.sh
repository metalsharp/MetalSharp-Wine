#!/bin/zsh
# Mullion (DXMT fork, https://github.com/dttdrv/mullion): Direct3D 10/11/12 on Metal.
# Builds ARM64X PE modules (native ARM64 and x86_64-through-FEX processes), i386 PE modules
# (D3D10/11 only; Mullion's D3D12 is 64-bit only) and the arm64 unix side, then installs:
#   install/lib/mullion/{aarch64-windows,i386-windows,aarch64-unix}   the runtime
#   install/lib/wine/{aarch64,i386}-windows/winemetal.dll, aarch64-unix/winemetal.so
#   install/lib/wine/aarch64-windows/nvngx.dll     NGX core for DLSS on MetalFX, signed (see below)
#   install/lib/wine/aarch64-windows/nvapi64.dll   NVAPI, for DLSS games (Streamline loads it from System32)
# nvngx.dll is signed with Mullion's NGX key when MULLION_NGX_SIGNING_KEY_FILE names it (a PEM private key; the
# certificate is src/mullion/src/nvngx/signing/mullion-ngx.pem): NVIDIA's NGX library loads only a signed core, and
# Mullion's DXGI trusts the certificate with NVEXT on. As Wine builtins both reach every prefix's System32, where
# NGX and Streamline look; nvapi64.dll loads only in a program Mullion shows an NVIDIA GPU (a DLSS game, which
# MetalSharp Wine sets up), so Steam and other programs find no NVAPI, as on another vendor's GPU.
# winemetal is a new module name for Wine, so wineboot creates its placeholder in every prefix;
# d3d11/dxgi/... are routed to the runtime only for processes with GRAPHICS_BACKEND=dxmt.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compilers
M=$ROOT/src/mullion
I=$ROOT/install
[[ -d $ROOT/deps/llvm15/lib ]] || zsh $ROOT/scripts/build-llvm15.sh
mkdir -p $M/toolchains && ln -sfn $ROOT/deps/llvm15 $M/toolchains/llvm

build() {  # build <dir> <cross-file> [meson options]
  local dir=$1 cross=$2; shift 2
  if [[ -f $M/$dir/build.ninja ]]; then
    (( $# )) && meson configure $M/$dir "$@" >/dev/null
  else
    meson setup $M/$dir $M --cross-file $M/$cross --buildtype release \
      -Dwine_build_path=$ROOT/src/wine -Dnative_llvm_path=$ROOT/deps/llvm15 "$@"
  fi
  ninja -C $M/$dir -j$JOBS
}
build build-arm64ec build-arm64ec.txt -Denable_nvngx=true -Denable_nvapi=true
build build-win32 build-win32.txt -Denable_d3d12=false

R=$I/lib/mullion
rm -rf $R; mkdir -p $R/aarch64-windows $R/i386-windows $R/aarch64-unix
B=$M/build-arm64ec/src
cp $B/d3d10/d3d10core.dll $B/d3d11/d3d11.dll $B/d3d12/d3d12.dll $B/d3d12/d3d12core.dll \
   $B/dxgi/dxgi.dll $B/winemetal/winemetal.dll $R/aarch64-windows/
cp $B/winemetal/unix/winemetal.so $R/aarch64-unix/
cp $B/nvapi/nvapi64.dll $R/aarch64-windows/
if [[ -n $MULLION_NGX_SIGNING_KEY_FILE ]]; then
  osslsigncode sign -certs $M/src/nvngx/signing/mullion-ngx.pem -key $MULLION_NGX_SIGNING_KEY_FILE -n "Mullion NGX" \
    -h sha256 -in $B/nvngx/nvngx.dll -out $R/aarch64-windows/nvngx.dll >/dev/null
  osslsigncode verify -CAfile $M/src/nvngx/signing/mullion-ngx.pem -in $R/aarch64-windows/nvngx.dll >/dev/null
else
  echo "warning: MULLION_NGX_SIGNING_KEY_FILE is not set: nvngx.dll is unsigned, and NGX (DLSS) will not load it" >&2
  cp $B/nvngx/nvngx.dll $R/aarch64-windows/
fi
B=$M/build-win32/src
cp $B/d3d10/d3d10core.dll $B/d3d11/d3d11.dll $B/dxgi/dxgi.dll $B/winemetal/winemetal.dll $R/i386-windows/
cp $M/LICENSE $R/LICENSE
echo "Mullion $(git -C $M rev-parse --short HEAD)" > $R/VERSION

cp $R/aarch64-windows/winemetal.dll $R/aarch64-windows/nvngx.dll $I/lib/wine/aarch64-windows/
mv $R/aarch64-windows/nvapi64.dll $I/lib/wine/aarch64-windows/
cp $R/i386-windows/winemetal.dll $I/lib/wine/i386-windows/
rm -f $I/lib/wine/aarch64-unix/winemetal.so; cp $R/aarch64-unix/winemetal.so $I/lib/wine/aarch64-unix/
echo "mullion: $R ($(cat $R/VERSION))"
