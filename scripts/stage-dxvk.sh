#!/bin/zsh
# DXVK 3.1 (src/metalvulkan/dxvk, built by build-dxvk-macos.sh) as the opt-in GRAPHICS_BACKEND=dxvk, on MoltenVK:
# the ARM64X build in install/lib/dxvk/x86_64-windows for x86_64 programs (D3D7 to D3D11, as ARM64EC under FEX),
# the i386 build in install/lib/dxvk/i386-windows for 32-bit ones (D3D7 to D3D9). ddraw.dll is d7vk's (D3D7, 6, 5
# and 3); the loader gives it Wine's own ddraw.dll as ddraw_.dll, for DirectDraw itself. Each DLL gets the Wine
# builtin signature, which the loader requires on its builtin path.
#   stage-dxvk.sh [ARM64X build dir] [NGX dir] [i386 build dir]
set -e
source ${0:A:h}/env.sh
D=$ROOT/src/metalvulkan/dxvk
B=${1:-$D/build-arm64x}
B32=${3:-$D/build-i386}
O=$ROOT/install/lib/dxvk
X64=(ddraw/ddraw d3d8/d3d8 d3d9/d3d9 d3d10/d3d10core d3d11/d3d11 dxgi/dxgi)
X86=(ddraw/ddraw d3d8/d3d8 d3d9/d3d9)
for f in $X64; do
  [[ -f $B/src/$f.dll ]] || { echo "missing: $B/src/$f.dll (run build-dxvk-macos.sh)" >&2; exit 1; }
done
for f in $X86; do
  [[ -f $B32/src/$f.dll ]] || { echo "missing: $B32/src/$f.dll (run build-dxvk-macos.sh)" >&2; exit 1; }
done
stamp() {
  python3 - $1 <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
}
rm -rf $O; mkdir -p $O/x86_64-windows $O/i386-windows
for f in $X64; do
  cp $B/src/$f.dll $O/x86_64-windows/
  stamp $O/x86_64-windows/${f:t}.dll
done
for f in $X86; do
  cp $B32/src/$f.dll $O/i386-windows/
  stamp $O/i386-windows/${f:t}.dll
done
# the NGX core and NVAPI for DLSS on MetalFX (MetalVulkan's ngx/, scripts/build-ngx-arm64x.sh), which Wine
# loads for this lane in place of Mullion's builtins of those names
NGX=${2:-$ROOT/src/metalvulkan/artifacts/build/ngx/aarch64-windows}
if [[ -f $NGX/nvngx.dll && -f $NGX/nvapi64.dll ]]; then
  for f in nvngx nvapi64; do
    cp $NGX/$f.dll $O/x86_64-windows/
    stamp $O/x86_64-windows/$f.dll
  done
fi
# Wine's own ddraw, as ddraw_.dll, for d7vk to pass DirectDraw on to. wineboot installs it into the prefix like any
# builtin, which the loader needs (it only loads builtins that have a file there); the loader gives that file Wine's
# ddraw, not the lane's (is_d7vk_proxy_file()).
for a in i386-windows aarch64-windows; do
  W=$ROOT/install/lib/wine/$a
  [[ -f $W/ddraw.dll ]] || { echo "missing: $W/ddraw.dll (run build-wine.sh install)" >&2; exit 1; }
  cp $W/ddraw.dll $W/ddraw_.dll
done
cp $D/LICENSE $O/LICENSE
cp $D/subprojects/dxbc-spirv/LICENSE $O/LICENSE.dxbc-spirv
echo "DXVK $(cat $D/RELEASE) (metalsharp/MetalVulkan $(git -C $D describe --always --tags --first-parent 2>/dev/null), ARM64X and i386)" > $O/VERSION
echo "dxvk: $(cat $O/VERSION)"
