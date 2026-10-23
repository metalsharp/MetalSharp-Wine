#!/bin/zsh
# The rest of the MetalSharp app's payloads, in install/share/metalsharp:
#   runtime/host          MetalSharp host runtime (libmetalsharp_host_runtime.dylib, arm64; ABI 1.0)
#   redist/vcrun2022      Microsoft's VC++ 2015-2022 redistributable installers (14.44.35211.0)
#   redist/corefonts      Microsoft's core fonts for the web installers
# The Microsoft files are the original, unmodified installers, which their licenses let us
# redistribute; the app installs or extracts them into prefixes. Wine itself doesn't use them.
# Checksums: the VC++ ones are the SHA-256 in Microsoft's own versioned download URLs; the
# corefonts ones match winetricks 20250102.
set -e
source ${0:A:h}/env.sh
X=${1:-$ROOT/downloads/metalsharp-bundles/extract}
DL=$ROOT/downloads/redist; O=$ROOT/install/share/metalsharp
mkdir -p $DL
fetch() {  # fetch <url> <file> <sha256>
  [[ -f $DL/$2 ]] || curl -fsSL -o $DL/$2 $1
  [[ $(shasum -a 256 $DL/$2 | cut -d' ' -f1) == $3 ]] || { echo "checksum mismatch: $2" >&2; exit 1; }
}
VC=https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9
fetch $VC/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe vc_redist.x64.exe \
  cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
fetch $VC/0C09F2611660441084CE0DF425C51C11E147E6447963C3690F97E0B25C55ED64/VC_redist.x86.exe vc_redist.x86.exe \
  0c09f2611660441084ce0df425c51c11e147e6447963c3690f97e0b25c55ed64
typeset -A FONTS
FONTS=(andale32 0524fe42951adc3a7eb870e32f0920313c71f170c859b5f770d82b4ee111e970
       arial32  85297a4d146e9c87ac6f74822734bdee5f4b2a722d7eaa584b7f2cbf76f478f6
       arialb32 a425f0ffb6a1a5ede5b979ed6177f4f4f4fdef6ae7c302a7b7720ef332fec0a8
       comic32  9c6df3feefde26d4e41d4a4fe5db2a89f9123a772594d7f59afd062625cd204e
       courie32 bb511d861655dde879ae552eb86b134d6fae67cb58502e6ff73ec5d9151f3384
       georgi32 2c2c7dcda6606ea5cf08918fb7cd3f3359e9e84338dc690013f20cd42e930301
       impact32 6061ef3b7401d9642f5dfdb5f2b376aa14663f6275e60a51207ad4facf2fccfb
       times32  db56595ec6ef5d3de5c24994f001f03b2a13e37cee27bc25c58f6f43e8f807ab
       trebuc32 5a690d9bb8510be1b8b4fe49f1f2319651fe51bbe54775ddddd8ef0bd07fdac9
       verdan32 c1cb61255e363166794e47664e2f21af8e3a26cb6346eb8d2ae2fa85dd5aad96
       webdin32 64595b5abc1080fba8610c5c34fab5863408e806aafe84653ca8575bed17d75a)
for f in ${(k)FONTS}; do fetch https://github.com/pushcx/corefonts/raw/master/$f.exe $f.exe $FONTS[$f]; done

rm -rf $O/runtime/host $O/redist; mkdir -p $O/runtime $O/redist/vcrun2022 $O/redist/corefonts
rsync -a $X/metalsharp-runtime/runtime/host $O/runtime/
cp $DL/vc_redist.x64.exe $DL/vc_redist.x86.exe $O/redist/vcrun2022/
echo "Microsoft Visual C++ 2015-2022 Redistributable 14.44.35211.0" > $O/redist/vcrun2022/VERSION
for f in ${(k)FONTS}; do cp $DL/$f.exe $O/redist/corefonts/; done
echo "Microsoft core fonts for the web (EULA inside each installer)" > $O/redist/corefonts/VERSION
echo "metalsharp redist: $(du -sh $O/redist | cut -f1); host runtime $(lipo -archs $O/runtime/host/libmetalsharp_host_runtime.dylib)"
