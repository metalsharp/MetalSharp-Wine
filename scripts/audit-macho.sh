#!/bin/zsh
# Audit Mach-O files under the given directories:
#  - arm64 only (no x86_64 slices)
#  - LC_BUILD_VERSION minos <= 15.0
#  - no dependencies on Homebrew or other non-system, non-bundle paths
# Usage: audit-macho.sh <dir>... ; exits non-zero on any violation.
bad=0
# STRICT=1: a relocatable bundle may not reference anything in the build tree either.
[[ $STRICT == 1 ]] && allowed='^(/usr/lib/|/System/|@rpath/|@loader_path/|@executable_path/)' \
                  || allowed='^(/usr/lib/|/System/|@rpath/|@loader_path/|@executable_path/|'"$ROOT"')'
newsyms=($(sed -n 's/^ac_cv_func_\(.*\)=no$/_\1/p' ${0:A:h}/config.site))
for d in "$@"; do
  for f in $(find $d -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \) 2>/dev/null); do
    file -b $f | grep -q Mach-O || continue
    archs=$(lipo -archs $f 2>/dev/null)
    [[ $archs == arm64 ]] || { echo "ARCH  $f: $archs"; bad=1; }
    minos=$(vtool -show-build $f 2>/dev/null | awk '/minos/{print $2; exit}')
    [[ -n $minos ]] && { [[ $(printf '%s\n15.0\n' $minos | sort -V | tail -1) == 15.0 ]] || { echo "MINOS $f: $minos"; bad=1; } }
    otool -L $f | tail -n +2 | awk '{print $1}' | grep -vE "$allowed" \
      | sed "s|^|DEP   $f: |" | grep . && bad=1
    [[ $STRICT == 1 ]] && otool -l $f | awk '/LC_RPATH/{getline; getline; print $2}' | grep '^/' \
      | sed "s|^|RPATH $f: |" | grep . && bad=1
    for s in $(nm -u $f 2>/dev/null); do (( ${newsyms[(Ie)$s]} )) && { echo "NEWSYM $f: $s"; bad=1; }; done
  done
done
exit $bad
