#!/bin/zsh
# The whole of MetalSharp-Wine.app from a fresh clone, in order: the dependencies, the ARM64X runtimes, Wine, FEX,
# every graphics lane and add-on the release carries, the tests, then the audit, the package and the signed app
# (dist/MetalSharp-Wine.app). docs/BUILDING.md has what it needs. Each step logs to logs/build-all/<n>-<step>.log;
# the first failure stops the run and names its log.
#   build-all.sh               every step
#   build-all.sh <n|step>      from that step on (after fixing what stopped a run)
#   build-all.sh --list        the steps
# Environment:
#   MACOS15_SDK=<path>/MacOSX15.x.sdk   required: packaging holds the payloads to macOS 15 with it
#   SIGN_IDENTITY, PROFILE              Developer ID certificate and provisioning profile (sign-app.sh); without
#                                       them the app is signed ad hoc, which runs nothing with FEX's entitlement
#   MULLION_SIGNED_NGX_DIR, MULLION_NGX_SIGNING_KEY_FILE
#                                       Mullion's signed NGX core (build-mullion.sh); with neither, the newest one
#                                       signed for src/mullion's nvngx is fetched (fetch-signed-ngx.sh)
#   SKIP="step step"                    steps to leave out (the app then lacks them). The first step, pins,
#                                       stops a build whose src/ pins are behind their branches or whose
#                                       submodule checkouts aren't at their pins (scripts/pins.py)
#   NAME=metalsharp-wine-<version>-arm64   the dist folder package.sh makes (sign-app.sh's default)
set -e
S=${0:A:h}
source $S/env.sh
NAME=${NAME:-metalsharp-wine-10.0-arm64}
LOG=$ROOT/logs/build-all

steps=(
  "pins|python3 $S/pins.py check --worktree"
  "toolchain|zsh $S/fetch-toolchain.sh"
  "deps|zsh $S/build-deps.sh"
  "arm64ec-runtimes|[[ -e $ROOT/deps/stamps/arm64ec-runtimes ]] || { zsh $S/build-arm64ec-runtimes.sh && touch $ROOT/deps/stamps/arm64ec-runtimes; }"
  "wine|zsh $S/build-wine.sh"
  "fex|zsh $S/build-fex.sh"
  "mullion|zsh $S/build-mullion.sh"
  "winemetalgl|zsh $S/build-winemetalgl.sh"
  "wfdxcompat|zsh $S/build-wfdxcompat.sh"
  "linuxhost|zsh $S/build-linuxhost.sh"
  "vkd3d-proton|zsh $S/build-vkd3d-proton.sh"
  "dxvk|zsh $S/build-dxvk-macos.sh && zsh $S/stage-dxvk.sh"
  "shader-compilers|zsh $S/stage-shader-compilers.sh"
  "addons|zsh $S/fetch-addons.sh"
  "fonts|zsh $S/fetch-fonts.sh"
  "innoextract|zsh $S/build-innoextract.sh"
  "cabextract|zsh $S/build-cabextract.sh"
  "metalsharp-assets|zsh $S/fetch-metalsharp-assets.sh && zsh $S/stage-metalsharp-assets.sh"
  "metalsharp-redist|zsh $S/stage-metalsharp-redist.sh"
  "kosmickrisp|zsh $S/build-kosmickrisp.sh"
  "zink|zsh $S/build-zink.sh"
  "metalvr|zsh $S/build-metalvr.sh"
  "tests|zsh $S/build-tests.sh"
  "audit|zsh $S/audit-macho.sh $ROOT/deps/prefix $ROOT/install"
  "package|zsh $S/package.sh $NAME"
  "audit-dist|STRICT=1 zsh $S/audit-macho.sh $ROOT/dist/$NAME"
  "sign|zsh $S/sign-app.sh"
)

if [[ $1 == --list ]]; then
  for i in {1..$#steps}; do echo "$i ${steps[$i]%%|*}"; done; exit 0
fi
start=1
if [[ -n $1 ]]; then
  if [[ $1 == <-> ]]; then start=$1
  else
    for i in {1..$#steps}; do [[ ${steps[$i]%%|*} == $1 ]] && start=$i; done
    [[ ${steps[$start]%%|*} == $1 ]] || { echo "build-all: no step $1 (build-all.sh --list)" >&2; exit 1; }
  fi
fi

# What would otherwise stop a run hours in
skipped() { [[ " $SKIP " == *" $1 "* ]] }
missing=()
tools=(cmake ninja meson pkgconf nasm gperf zstd rsync xz)
skipped mullion || tools+=(osslsigncode gh)
skipped metalvr || [[ -x $HOME/.cargo/bin/cargo ]] || tools+=(cargo)   # build-metalvr.sh adds rustup's to PATH
for t in $tools; do command -v $t >/dev/null || missing+=($t); done
kegs=(bison flex gettext)
skipped kosmickrisp || kegs+=(llvm spirv-llvm-translator libclc)
for f in $kegs; do [[ -d /opt/homebrew/opt/$f ]] || missing+=($f); done
(( $#missing )) && { echo "build-all: missing $missing (brew install them; cargo from https://rustup.rs)" >&2; exit 1; }
xcrun -sdk macosx metal -v >/dev/null 2>&1 ||
  { echo "build-all: needs the Metal Toolchain (xcodebuild -downloadComponent MetalToolchain)" >&2; exit 1; }
[[ -d ${MACOS15_SDK:-/nonexistent} ]] ||
  { echo "build-all: set MACOS15_SDK to a macOS 15 SDK (MacOSX15.x.sdk, from Xcode 16): packaging needs it" >&2; exit 1; }
[[ -f $ROOT/src/wine/configure && -f $ROOT/src/FEX/CMakeLists.txt && -f $ROOT/src/mullion/meson.build &&
   -f $ROOT/src/metalvulkan/scripts/build-moltenvk.sh && -f $ROOT/src/winemetalgl/CMakeLists.txt ]] ||
  { echo "build-all: src/ is not checked out (zsh scripts/fetch-sources.sh)" >&2; exit 1; }
if ! skipped mullion && [[ -z $MULLION_SIGNED_NGX_DIR && -z $MULLION_NGX_SIGNING_KEY_FILE &&
                           $MULLION_NGX_UNSIGNED != 1 ]]; then
  ngx=$(zsh $S/fetch-signed-ngx.sh) ||
    { echo "build-all: Mullion's NGX core (DLSS) needs signing: see scripts/fetch-signed-ngx.sh" >&2; exit 1; }
  export MULLION_SIGNED_NGX_DIR=$ngx
fi
[[ -n $SIGN_IDENTITY ]] || echo "build-all: no SIGN_IDENTITY: the app will be signed ad hoc (sign-app.sh)" >&2

mkdir -p $LOG
for i in {$start..$#steps}; do
  name=${steps[$i]%%|*}; cmd=${steps[$i]#*|}
  if skipped $name; then echo "[$(date +%T)] $i $name: skipped"; continue; fi
  echo "[$(date +%T)] $i $name"
  if ! (cd $ROOT && eval $cmd) > $LOG/$i-$name.log 2>&1; then
    echo "[$(date +%T)] build-all: $name failed; its log is $LOG/$i-$name.log. Fix it, then: zsh scripts/build-all.sh $name" >&2
    tail -20 $LOG/$i-$name.log >&2
    exit 1
  fi
done
echo "[$(date +%T)] build-all: ok, $ROOT/dist/MetalSharp-Wine.app"
