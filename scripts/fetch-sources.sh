#!/bin/zsh
# Check out the source trees the build scripts use, into src/. In a clone of this repository they
# are submodules pinned to the commits a release was built from:
#   src/wine         Wine 11.20 + MetalSharp (branch source/wine of this repository)
#   src/FEX          FEX-2610 + MetalSharp (branch source/fex), with its own submodules
#   src/winemetalgl  WineMetalGL, arm64 Wine module (branch source/winemetalgl)
#   src/mullion      Mullion (DXMT fork, aaf2tbz/mullion); its work goes upstream as pull requests
#   src/wfdxcompat   WFDXCompat + the ARM64EC launcher companion (branch source/wfdxcompat)
#   src/linuxhost    the Linux module host for Easy Anti-Cheat (branch source/linuxhost, from metalsharp/Proton)
#   src/steamvm      the Linux Steam microVM bridge (branch source/steamvm, from metalsharp/Proton's darwin/steamvm):
#                    the host daemon, the metalsharp-steam setup command and the guest files, for stage-steamvm.sh
#   src/vkd3d-proton VKD3D-Proton macOS (metalsharp/VKD3D-Proton-MacOS): the GRAPHICS_BACKEND=vkd3d lane and
#                    MoltenVK. Its own scripts/fetch-sources.sh fetches the VKD3D-Proton, MoltenVK and DXVK
#                    sources it patches, for stage-vkd3d-proton.sh and stage-moltenvk.sh
#   src/dxvk-macos   DXVK-MacOS 3.1 (metalsharp/DXVK-MacOS, branch main): the GRAPHICS_BACKEND=dxvk lane, built as
#                    ARM64X by build-dxvk-macos.sh, with its own submodules (dxbc-spirv, Vulkan and SPIR-V headers)
#   src/wine-x86_64  with `fetch-sources.sh x86_64` only: the x86_64 Wine (Wine 11.17 + WineForge + MetalSharp,
#                    branch source/wine-x86_64) that MetalSharp-Wine-x86_64.app is built from (docs/X86_64.md)
# Outside a git checkout, the branches are cloned instead. patches/ has the same changes as git
# patches against the upstream tags.
set -e
source ${0:A:h}/env.sh
if [[ -f $ROOT/.gitmodules ]] && git -C $ROOT rev-parse --git-dir >/dev/null 2>&1; then
  git -C $ROOT submodule update --init src/wine src/FEX src/winemetalgl src/mullion src/wfdxcompat src/linuxhost src/vkd3d-proton src/dxvk-macos
  git -C $ROOT config -f .gitmodules submodule.src/steamvm.path >/dev/null && git -C $ROOT submodule update --init src/steamvm
  [[ ${1:-} = x86_64 ]] && git -C $ROOT submodule update --init src/wine-x86_64
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
  clone linuxhost $REPO source/linuxhost
  clone steamvm $REPO source/steamvm
  clone mullion https://github.com/aaf2tbz/mullion.git metalsharp-2.1-main
  clone vkd3d-proton https://github.com/metalsharp/VKD3D-Proton-MacOS.git main
  clone dxvk-macos https://github.com/metalsharp/DXVK-MacOS.git main
  [[ ${1:-} = x86_64 ]] && clone wine-x86_64 $REPO source/wine-x86_64
fi
git -C $ROOT/src/dxvk-macos submodule update --init --recursive
git -C $ROOT/src/FEX submodule update --init --recursive
echo "sources ready in $ROOT/src"
