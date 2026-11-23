#!/bin/zsh
# Put WineMetalGL's opengl32.dll back in a prefix's System32 and SysWOW64 (or, with --off, Wine's own).
#   winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]
# WINE_ROOT is dist/metalsharp-wine-<version>-arm64 or MetalSharp-Wine.app/Contents.
# WineMetalGL's opengl32.dll is the builtin opengl32 (lib/wine), so wineboot already gives a prefix its copies, and
# ntdll loads it for opengl32.dll whatever the prefix holds. --off only changes the prefix's copies: Wine's own
# opengl32 (lib/wine-opengl) is loaded with OPENGL_BACKEND=wine or WINEMETALGL=0. Bundles from before
# lib/wine-opengl kept the forwarder in lib/winemetalgl and needed opengl32=native; this handles them too.
set -e
W=${1:?usage: winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]}
P=${2:?usage: winemetalgl-prefix.sh WINE_ROOT PREFIX [--off]}
WINE=$W/bin/wine
[[ -x $WINE ]] || WINE=$W/MacOS/wine
S=$P/drive_c/windows
export WINEPREFIX=$P
[[ -d $S ]] || { $WINE wineboot -i >/dev/null 2>&1; }
if [[ -d $W/lib/wine-opengl ]]; then
  if [[ $3 == --off ]]; then F=$W/lib/wine-opengl; else F=$W/lib/wine; fi
  $WINE reg delete 'HKCU\Software\Wine\DllOverrides' /v opengl32 /f >/dev/null 2>&1 || true
  cp $F/aarch64-windows/opengl32.dll $S/system32/opengl32.dll
  cp $F/i386-windows/opengl32.dll $S/syswow64/opengl32.dll
  [[ $3 == --off ]] && echo "Wine's opengl32 in $P (run with OPENGL_BACKEND=wine to load it)" || echo "WineMetalGL on for $P"
  exit 0
fi
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
