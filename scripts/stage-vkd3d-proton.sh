#!/bin/zsh
# VKD3D-Proton as GRAPHICS_BACKEND=vkd3d for x86_64 programs: D3D12 on Vulkan (MoltenVK, stage-moltenvk.sh),
# from src/vkd3d-proton (the ARM64X build, scripts/build-vkd3d-proton-arm64x.sh: ARM64EC code for x86_64
# programs under FEX), with DXVK 2.7.1's DXGI and D3D10/11 (scripts/build-dxvk.sh), which present its swap
# chains and make D3D11 devices on D3D12 ones (D3D11On12CreateDevice). The loader takes those from here and
# D3D9 from DXVK 1.10.3 (stage-dxvk.sh). Installed into install/lib/vkd3d-proton/x86_64-windows with the
# Wine builtin signature, which the loader requires on its builtin path.
#   stage-vkd3d-proton.sh [vkd3d-proton build dir] [dxvk build dir]
set -e
source ${0:A:h}/env.sh
WS=$ROOT/src/vkd3d-proton
VKD3D=${1:-$WS/artifacts/build/vkd3d-proton/aarch64-windows}
DXVK=${2:-$WS/artifacts/build/dxvk/x86_64-windows}
for f in $VKD3D/d3d12.dll $VKD3D/d3d12core.dll $DXVK/dxgi.dll $DXVK/d3d11.dll $DXVK/d3d10core.dll; do
  [[ -f $f ]] || { echo "missing: $f (build it first)" >&2; exit 1; }
done
O=$ROOT/install/lib/vkd3d-proton
rm -rf $O; mkdir -p $O/x86_64-windows
cp $VKD3D/d3d12.dll $VKD3D/d3d12core.dll $DXVK/dxgi.dll $DXVK/d3d11.dll $DXVK/d3d10core.dll $O/x86_64-windows/
for f in $O/x86_64-windows/*.dll; do
  python3 - $f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
# VKD3D-Proton and DXVK are LGPL 2.1, the license text Wine ships as COPYING.LIB.
cp $ROOT/src/wine/COPYING.LIB $O/LICENSE
{
  echo "VKD3D-Proton $(git -C $WS/sources/vkd3d-proton describe --always --tags 2>/dev/null) (ARM64X, src/vkd3d-proton)"
  echo "DXVK $(git -C $WS/sources/dxvk describe --always --tags 2>/dev/null) dxgi/d3d10core/d3d11 (x86_64)"
} > $O/VERSION
echo "vkd3d-proton:"; cat $O/VERSION; ls $O/x86_64-windows
