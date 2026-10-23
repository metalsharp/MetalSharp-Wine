#!/bin/zsh
# Check out the source trees the build scripts use, into src/:
#   src/wine         Wine 11.20 + MetalSharp (branch source/wine of this repository)
#   src/FEX          FEX-2610 + MetalSharp (branch source/fex), with its submodules
#   src/winemetalgl  WineMetalGL, arm64 Wine module (branch source/winemetalgl)
#   src/mullion      Mullion (DXMT fork); its MetalSharp work goes upstream as pull requests
# The same changes are in patches/ as git patches against the upstream tags.
set -e
source ${0:A:h}/env.sh
REPO=${METALSHARP_WINE_REPO:-https://github.com/metalsharp/MetalSharp-Wine.git}
MULLION=${MULLION_REPO:-https://github.com/aaf2tbz/mullion.git} MULLION_BRANCH=${MULLION_BRANCH:-i386-build}
mkdir -p $ROOT/src
clone() {  # clone <dir> <url> <branch>
  [[ -d $ROOT/src/$1/.git || -f $ROOT/src/$1/.git ]] && { echo "src/$1: already present"; return; }
  git clone --branch $3 --single-branch $2 $ROOT/src/$1
}
clone wine $REPO source/wine
clone FEX $REPO source/fex
git -C $ROOT/src/FEX submodule update --init --recursive
clone winemetalgl $REPO source/winemetalgl
clone mullion $MULLION $MULLION_BRANCH
echo "sources ready in $ROOT/src"
