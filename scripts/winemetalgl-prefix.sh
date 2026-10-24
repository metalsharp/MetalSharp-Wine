#!/bin/zsh
# Switch a prefix's OpenGL to WineMetalGL (or back with --off).
#   winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]
# WINE_ROOT is dist/metalsharp-wine-2.1-arm64 or MetalSharp-Wine.app/Contents (it has lib/winemetalgl).
set -e
W=${1:?usage: winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]}
P=${2:?usage: winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]}
WINE=$W/bin/wine
[[ -x $WINE ]] || WINE=$W/MacOS/wine
S=$P/drive_c/windows
export WINEPREFIX=$P
[[ -d $S ]] || { $WINE wineboot -i >/dev/null 2>&1; }
if [[ $3 == --off ]]; then
  $WINE reg delete 'HKCU\Software\Wine\DllOverrides' /v opengl32 /f >/dev/null 2>&1 || true
  rm -f $S/system32/opengl32.dll $S/syswow64/opengl32.dll
  $WINE wineboot -u >/dev/null 2>&1   # restores Wine's own opengl32 placeholders
  echo "WineMetalGL off for $P"
  exit 0
fi
cp $W/lib/winemetalgl/aarch64-windows/opengl32.dll $S/system32/opengl32.dll
cp $W/lib/winemetalgl/i386-windows/opengl32.dll $S/syswow64/opengl32.dll
$WINE reg add 'HKCU\Software\Wine\DllOverrides' /v opengl32 /t REG_SZ /d native /f >/dev/null 2>&1
echo "WineMetalGL on for $P"
