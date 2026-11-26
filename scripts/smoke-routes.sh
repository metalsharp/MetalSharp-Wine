#!/bin/zsh
# smoke-routes.sh <MetalSharp-Wine.app | MetalSharp-Wine-x86_64.app>
#
# What every game MetalSharp starts relies on, checked on the built app before it ships: for each graphics lane, a
# program whose folder has MetalSharp's .metalsharp/launch.env and ships DLSS (nvngx_dlss.dll) must
#   - get its launch.env (GRAPHICS_BACKEND, and MTL_HUD_ENABLED for the Metal HUD),
#   - be shown the NVIDIA GPU (an RTX 4080, vendor 10de), so games offer DLSS,
#   - load NVAPI (NvAPI_Initialize returns 0),
#   - and, on Mullion, run Mullion (its log, with NVIDIA's extension on),
# both when Wine starts it and when another Windows program starts it (cmd.exe here, Steam for games). The x86_64
# app's lanes are Mullion, D3DMetal and VKD3D / DXVK; the FEX app's Mullion and VKD3D / DXVK. Any failure stops the
# build: run by package.sh's callers (build-all.sh's smoke step) and package-x86_64.sh.
set -e
APP=${1:?usage: smoke-routes.sh <MetalSharp-Wine.app>}
APP=${APP:A}
S=${0:A:h}
ROOT=${S:h}
WINE=$APP/Contents/MacOS/wine
[[ -x $WINE ]] || { echo "smoke-routes: no $WINE" >&2; exit 1; }
BIN=$ROOT/tests/bin
if [[ ! -f $BIN/d3d11info-x86_64.exe || ! -f $BIN/nvapiprobe-x86_64.exe ||
      $BIN/d3d11info-x86_64.exe -ot $ROOT/tests/src/d3d11info.c || $BIN/nvapiprobe-x86_64.exe -ot $ROOT/tests/src/nvapiprobe.c ]]; then
  zsh $S/build-tests.sh >/dev/null 2>&1 || { echo "smoke-routes: building tests/ failed (zsh scripts/build-tests.sh)" >&2; exit 1; }
fi
if [[ -d $APP/Contents/share/metalsharp/runtime/d3dmetal-gptk4-beta3 ]]; then
  lanes=(dxmt d3dmetal vkd3d)
else
  lanes=(dxmt vkd3d)
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/smoke-routes.XXXXXX")
export WINEPREFIX=$WORK/prefix WINEDEBUG=-all
cleanup() { "$APP/Contents/MacOS/wineserver" -k 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT
[[ -x $APP/Contents/MacOS/wineserver ]] || cleanup() { rm -rf "$WORK"; }
echo "smoke-routes: ${APP:t} (${lanes[*]})"
timeout 600 $WINE wineboot -i >/dev/null 2>&1 || { echo "smoke-routes: wineboot failed" >&2; exit 1; }

failures=()
check() {  # <what> <file> <pattern>
  grep -a -q -E "$3" "$2" || failures+=("$1: expected /$3/ in ${2:t}")
}
for lane in $lanes; do
  dir=$WORK/$lane
  mkdir -p $dir/.metalsharp $dir/logs
  cp $BIN/d3d11info-x86_64.exe $BIN/nvapiprobe-x86_64.exe $dir/
  : > $dir/nvngx_dlss.dll  # a DLSS game: the scan goes by the file's name
  print -r -- "GRAPHICS_BACKEND=$lane
MS_GRAPHICS_BACKEND=$lane
METALSHARP_PIPELINE=$lane
MTL_HUD_ENABLED=1
MULLION_LOG_PATH=$dir/logs/" > $dir/.metalsharp/launch.env
  for how in direct child; do
    for probe in d3d11info nvapiprobe; do
      out=$dir/$probe-$how.txt
      rm -f $dir/.metalsharp/dlss
      if [[ $how = direct ]]; then
        (cd $dir && timeout 180 $WINE ./$probe-x86_64.exe > $out 2>&1) || true
      else
        winpath="Z:${dir//\//\\}\\$probe-x86_64.exe"
        (cd $dir && timeout 180 $WINE cmd /c "$winpath" > $out 2>&1) || true
      fi
    done
    tag="$lane, started $how"
    check "$tag" $dir/d3d11info-$how.txt 'frames presented: ok'
    check "$tag" $dir/d3d11info-$how.txt 'adapter: NVIDIA GeForce RTX 4080 \(vendor 10de'
    check "$tag" $dir/d3d11info-$how.txt '^MTL_HUD_ENABLED: 1'
    check "$tag" $dir/d3d11info-$how.txt "^GRAPHICS_BACKEND: $lane"
    check "$tag" $dir/nvapiprobe-$how.txt 'NvAPI_Initialize .* -> 0'
  done
  if [[ $lane = dxmt ]]; then
    log=$(print -l $dir/logs/*dxgi.log(N) | head -1)
    if [[ -n $log ]]; then check "dxmt" $log 'Vendor extension enabled: NVEXT'
    else failures+=("dxmt: Mullion wrote no log (MULLION_LOG_PATH)"); fi
  fi
done

if (( $#failures )); then
  print -l "smoke-routes: FAILED for ${APP:t}:" "  "${^failures}
  for f in $WORK/*/*.txt; do echo "--- ${f#$WORK/}"; head -8 $f; done
  exit 1
fi
echo "smoke-routes: ok (${lanes[*]}: launch.env, Metal HUD, NVIDIA GPU, NVAPI; started directly and by another program)"
