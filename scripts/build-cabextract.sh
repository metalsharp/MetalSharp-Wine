#!/bin/zsh
# cabextract 1.11 (GPL-3.0-or-later) as a native arm64 tool, minos 15.0, for unpacking Microsoft
# cabinet installers (the corefonts setups, VC++ payloads) the way winetricks does. Installed as
# deps/cabextract/cabextract; package.sh copies it to bin/. The checksum matches Homebrew's formula.
set -e
source ${0:A:h}/env.sh
DL=$ROOT/downloads/cabextract; W=$ROOT/deps/build/cabextract; O=$ROOT/deps/cabextract
V=1.11
mkdir -p $DL $W
[[ -f $DL/cabextract-$V.tar.gz ]] || curl -fsSL -o $DL/cabextract-$V.tar.gz https://www.cabextract.org.uk/cabextract-$V.tar.gz
[[ $(shasum -a 256 $DL/cabextract-$V.tar.gz | cut -d' ' -f1) == b5546db1155e4c718ff3d4b278573604f30dd64c3c5bfd4657cd089b823a3ac6 ]] ||
  { echo "checksum mismatch: cabextract-$V.tar.gz" >&2; exit 1; }
cd $W; rm -rf cabextract-$V; tar -xzf $DL/cabextract-$V.tar.gz
(cd cabextract-$V && ./configure --disable-dependency-tracking && make -j$JOBS) > $W/build.log 2>&1
rm -rf $O; mkdir -p $O
cp cabextract-$V/cabextract $O/; strip $O/cabextract; codesign -f -s - $O/cabextract 2>/dev/null
cp cabextract-$V/COPYING $O/LICENSE.cabextract
echo "cabextract $V" > $O/VERSION
otool -L $O/cabextract | tail -n +2; $O/cabextract --version
