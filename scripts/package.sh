#!/bin/zsh
# Turn install/ + deps/prefix into a relocatable, self-contained tree in dist/<name>.
# All dependency references become @rpath, absolute rpaths are removed, binaries are re-signed.
set -e
source ${0:A:h}/env.sh
NAME=${1:-metalsharp-wine-2.1-arm64}
D=$ROOT/dist/$NAME
rm -rf $D; mkdir -p $D
rsync -a $ROOT/install/ $D/

# Dependencies: shared libs (with their symlinks), GStreamer plugins + scanner, Vulkan ICD.
rsync -a --include='*.dylib' --exclude='*' $PREFIX/lib/ $D/lib/
rsync -a --include='*.dylib' --exclude='*' $PREFIX/lib/gstreamer-1.0/ $D/lib/gstreamer-1.0/
# MetalSharp's MoltenVK 1.4.3 (scripts/stage-moltenvk.sh) replaces the 1.4.2 from build-deps.sh
if [[ -f $ROOT/deps/moltenvk/libMoltenVK.dylib ]]; then
  rm -f $D/lib/libMoltenVK.dylib; cp $ROOT/deps/moltenvk/libMoltenVK.dylib $D/lib/libMoltenVK.dylib
fi
# innoextract (scripts/build-innoextract.sh), for unpacking Inno Setup installers
if [[ -x $ROOT/deps/innoextract/innoextract ]]; then
  cp $ROOT/deps/innoextract/innoextract $D/bin/
  mkdir -p $D/share/licenses/innoextract; cp $ROOT/deps/innoextract/LICENSE.* $D/share/licenses/innoextract/
fi
# cabextract (scripts/build-cabextract.sh), for Microsoft cabinet installers (corefonts, VC++)
if [[ -x $ROOT/deps/cabextract/cabextract ]]; then
  cp $ROOT/deps/cabextract/cabextract $D/bin/
  mkdir -p $D/share/licenses/cabextract; cp $ROOT/deps/cabextract/LICENSE.* $D/share/licenses/cabextract/
fi
mkdir -p $D/libexec/gstreamer-1.0 $D/share/vulkan/icd.d
cp $PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner $D/libexec/gstreamer-1.0/
sed 's#"library_path": *"[^"]*"#"library_path": "../../../lib/libMoltenVK.dylib"#' \
    $PREFIX/share/vulkan/icd.d/MoltenVK_icd.json > $D/share/vulkan/icd.d/MoltenVK_icd.json

# Relative rpath (to $D/lib) for each kind of binary.
rpath_for() {
  case $1 in
    $D/lib/wine/aarch64-unix/wine)    echo @executable_path/../.. ;;
    $D/lib/wine/aarch64-unix/*)       echo @loader_path/../.. ;;
    $D/lib/gstreamer-1.0/*)           echo @loader_path/.. ;;
    $D/libexec/gstreamer-1.0/*)       echo @executable_path/../../lib ;;
    $D/bin/*)                         echo @executable_path/../lib ;;
    $D/lib/*)                         echo @loader_path ;;
  esac
}

for f in $(find $D -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \)); do
  file -b $f | grep -q Mach-O || continue
  args=()
  # dylib ids
  id=$(otool -D $f | sed -n 2p)
  [[ $id == $PREFIX/lib/* ]] && args+=(-id @rpath/${id#$PREFIX/lib/})
  # dependencies
  for dep in $(otool -L $f | tail -n +2 | awk '{print $1}' | grep "^$PREFIX/lib/"); do
    args+=(-change $dep @rpath/${dep#$PREFIX/lib/})
  done
  # rpaths: drop absolute ones, add the relative one
  for rp in $(otool -l $f | awk '/LC_RPATH/{getline; getline; print $2}'); do
    [[ $rp == /* ]] && args+=(-delete_rpath $rp)
  done
  want=$(rpath_for $f)
  [[ -n $want ]] && ! otool -l $f | grep -q "path $want " && args+=(-add_rpath $want)
  (( $#args )) && install_name_tool $args $f 2>/dev/null
  codesign -f -s - $f 2>/dev/null
done

# Launcher: point GStreamer and the Vulkan loader inside the bundle, then run the real loader.
mv $D/bin/wine $D/bin/wine-launcher
cat > $D/bin/wine <<'EOF'
#!/bin/sh
here=$(cd "$(dirname "$0")/.." && pwd -P)
: "${GST_PLUGIN_SYSTEM_PATH_1_0:=$here/lib/gstreamer-1.0}"
: "${GST_PLUGIN_SCANNER_1_0:=$here/libexec/gstreamer-1.0/gst-plugin-scanner}"
: "${VK_DRIVER_FILES:=$here/share/vulkan/icd.d/MoltenVK_icd.json}"
export GST_PLUGIN_SYSTEM_PATH_1_0 GST_PLUGIN_SCANNER_1_0 VK_DRIVER_FILES
exec "$here/bin/wine-launcher" "$@"
EOF
chmod +x $D/bin/wine
echo "packaged: $D"

# Licenses of everything redistributed in the bundle.
LIC=$D/share/licenses; mkdir -p $LIC/wine $LIC/fex $LIC/llvm-mingw
cp $ROOT/src/wine/COPYING.LIB $ROOT/src/wine/LICENSE $ROOT/src/wine/AUTHORS $LIC/wine/
cp $ROOT/src/FEX/LICENSE $LIC/fex/
cp $MINGW/LICENSE.TXT $LIC/llvm-mingw/   # libc++ / compiler-rt / mingw-w64 CRT linked into the FEX DLLs
for p in $(ls $ROOT/deps/src); do
  s=$(ls -d $ROOT/deps/src/$p/*/ | head -1)
  mkdir -p $LIC/$p
  for f in $(ls $s | grep -iE '^(copying|license|licence|lgpl|gpl|copyright|notice)'); do
    [[ -f $s/$f ]] && cp $s/$f $LIC/$p/
  done
  [[ $p == freetype ]] && cp $s/docs/FTL.TXT $s/docs/GPLv2.TXT $LIC/$p/ 2>/dev/null
done
cp $ROOT/README.md $D/README.md 2>/dev/null
mkdir -p $D/share/doc/metalsharp && cp $ROOT/docs/*.md $D/share/doc/metalsharp/ 2>/dev/null

# Release archive + checksum
(cd $ROOT/dist && rm -f $NAME.tar.xz $NAME.tar.xz.sha256 && tar -cJf $NAME.tar.xz $NAME && shasum -a 256 $NAME.tar.xz > $NAME.tar.xz.sha256)
echo "archive: $ROOT/dist/$NAME.tar.xz ($(du -h $ROOT/dist/$NAME.tar.xz | cut -f1))"
