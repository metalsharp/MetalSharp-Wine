#!/bin/zsh
# DXVK 1.10.3 for macOS (DXVK-macOS 1.10.3-async, x86_64 only) from the metalsharp-graphics-dll
# bundle, as the opt-in GRAPHICS_BACKEND=dxvk for x86_64 programs (D3D9/10/11 on MoltenVK).
# Installed into install/lib/dxvk/x86_64-windows with the Wine builtin signature, which the
# loader requires on its builtin path. Its dxvk.conf (a DXVK 3.x option) is not used.
#   stage-dxvk.sh [path/to/Graphics/dll/dxvk]
set -e
source ${0:A:h}/env.sh
SRC=${1:-$ROOT/downloads/metalsharp-bundles/extract/metalsharp-graphics-dll/Graphics/dll/dxvk}
(cd $SRC && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "checksum mismatch in $SRC" >&2; exit 1; }
typeset -A SHA
SHA=(d3d9.dll ab6d25f0a6f9a7375483710a714a3aa0b81b08ec645764ad47340d9d56b7daeb
     d3d10core.dll f85c6298bfbbba66ad7e2728e420807909cc443a766bfd1496f7bae1b6bc1f62
     d3d11.dll a88c7ded56f8f280f17fc6cbde8f61829935fb89f6998a3fd31687aef6f11501
     dxgi.dll e37f43183a1bc7174fc898c6e23729b0b60aede0d641c51b752228129ec4cb21)
O=$ROOT/install/lib/dxvk
rm -rf $O; mkdir -p $O/x86_64-windows
for f in ${(k)SHA}; do
  [[ $(shasum -a 256 $SRC/x86_64-windows/$f | cut -d' ' -f1) == $SHA[$f] ]] || { echo "not the MetalSharp DXVK 1.10.3: $f" >&2; exit 1; }
  cp $SRC/x86_64-windows/$f $O/x86_64-windows/
  python3 - $O/x86_64-windows/$f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
cp $SRC/LICENSE $O/LICENSE
echo "DXVK $(strings $SRC/x86_64-windows/d3d11.dll | grep -m1 -o 'v1\.10\.3[^ ]*') (x86_64)" > $O/VERSION
echo "dxvk: $(cat $O/VERSION)"
