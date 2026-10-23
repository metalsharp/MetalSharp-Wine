#!/bin/zsh
# Run Wine conformance tests from the build tree against the installed Wine.
# Usage: run-winetest.sh <arch> <module>...    arch: aarch64 | arm64ec | i386
# Results go to logs/winetest/<arch>/<module>/<unit>.log, summary on stdout.
source ${0:A:h}/env.sh
arch=$1; shift
# WINE_DIST: run against a packaged tree (default: install/)
W=${WINE_DIST:-$ROOT/install}
# WINE_APP: run through the signed MetalSharp-Wine.app (needed for i386: low 4G mappings need the entitlement)
if [[ -n $WINE_APP ]]; then
  WINE=$WINE_APP/Contents/MacOS/wine WINESERVER=$WINE_APP/Contents/MacOS/wineserver
  export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$WINE_APP/Contents/share/vulkan/icd.d/MoltenVK_icd.json}
else
  WINE=$W/bin/wine WINESERVER=$W/bin/wineserver
fi
export WINEPREFIX=${WINETEST_PREFIX:-$ROOT/prefixes/winetest} WINEDEBUG=-all WINETEST_COLOR=0 WINETEST_REPORT_SUCCESS=0
[[ -d $WINEPREFIX ]] || { $WINE wineboot -i >/dev/null 2>&1; $WINESERVER -w; }
# crashes are expected in some units: log them, never pop up the crash dialog on the desktop
$WINE reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
$WINESERVER -p 30 >/dev/null 2>&1

for mod in "$@"; do
  T=${TEST_TREE:-$ROOT/src/wine}
  exe=$(ls $T/dlls/$mod/tests/$arch-windows/*_test.exe(N) $T/programs/$mod/tests/$arch-windows/*_test.exe(N) 2>/dev/null | head -1)
  [[ -n $exe ]] || { echo "$mod: no $arch tests"; continue; }
  out=$ROOT/logs/winetest/$arch/$mod; rm -rf $out; mkdir -p $out
  units=($($WINE $exe --list 2>/dev/null | tr -d '\r' | sed -n 's/^    //p'))
  pass=0; fail=0; crash=0; failed_units=()
  for u in $units; do
    timeout 300 $WINE $exe $u > $out/$u.log 2>&1
    rc=$?
    summary=$(grep -aE "^[0-9a-f]+:$u: [0-9]+ tests executed" $out/$u.log | tail -1)
    if [[ $rc -eq 0 ]]; then (( pass++ ))
    elif [[ -n $summary ]]; then (( fail++ )); failed_units+=("${u}[$(echo $summary | sed -E 's/.*executed \(([0-9]+ marked as todo, )?(.*)\)$/\2/')]")
    else (( crash++ )); failed_units+=("${u}[rc=$rc]"); fi
  done
  echo "$mod [$arch]: ${#units} units, $pass passed, $fail with failures, $crash crashed/timeout ${failed_units:+-- ${failed_units[*]}}"
done
