#!/bin/zsh
# Mullion (DXMT fork, https://github.com/dttdrv/mullion): Direct3D 10/11/12 on Metal.
# Builds ARM64X PE modules (native ARM64 and x86_64-through-FEX processes), i386 PE modules
# (D3D10/11 only; Mullion's D3D12 is 64-bit only) and the arm64 unix side, then installs:
#   install/lib/mullion/{aarch64-windows,i386-windows,aarch64-unix}   the runtime
#   install/lib/wine/{aarch64,i386}-windows/winemetal.dll, aarch64-unix/winemetal.so
#   install/lib/wine/aarch64-windows/nvngx.dll     NGX core for DLSS on MetalFX, signed (see below)
#   install/lib/wine/aarch64-windows/nvapi64.dll   NVAPI, for DLSS games (Streamline loads it from System32)
# NVIDIA's NGX library loads only a signed core: nvngx.dll must carry Mullion's signature, which Mullion's DXGI trusts
# with NVEXT on (the certificate is src/mullion/src/nvngx/signing/mullion-ngx.pem). Either
#   MULLION_NGX_SIGNING_KEY_FILE=<PEM private key>   signs the core built here, or
#   MULLION_SIGNED_NGX_DIR=<signed Mullion release>  takes that release's aarch64-windows/nvngx.dll (Mullion's CI
#                                                    signs it; use the release of src/mullion's commit), or the
#                                                    arm64ec-nvngx.dll of Mullion's sign-ngx workflow artifact.
# The core installed is then checked against the certificate. With neither, the build stops before building, since
# an unsigned core gives every game no DLSS; MULLION_NGX_UNSIGNED=1 builds one anyway, for a build without DLSS.
# As Wine builtins nvngx.dll and nvapi64.dll reach every prefix's System32, where
# NGX and Streamline look; nvapi64.dll loads only in a program Mullion shows an NVIDIA GPU (a DLSS game, which
# MetalSharp Wine sets up), so Steam and other programs find no NVAPI, as on another vendor's GPU.
# winemetal is a new module name for Wine, so wineboot creates its placeholder in every prefix;
# d3d11/dxgi/... are routed to the runtime only for processes with GRAPHICS_BACKEND=dxmt.
set -e
source ${0:A:h}/env.sh
NGX_DIR=${MULLION_SIGNED_NGX_DIR:+${MULLION_SIGNED_NGX_DIR:A}}
if [[ -n $MULLION_NGX_SIGNING_KEY_FILE ]]; then
  [[ -f $MULLION_NGX_SIGNING_KEY_FILE ]] || { echo "error: no MULLION_NGX_SIGNING_KEY_FILE" >&2; exit 1; }
elif [[ -n $NGX_DIR ]]; then
  # an unpacked release has aarch64-windows/nvngx.dll; Mullion's sign-ngx workflow's artifact has arm64ec-nvngx.dll
  NGX_DLL=$NGX_DIR/aarch64-windows/nvngx.dll
  [[ -f $NGX_DLL ]] || NGX_DLL=$NGX_DIR/arm64ec-nvngx.dll
  [[ -f $NGX_DLL ]] ||
    { echo "error: MULLION_SIGNED_NGX_DIR has no aarch64-windows/nvngx.dll or arm64ec-nvngx.dll" >&2; exit 1; }
elif [[ $MULLION_NGX_UNSIGNED != 1 ]]; then
  echo "error: nvngx.dll needs Mullion's signature, or no game gets DLSS: set MULLION_NGX_SIGNING_KEY_FILE or" >&2
  echo "       MULLION_SIGNED_NGX_DIR (see this script), or MULLION_NGX_UNSIGNED=1 to build without DLSS" >&2
  exit 1
fi
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
CERT=$M/src/nvngx/signing/mullion-ngx.pem
if [[ -n $MULLION_NGX_SIGNING_KEY_FILE ]]; then
  osslsigncode sign -certs $CERT -key $MULLION_NGX_SIGNING_KEY_FILE -n "Mullion NGX" \
    -h sha256 -in $B/nvngx/nvngx.dll -out $R/aarch64-windows/nvngx.dll >/dev/null
elif [[ -n $NGX_DIR ]]; then
  cp $NGX_DLL $R/aarch64-windows/nvngx.dll
else
  echo "warning: nvngx.dll is unsigned (MULLION_NGX_UNSIGNED=1): NGX will not load it, so no game gets DLSS" >&2
  cp $B/nvngx/nvngx.dll $R/aarch64-windows/
fi
if [[ -n $MULLION_NGX_SIGNING_KEY_FILE || -n $NGX_DIR ]] &&
   ! osslsigncode verify -CAfile $CERT -in $R/aarch64-windows/nvngx.dll >/dev/null 2>&1; then
  echo "error: $R/aarch64-windows/nvngx.dll is not signed with $CERT" >&2
  exit 1
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
