#!/bin/zsh
# Microsoft's shader compilers, which wine.inf copies into every prefix it creates or updates
# (ShaderCompilers.ntarm64 and .ntx86, from <datadir>/shadercompilers):
#   install/share/wine/shadercompilers/arm64   dxcompiler.dll, dxil.dll (DXC 1.9.2609), d3dcompiler_47.dll (SDK)
#   install/share/wine/shadercompilers/x86     dxcompiler.dll, dxil.dll (DXC 1.9.2609)
# Programs that compile HLSL at run time (and Mullion's tests) need them: Wine's own d3dcompiler_47
# (vkd3d-shader) miscompiles some shaders (a scalar eval_snapped writes two components), and Wine has
# no DXC. 32-bit programs keep Wine's d3dcompiler_47, there being no x86 one in the SDK's package.
# The files are Microsoft's redistributables, unmodified: DXC's release (LICENSE-MS.txt covers dxil.dll,
# LICENSE-LLVM.txt the rest) and the Windows SDK's Redist\D3D (the SDK license's distributable code).
set -e
source ${0:A:h}/env.sh
DL=$ROOT/downloads/shadercompilers; O=$ROOT/install/share/wine/shadercompilers; L=$ROOT/install/share/licenses
mkdir -p $DL
fetch() {  # fetch <url> <file> <sha256>
  [[ -f $DL/$2 ]] || curl -fsSL -o $DL/$2 $1
  [[ $(shasum -a 256 $DL/$2 | cut -d' ' -f1) == $3 ]] || { echo "checksum mismatch: $2" >&2; exit 1; }
}
fetch https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip \
  dxc_2026_09_29.zip ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
fetch https://www.nuget.org/api/v2/package/Microsoft.Windows.SDK.CPP/10.0.28000.2705 \
  microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807

X=$DL/extract; rm -rf $X $O; mkdir -p $X $O/arm64 $O/x86
# DXC's zip names its files with backslashes (bin\arm64\dxil.dll), which unzip does not split
python3 -I -c '
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    p = n.replace("\\", "/")
    if p.endswith(".dll") and p.split("/")[1] in ("arm64", "x86") or p.startswith("LICENSE-"):
        os.makedirs(os.path.join(sys.argv[2], os.path.dirname(p)), exist_ok=True)
        open(os.path.join(sys.argv[2], p), "wb").write(z.read(n))
' $DL/dxc_2026_09_29.zip $X/dxc
unzip -q -o $DL/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg 'c/Redist/D3D/arm64/d3dcompiler_47.dll' -d $X/sdk
cp $X/dxc/bin/arm64/dxcompiler.dll $X/dxc/bin/arm64/dxil.dll $X/sdk/c/Redist/D3D/arm64/d3dcompiler_47.dll $O/arm64/
cp $X/dxc/bin/x86/dxcompiler.dll $X/dxc/bin/x86/dxil.dll $O/x86/
mkdir -p $L/dxc $L/windows-sdk
cp $X/dxc/LICENSE-MS.txt $X/dxc/LICENSE-LLVM.txt $L/dxc/
echo "d3dcompiler_47.dll: Microsoft Windows SDK 10.0.28000.2705 (Microsoft.Windows.SDK.CPP, Redist\\D3D), \
distributable under the SDK license: https://aka.ms/WinSDKLicenseURL" > $L/windows-sdk/NOTICE
echo "DXC 1.9.2609 (dxc_2026_09_29), d3dcompiler_47 from Windows SDK 10.0.28000.2705" > $O/VERSION
echo "shader compilers: $O ($(du -sh $O | cut -f1))"
