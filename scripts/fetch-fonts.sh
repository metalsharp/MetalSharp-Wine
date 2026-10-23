#!/bin/zsh
# Liberation 2.1.5 (SIL OFL; metric-compatible with Arial, Times New Roman and Courier New) under
# the Windows file names, in install/share/wine/fonts. wine.inf (FontFiles) copies them and Wine's
# Tahoma into C:\windows\Fonts, for programs that open font files there by name (Unity's dynamic
# fonts, for one) instead of asking GDI. A real Arial in a prefix is never overwritten.
# The checksum matches the Homebrew font-liberation cask.
set -e
source ${0:A:h}/env.sh
D=$ROOT/downloads/fonts; L=liberation-fonts-ttf-2.1.5
mkdir -p $D
[[ -f $D/$L.tar.gz ]] || curl -fsSL -o $D/$L.tar.gz https://github.com/liberationfonts/liberation-fonts/files/7261482/$L.tar.gz
[[ $(shasum -a 256 $D/$L.tar.gz | cut -d' ' -f1) == 7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0 ]] ||
  { echo "checksum mismatch: $L.tar.gz" >&2; exit 1; }
rm -rf $D/$L; tar -xzf $D/$L.tar.gz -C $D
F=$ROOT/install/share/wine/fonts
# Built like Proton's: the family names inside the files become Arial, Times New Roman and
# Courier New, since programs (Unity) match fonts by those names, not by file name. The OFL
# requires a modified Liberation to drop that reserved name, which this does. fontTools is a
# build-only venv in deps/.
V=$ROOT/deps/fonttools-venv
[[ -x $V/bin/python ]] || { python3 -m venv $V && $V/bin/pip install -q fonttools==4.55.0; }
for lib win family in Sans arial Arial Serif times "Times New Roman" Mono cour "Courier New"; do
  for style suffix in Regular "" Bold bd Italic i BoldItalic bi; do
    $V/bin/python -I ${0:A:h}/rename-font.py $D/$L/Liberation$lib-$style.ttf $F/$win$suffix.ttf "$family"
  done
done
mkdir -p $ROOT/install/share/licenses/liberation-fonts
cp $D/$L/LICENSE $D/$L/AUTHORS $ROOT/install/share/licenses/liberation-fonts/
ls $F | grep -E '^(arial|times|cour)'
