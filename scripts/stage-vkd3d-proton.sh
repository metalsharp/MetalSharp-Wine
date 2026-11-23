#!/bin/zsh
# VKD3D-Proton as GRAPHICS_BACKEND=vkd3d for x86_64 and native ARM64 programs: D3D12 on Vulkan (MoltenVK,
# stage-moltenvk.sh), from src/metalvulkan (the ARM64X build, scripts/build-vkd3d-proton-arm64x.sh: ARM64EC code for
# x86_64 programs under FEX, ARM64 code for native ones). Installed into install/lib/vkd3d-proton/x86_64-windows with
# the Wine builtin signature, which the loader requires on its builtin path.
# Only D3D12 is here. The lane's DXGI, which presents VKD3D-Proton's swap chains, and D3D11 to D3D3 (D3D11On12 too)
# are the DXVK lane's DXVK (MetalVulkan's dxvk/) (stage-dxvk.sh: ARM64X, and i386 for 32-bit programs), which ntdll loads for the
# VKD3D lane where this folder has no DLL of that name: one DXVK in the process, from D3D12 to D3D3.
#   stage-vkd3d-proton.sh [vkd3d-proton build dir] [NGX dir]
set -e
source ${0:A:h}/env.sh
WS=$ROOT/src/metalvulkan
VKD3D=${1:-$WS/artifacts/build/vkd3d-proton/aarch64-windows}
for f in $VKD3D/d3d12.dll $VKD3D/d3d12core.dll; do
  [[ -f $f ]] || { echo "missing: $f (build it first)" >&2; exit 1; }
done
O=$ROOT/install/lib/vkd3d-proton
rm -rf $O; mkdir -p $O/x86_64-windows
cp $VKD3D/d3d12.dll $VKD3D/d3d12core.dll $O/x86_64-windows/
# the lane's own NGX core and NVAPI (DLSS on MetalFX), which Wine loads for this lane in place of Mullion's builtins
# of those names: the given folder, else src/metalvulkan's own build (scripts/build-ngx-arm64x.sh) when its nvngx.dll
# is signed, else MetalVulkan's release (fetch-metalvulkan-ngx.sh). NGX loads only a signed core, and without them
# the lane has no DLSS, so staging stops rather than leave them out.
NGX=$2
if [[ -z $NGX ]]; then
  NGX=$WS/artifacts/build/ngx/aarch64-windows
  [[ -f $NGX/nvngx.dll && -f $NGX/nvapi64.dll ]] && command -v osslsigncode >/dev/null &&
    osslsigncode verify -CAfile $WS/ngx/signing/mullion-ngx.pem -in $NGX/nvngx.dll >/dev/null 2>&1 ||
    NGX=$(zsh ${0:A:h}/fetch-metalvulkan-ngx.sh) ||
    { echo "stage-vkd3d-proton: no signed NGX core for the lane (DLSS): see scripts/fetch-metalvulkan-ngx.sh" >&2; exit 1; }
fi
for f in nvngx.dll nvapi64.dll; do
  [[ -f $NGX/$f ]] || { echo "missing: $NGX/$f" >&2; exit 1; }
done
cp $NGX/nvngx.dll $NGX/nvapi64.dll $O/x86_64-windows/
# Wine's builtin marker: MetalVulkan's NGX DLLs carry it already, inside nvngx.dll's signed bytes.
for f in $O/x86_64-windows/d3d12.dll $O/x86_64-windows/d3d12core.dll; do
  python3 - $f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
# VKD3D-Proton is LGPL 2.1, the license text Wine ships as COPYING.LIB.
cp $ROOT/src/wine/COPYING.LIB $O/LICENSE
{
  echo "VKD3D-Proton $(git -C $WS/sources/vkd3d-proton describe --always --tags 2>/dev/null) (ARM64X, src/metalvulkan)"
  echo "DXGI and D3D11 to D3D3: the DXVK lane's DXVK (MetalVulkan's dxvk/) (lib/dxvk)"
  echo "NGX core and NVAPI (ARM64X, ngx/, signed): ${NGX:h:t}/${NGX:t}"
} > $O/VERSION
echo "vkd3d-proton:"; cat $O/VERSION; ls $O/x86_64-windows
