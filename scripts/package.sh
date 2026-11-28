#!/bin/zsh
# Turn install/ + deps/prefix into a relocatable, self-contained tree in dist/<name>.
# All dependency references become @rpath, absolute rpaths are removed, binaries are re-signed.
set -e
source ${0:A:h}/env.sh
NAME=${1:-metalsharp-wine-11.0-arm64}
D=$ROOT/dist/$NAME
rm -rf $D; mkdir -p $D
rsync -a --exclude=/share/wine/shadercompilers/x64 $ROOT/install/ $D/  # x64: the x86_64 app's (package-x86_64.sh)

# Dependencies: shared libs (with their symlinks), GStreamer plugins + scanner, Vulkan ICD.
rsync -a --include='*.dylib' --exclude='*' $PREFIX/lib/ $D/lib/
rsync -a --include='*.dylib' --exclude='*' $PREFIX/lib/gstreamer-1.0/ $D/lib/gstreamer-1.0/
# MetalSharp's MoltenVK 1.4.3 (scripts/stage-moltenvk.sh) replaces the 1.4.2 from build-deps.sh
if [[ -f $ROOT/deps/moltenvk/libMoltenVK.dylib ]]; then
  rm -f $D/lib/libMoltenVK.dylib; cp $ROOT/deps/moltenvk/libMoltenVK.dylib $D/lib/libMoltenVK.dylib
fi
# Metal Shader Converter's library (Apple, shipped unmodified and Apple-signed under its license, section 2B), which
# MoltenVK loads to convert DXIL shaders for VKD3D-Proton (docs/MSC-BRIDGE-CONTRACT.md), with its license and
# acknowledgements: the files MetalVulkan's MoltenVK package carries (its fetch script checks their SHA-256s).
MSC=$ROOT/src/metalvulkan/artifacts/metal-shaderconverter
MSC_FETCH=$ROOT/src/metalvulkan/scripts/fetch-metal-shaderconverter.sh
[[ -f $MSC/libmetalirconverter.dylib || ! -f $MSC_FETCH ]] || bash $MSC_FETCH
if [[ -f $MSC/libmetalirconverter.dylib ]]; then
  cp $MSC/libmetalirconverter.dylib $D/lib/
  mkdir -p $D/share/licenses/metal-shaderconverter
  cp $MSC/LICENSE.metal-shaderconverter.rtf $MSC/ACKNOWLEDGEMENTS.metal-shaderconverter.rtf $D/share/licenses/metal-shaderconverter/
fi
# DirectXShaderCompiler's DXBC to DXIL converter (dxilconv, NCSA), which MoltenVK loads to hand VKD3D-Proton's DXBC
# shaders to Metal Shader Converter: built by scripts/build-vkd3d-proton.sh, arm64 slice. Without it, DXBC shaders
# stay on SPIR-V.
DXBC=$ROOT/src/metalvulkan/artifacts/dxbcconverter
[[ -f $DXBC/libdxbcconverter.dylib ]] ||
  echo "warning: no $DXBC/libdxbcconverter.dylib (scripts/build-vkd3d-proton.sh): DXBC shaders stay on SPIR-V" >&2
if [[ -f $DXBC/libdxbcconverter.dylib ]]; then
  lipo $DXBC/libdxbcconverter.dylib -thin arm64 -output $D/lib/libdxbcconverter.dylib 2>/dev/null || cp $DXBC/libdxbcconverter.dylib $D/lib/
  mkdir -p $D/share/licenses/dxc; cp $DXBC/LICENSE.dxc.txt $D/share/licenses/dxc/LICENSE.dxilconv.txt
fi
# innoextract (scripts/build-innoextract.sh), for unpacking Inno Setup installers
if [[ -x $ROOT/deps/innoextract/innoextract ]]; then
  cp $ROOT/deps/innoextract/innoextract $D/bin/
  mkdir -p $D/share/licenses/innoextract; cp $ROOT/deps/innoextract/LICENSE.* $D/share/licenses/innoextract/
fi
# Proton's per-game rules (Wine patch 0141 applies them to the programs Steam starts). metalsharp/Proton
# generates the file from its launcher with appcompat/extract.py; resources/ keeps a copy.
mkdir -p $D/share/proton; cp $ROOT/resources/appcompat.env $D/share/proton/appcompat.env
# Microsoft's Edge WebView2 Evergreen bootstrapper, unmodified (Wine patch 0167): kernelbase runs it in a prefix the
# first time a program there loads WebView2Loader.dll, and it installs the WebView2 Runtime from Microsoft.
mkdir -p $D/share/wine/webview2; cp $ROOT/resources/webview2/MicrosoftEdgeWebview2Setup.exe $D/share/wine/webview2/
# MetalVR (scripts/build-metalvr.sh): install/ has bin/metalvrd and lib/metalvr already; its licenses, and those of
# ALVR and OpenComposite, go with the rest
if [[ -d $D/lib/metalvr ]]; then
  mkdir -p $D/share/licenses/metalvr
  cp -R $D/lib/metalvr/LICENSE $D/lib/metalvr/LICENSE.ALVR $D/lib/metalvr/OpenComposite $D/share/licenses/metalvr/
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

# The oldest macOS the release supports is 15.0: payloads linked on a newer macOS without a deployment target are
# held to it once the macOS 15 SDK shows they need nothing newer (scripts/macos-floor.py; the loop below signs them)
python3 -I ${0:A:h}/macos-floor.py --sdk ${MACOS15_SDK:?set MACOS15_SDK to a macOS 15 SDK (MacOSX15.x.sdk)} --fix $D

for f in $(find $D -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \)); do
  file -b $f | grep -q Mach-O || continue
  # Apple's converter library ships unmodified (its license forbids changes); it links system libraries only.
  [[ $f == $D/lib/libmetalirconverter.dylib ]] && continue
  args=()
  # dylib ids
  id=$(otool -D $f | sed -n 2p)
  # any deps/prefix, not only this tree's: a build tree copied or moved from another still names that one's
  if [[ $id == /*/deps/prefix/lib/* ]]; then args+=(-id @rpath/${id##*/deps/prefix/lib/})
  # any other id that names a build machine's path (the build tree, Homebrew, /tmp) is its own name under @rpath
  elif [[ $id == /* && $id != /usr/lib/* && $id != /System/* ]]; then args+=(-id @rpath/${id:t})
  fi
  # dependencies
  for dep in $(otool -L $f | tail -n +2 | awk '{print $1}' | grep '^/.*/deps/prefix/lib/'); do
    args+=(-change $dep @rpath/${dep##*/deps/prefix/lib/})
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

# Launcher: point GStreamer inside the bundle, then run the real loader. The Vulkan driver (MoltenVK, or KosmicKrisp
# with SharpVK_Switch=1) is chosen by ntdll, which the signed app runs without this script.
mv $D/bin/wine $D/bin/wine-launcher
cat > $D/bin/wine <<'EOF'
#!/bin/sh
here=$(cd "$(dirname "$0")/.." && pwd -P)
: "${GST_PLUGIN_SYSTEM_PATH_1_0:=$here/lib/gstreamer-1.0}"
: "${GST_PLUGIN_SCANNER_1_0:=$here/libexec/gstreamer-1.0/gst-plugin-scanner}"
export GST_PLUGIN_SYSTEM_PATH_1_0 GST_PLUGIN_SCANNER_1_0
# winedbg, winecfg, regedit... are symlinks to this script; the loader picks the builtin program from argv[0]'s
# basename, which exec turns into wine-launcher, so pass the name on as the program to run (wine winedbg ...)
name=${0##*/}
case $name in wine|wine-launcher) ;; *) set -- "$name" "$@" ;; esac
exec "$here/bin/wine-launcher" "$@"
EOF
chmod +x $D/bin/wine
echo "packaged: $D"

# Licenses of everything redistributed in the bundle.
LIC=$D/share/licenses; mkdir -p $LIC/wine $LIC/fex $LIC/llvm-mingw $LIC/wfdxcompat
# Wine's licenses, and NOTICES.md: the notices of the third-party code Wine imports
cp $ROOT/src/wine/COPYING.LIB $ROOT/src/wine/LICENSE $ROOT/src/wine/AUTHORS $ROOT/src/wine/NOTICES.md $LIC/wine/
# WFDXCompat, WineForge's DirectX companion by Radim Vesely (LGPL-2.1-or-later); lib/wfdxcompat has it too
cp $ROOT/src/wfdxcompat/LICENSE $LIC/wfdxcompat/
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

# Release archive + checksum (single-threaded xz, many minutes; NO_TARBALL=1 skips it when only the app is needed)
[[ ${NO_TARBALL:-0} == 1 ]] && exit 0
(cd $ROOT/dist && rm -f $NAME.tar.xz $NAME.tar.xz.sha256 && tar -cJf $NAME.tar.xz $NAME && shasum -a 256 $NAME.tar.xz > $NAME.tar.xz.sha256)
echo "archive: $ROOT/dist/$NAME.tar.xz ($(du -h $ROOT/dist/$NAME.tar.xz | cut -f1))"
