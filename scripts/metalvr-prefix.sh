#!/bin/zsh
# Make MetalVR a prefix's OpenXR runtime (or take it away again with --off).
#   metalvr-prefix.sh WINE_ROOT PREFIX [--off]
# WINE_ROOT is dist/metalsharp-wine-7.0-arm64 or MetalSharp-Wine.app/Contents (it has lib/metalvr, from
# build-metalvr.sh). A game's OpenXR loader, and OpenComposite's, read HKLM\Software\Khronos\OpenXR\1\ActiveRuntime;
# it is set to the runtime's manifest where it is in WINE_ROOT, so the prefix follows the Wine it runs on and has
# no copy of the runtime to go stale. METALVR_DIR names another lib/metalvr.
#
# An OpenVR (SteamVR) game also needs its own openvr_api.dll replaced with lib/metalvr/x86_64-windows/openvr_api.dll.
# metalvrd has to be running before the game starts: bin/metalvrd --trust <the name the headset's ALVR app shows>.
set -e
W=${1:?usage: metalvr-prefix.sh WINE_ROOT PREFIX [--off]}
P=${2:?usage: metalvr-prefix.sh WINE_ROOT PREFIX [--off]}
WINE=$W/bin/wine
[[ -x $WINE ]] || WINE=$W/MacOS/wine
M=${METALVR_DIR:-$W/lib/metalvr}
KEY='HKLM\Software\Khronos\OpenXR\1'
export WINEPREFIX=$P
export WINEDEBUG=${WINEDEBUG:--all}
[[ -d $P/drive_c/windows ]] || { $WINE wineboot -i >/dev/null 2>&1; }
if [[ $3 == --off ]]; then
  $WINE reg delete $KEY /v ActiveRuntime /f >/dev/null 2>&1 || true
  echo "MetalVR off for $P"
  exit 0
fi
J=$M/x86_64-windows/metalvr_openxr.json
[[ -f $J && -f $M/x86_64-windows/metalvr_openxr.dll ]] || { echo >&2 "!! no MetalVR in $M: zsh scripts/build-metalvr.sh"; exit 1; }
# the manifest's path as the prefix sees it; the manifest names the DLL beside it
WJ=$($WINE winepath -w $J 2>/dev/null | tr -d '\r')
[[ -n $WJ ]] || { echo >&2 "!! winepath gave no Windows path for $J"; exit 1; }
$WINE reg add $KEY /v ActiveRuntime /t REG_SZ /d $WJ /f >/dev/null 2>&1
print -r -- "MetalVR on for $P ($WJ)"
