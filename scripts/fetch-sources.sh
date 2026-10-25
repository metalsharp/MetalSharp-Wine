#!/bin/zsh
# Check out the source trees the build scripts use, into src/. In a clone of this repository they
# are submodules pinned to the commits a release was built from:
#   src/wine         Wine 11.20 + MetalSharp (branch source/wine of this repository)
#   src/FEX          FEX-2610 + MetalSharp (branch source/fex), with its own submodules
#   src/winemetalgl  WineMetalGL, arm64 Wine module (branch source/winemetalgl)
#   src/mullion      Mullion (DXMT fork, aaf2tbz/mullion); its work goes upstream as pull requests
#   src/wfdxcompat   WFDXCompat + the ARM64EC launcher companion (branch source/wfdxcompat)
#   src/vkd3d-proton VKD3D-Proton macOS (metalsharp/VKD3D-Proton-MacOS): the GRAPHICS_BACKEND=vkd3d lane and
#                    MoltenVK. Its own scripts/fetch-sources.sh fetches the VKD3D-Proton, MoltenVK and DXVK
#                    sources it patches, for stage-vkd3d-proton.sh and stage-moltenvk.sh
#   src/dxvk-macos   DXVK-macOS 1.10.x (metalsharp/DXVK-macOS-1.10, branch metalsharp): the GRAPHICS_BACKEND=dxvk
#                    lane, built as ARM64X by build-dxvk-macos.sh
# Outside a git checkout, the branches are cloned instead. patches/ has the same changes as git
# patches against the upstream tags.
set -e
source ${0:A:h}/env.sh
if [[ -f $ROOT/.gitmodules ]] && git -C $ROOT rev-parse --git-dir >/dev/null 2>&1; then
  git -C $ROOT submodule update --init src/wine src/FEX src/winemetalgl src/mullion src/wfdxcompat src/vkd3d-proton src/dxvk-macos
else
  REPO=${METALSHARP_WINE_REPO:-https://github.com/metalsharp/MetalSharp-Wine.git}
  clone() {  # clone <dir> <url> <branch>
    [[ -e $ROOT/src/$1/.git ]] && { echo "src/$1: already present"; return; }
    git clone --branch $3 --single-branch $2 $ROOT/src/$1
  }
  mkdir -p $ROOT/src
  clone wine $REPO source/wine
  clone FEX $REPO source/fex
  clone winemetalgl $REPO source/winemetalgl
  clone wfdxcompat $REPO source/wfdxcompat
  clone mullion https://github.com/aaf2tbz/mullion.git metalsharp-2.1
  clone vkd3d-proton https://github.com/metalsharp/VKD3D-Proton-MacOS.git main
  clone dxvk-macos https://github.com/metalsharp/DXVK-macOS-1.10.git metalsharp
fi
git -C $ROOT/src/FEX submodule update --init --recursive
echo "sources ready in $ROOT/src"
