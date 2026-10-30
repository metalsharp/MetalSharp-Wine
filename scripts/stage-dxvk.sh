#!/bin/zsh
# DXVK-MacOS 3.1 (src/dxvk-macos, built as ARM64X by build-dxvk-macos.sh) as the opt-in GRAPHICS_BACKEND=dxvk
# for x86_64 programs (D3D9/10/11 on MoltenVK, as ARM64EC under FEX). Installed into
# install/lib/dxvk/x86_64-windows, where the loader looks for the lane's DLLs, with the Wine builtin signature,
# which the loader requires on its builtin path.
#   stage-dxvk.sh [build dir]
set -e
source ${0:A:h}/env.sh
D=$ROOT/src/dxvk-macos
B=${1:-$D/build-arm64x}
O=$ROOT/install/lib/dxvk
for f in d3d9/d3d9 d3d10/d3d10core d3d11/d3d11 dxgi/dxgi; do
  [[ -f $B/src/$f.dll ]] || { echo "missing: $B/src/$f.dll (run build-dxvk-macos.sh)" >&2; exit 1; }
done
rm -rf $O; mkdir -p $O/x86_64-windows
for f in d3d9/d3d9 d3d10/d3d10core d3d11/d3d11 dxgi/dxgi; do
  cp $B/src/$f.dll $O/x86_64-windows/
  python3 - $O/x86_64-windows/${f:t}.dll <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
cp $D/LICENSE $O/LICENSE
cp $D/subprojects/dxbc-spirv/LICENSE $O/LICENSE.dxbc-spirv
echo "DXVK-MacOS $(git -C $D describe --always --tags 2>/dev/null) (metalsharp/DXVK-MacOS, ARM64X)" > $O/VERSION
echo "dxvk: $(cat $O/VERSION)"
