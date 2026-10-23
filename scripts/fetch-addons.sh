#!/bin/zsh
# Wine Mono and Wine Gecko at the versions this Wine expects (dlls/mscoree, dlls/appwiz.cpl),
# installed into install/share/wine/{mono,gecko} so prefixes use them without a download.
#   Mono 11.3.0 arm64 package: arm64, x86 and x86_64 runtimes (every process kind here).
#   Gecko 2.47.4 x86 and x86_64: there is no ARM64 Gecko, so native ARM64 programs get no
#   MSHTML engine; x86_64 (ARM64EC) and i386 (WoW64) programs do.
# The tarballs are checked against the MSIs' SHA-256 values that Wine itself pins.
set -e
source ${0:A:h}/env.sh
D=$ROOT/downloads/addons
mkdir -p $D
fetch() {  # fetch <url> <file>
  [[ -f $D/$2 ]] || curl -fsSL -o $D/$2 $1/$2
}
check() {  # check <file> <sha256>
  [[ $(shasum -a 256 $D/$1 | cut -d' ' -f1) == $2 ]] || { echo "checksum mismatch: $1" >&2; exit 1; }
}
MONO=11.3.0 GECKO=2.47.4
fetch https://github.com/madewokherd/wine-mono/releases/download/wine-mono-$MONO wine-mono-$MONO-arm64.tar.xz
fetch https://github.com/madewokherd/wine-mono/releases/download/wine-mono-$MONO wine-mono-$MONO-arm64.msi
check wine-mono-$MONO-arm64.msi b0c47d374efaf22cbff76cb10488ac2d1627b067e085aef7acec573884120e0f
for a in x86 x86_64; do
  fetch https://dl.winehq.org/wine/wine-gecko/$GECKO wine-gecko-$GECKO-$a.tar.xz
  fetch https://dl.winehq.org/wine/wine-gecko/$GECKO wine-gecko-$GECKO-$a.msi
done
check wine-gecko-$GECKO-x86.msi 26cecc47706b091908f7f814bddb074c61beb8063318e9efc5a7f789857793d6
check wine-gecko-$GECKO-x86_64.msi e590b7d988a32d6aa4cf1d8aa3aa3d33766fdd4cf4c89c2dcc2095ecb28d066f

S=$ROOT/install/share/wine
rm -rf $S/mono $S/gecko; mkdir -p $S/mono $S/gecko
tar -xf $D/wine-mono-$MONO-arm64.tar.xz -C $S/mono
for a in x86 x86_64; do tar -xf $D/wine-gecko-$GECKO-$a.tar.xz -C $S/gecko; done
ls $S/mono $S/gecko
