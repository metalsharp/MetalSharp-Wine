#!/bin/zsh
# Stage the Linux Steam microVM runtime into deps/steamvm, laid out as package.sh copies it into the release:
#   bin/krunkit            the microVM runner (libkrun on Hypervisor.framework), signed with the hypervisor entitlement
#   bin/gvproxy            guest networking and port forwarding (gvisor-tap-vsock)
#   bin/metalsharp-vmd     the host desktop bridge daemon, built here from src/steamvm
#   lib/libkrun.1.dylib, lib/libepoxy.0.dylib    (libkrun's MoltenVK is the release's own lib/libMoltenVK.dylib)
#   share/krunkit/KRUN_EFI.silent.fd             the EFI firmware krunkit boots the guest with
#   share/steamvm/metalsharp-steam               the command that sets Linux Steam up and runs it
#   share/steamvm/LinuxSteam.app                 Steam's window on the Mac (metalsharp-steamview), built here
#   share/steamvm/mtsp-pickfolder.exe            Wine's folder browser for "library choose" (arm64 PE), built here
#   share/steamvm/guest/                         what that command installs in the guest
# All of it is native arm64; nothing here uses Rosetta. krunkit, libkrun and libepoxy are Homebrew's builds at the
# versions below (brew install krunkit); gvproxy is the upstream release binary, checked against its SHA-256.
#   stage-steamvm.sh        STEAMVM_SRC=<dir> builds from another checkout of the steamvm sources
#                           STEAMVM_MINGW=<dir> uses the llvm-mingw in that folder instead of env.sh's $MINGW
set -e
source ${0:A:h}/env.sh
[[ -z ${STEAMVM_MINGW:-} ]] || MINGW=$STEAMVM_MINGW
MINGW_CC=$MINGW/bin/aarch64-w64-mingw32-clang
[[ -x $MINGW_CC ]] || { echo >&2 "!! the Windows compiler is missing: $MINGW_CC (unpack llvm-mingw into toolchain/, or set STEAMVM_MINGW to an llvm-mingw folder)"; exit 1; }
SRC=${STEAMVM_SRC:-$ROOT/src/steamvm}
O=$ROOT/deps/steamvm
CACHE=$ROOT/deps/cache/steamvm
BREW=${HOMEBREW_PREFIX:-/opt/homebrew}
KRUNKIT_VERSION=1.3.2
LIBKRUN_VERSION=1.19.8
LIBEPOXY_VERSION=1.5.10
GVPROXY_VERSION=v0.9.0
GVPROXY_SHA256=e2a6825ba789fe588828b418c09aaf16525dec6d8cb2d16c94e72535dd4606be   # gvproxy-darwin (universal)
EDK2_TAG=edk2-stable202505

[[ -f $SRC/host/metalsharp-vmd.c ]] || { echo >&2 "!! src/steamvm is not checked out: zsh scripts/fetch-sources.sh"; exit 1; }

keg() {  # keg <formula> <version>: the installed keg at exactly that version (any revision)
  local k=($BREW/Cellar/$1/$2(N) $BREW/Cellar/$1/$2_<->(Nn))
  (( $#k )) || { echo >&2 "!! Homebrew $1 $2 is not installed (have: $(ls $BREW/Cellar/$1 2>/dev/null | tr '\n' ' ')): brew install $1"; exit 1; }
  echo $k[-1]
}
fetch() {  # fetch <url> <file> [sha256]
  if [[ ! -f $2 ]]; then
    curl -fsSL --retry 3 -o $2.part $1 || { echo >&2 "!! cannot download $1"; exit 1; }
    mv $2.part $2
  fi
  [[ -z $3 || $(shasum -a 256 $2 | cut -d' ' -f1) == $3 ]] || { echo >&2 "!! SHA-256 mismatch for $2"; rm -f $2; exit 1; }
}
KRUNKIT=$(keg krunkit $KRUNKIT_VERSION); LIBKRUN=$(keg libkrun $LIBKRUN_VERSION); LIBEPOXY=$(keg libepoxy $LIBEPOXY_VERSION)

rm -rf $O; mkdir -p $O/bin $O/lib $O/share/krunkit $O/share/steamvm $CACHE
LIC=$O/share/licenses; mkdir -p $LIC/krunkit $LIC/libkrun $LIC/libepoxy $LIC/gvproxy $LIC/edk2

# krunkit, libkrun, libepoxy and the firmware
cp $KRUNKIT/bin/krunkit $O/bin/
cp $KRUNKIT/share/krunkit/KRUN_EFI.silent.fd $O/share/krunkit/
cp -L $LIBKRUN/lib/libkrun.1.dylib $O/lib/libkrun.1.dylib
cp -L $LIBEPOXY/lib/libepoxy.0.dylib $O/lib/libepoxy.0.dylib
chmod u+w $O/bin/krunkit $O/lib/*.dylib $O/share/krunkit/*
cp $KRUNKIT/LICENSE $LIC/krunkit/; cp $LIBKRUN/LICENSE $LIC/libkrun/; cp $LIBEPOXY/COPYING $LIC/libepoxy/
fetch https://raw.githubusercontent.com/tianocore/edk2/$EDK2_TAG/License.txt $CACHE/edk2-License.txt
cp $CACHE/edk2-License.txt $LIC/edk2/License.txt

# gvproxy, arm64 slice
fetch https://github.com/containers/gvisor-tap-vsock/releases/download/$GVPROXY_VERSION/gvproxy-darwin $CACHE/gvproxy-darwin-$GVPROXY_VERSION $GVPROXY_SHA256
fetch https://raw.githubusercontent.com/containers/gvisor-tap-vsock/$GVPROXY_VERSION/LICENSE $CACHE/gvproxy-LICENSE-$GVPROXY_VERSION
lipo $CACHE/gvproxy-darwin-$GVPROXY_VERSION -thin arm64 -output $O/bin/gvproxy; chmod 755 $O/bin/gvproxy
cp $CACHE/gvproxy-LICENSE-$GVPROXY_VERSION $LIC/gvproxy/LICENSE

# the bridge daemon, the setup command and the guest files
# (linked with room for package.sh to add the bundle's @rpath, as everything else in bin/ is)
$=CC $=CFLAGS -Wall -Wextra -Wl,-headerpad_max_install_names -o $O/bin/metalsharp-vmd $SRC/host/metalsharp-vmd.c
install -m 755 $SRC/host/metalsharp-steam $O/share/steamvm/
# the folder browser "library choose" shows: a Windows program (arm64), run by the release's own Wine
$MINGW_CC -O2 -Wall -Wextra -o $O/share/steamvm/mtsp-pickfolder.exe $SRC/host/mtsp-pickfolder.c -lshell32 -lole32
chmod 644 $O/share/steamvm/mtsp-pickfolder.exe
# Steam's window on the Mac: an application of its own, so it has its own Dock icon and name
A=$O/share/steamvm/LinuxSteam.app/Contents
mkdir -p $A/MacOS
$=CC $=CFLAGS -fobjc-arc -Wall -Wextra -Wno-deprecated-declarations -Wl,-headerpad_max_install_names -framework Cocoa -framework QuartzCore \
    -o $A/MacOS/metalsharp-steamview $SRC/host/metalsharp-steamview.m
cat > $A/Info.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>com.metalsharp.wine.linuxsteam</string>
  <key>CFBundleExecutable</key><string>metalsharp-steamview</string>
  <key>CFBundleName</key><string>Steam</string>
  <key>CFBundleDisplayName</key><string>Steam</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>7.0</string>
  <key>CFBundleVersion</key><string>7.0</string>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PLIST
mkdir -p $O/share/steamvm/guest; install -m 755 $SRC/guest/*(.) $O/share/steamvm/guest/
cp $SRC/host/krunkit.entitlements $O/share/steamvm/

# Relocate: everything is found through @rpath (bin/ -> ../lib, lib/ -> itself), as package.sh lays the release out
old() {  # old <file> <library>: the path the file names that library by
  local dep=$(otool -L $1 | awk '{print $1}' | grep "/$2\$" | head -1)
  [[ -n $dep ]] || { echo >&2 "!! ${1:t} does not link $2: has Homebrew's build changed?"; exit 1; }
  echo $dep
}
install_name_tool -change $(old $O/bin/krunkit libkrun.1.dylib) @rpath/libkrun.1.dylib -add_rpath @executable_path/../lib $O/bin/krunkit 2>/dev/null
install_name_tool -id @rpath/libkrun.1.dylib -add_rpath @loader_path \
    -change $(old $O/lib/libkrun.1.dylib libMoltenVK.dylib) @rpath/libMoltenVK.dylib \
    -change $(old $O/lib/libkrun.1.dylib libepoxy.0.dylib) @rpath/libepoxy.0.dylib $O/lib/libkrun.1.dylib 2>/dev/null
install_name_tool -id @rpath/libepoxy.0.dylib $O/lib/libepoxy.0.dylib 2>/dev/null
codesign -f -s - $O/lib/*.dylib $O/bin/gvproxy $O/bin/metalsharp-vmd 2>/dev/null
codesign -f -s - --entitlements $SRC/host/krunkit.entitlements $O/bin/krunkit 2>/dev/null
codesign -f -s - $O/share/steamvm/LinuxSteam.app 2>/dev/null
for f in $O/bin/* $O/lib/*; do [[ $(lipo -archs $f) == arm64 ]] || { echo >&2 "!! $f is not arm64 only"; exit 1; }; done
if otool -L $O/bin/* $O/lib/* | grep -v ':$' | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/|@rpath/)'; then
  echo >&2 "!! a staged binary still names a build machine path"; exit 1
fi

src=$(git -C $SRC rev-parse --short HEAD 2>/dev/null || echo unversioned)
# a checkout with changes not committed says so, with a hash of the sources it was built from
[[ -z $(git -C $SRC status --porcelain -- . 2>/dev/null) ]] ||
  src="$src with local changes, sources sha256 $(cd $SRC && find host guest -type f | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -c1-12)"
echo "Linux Steam microVM runtime: krunkit ${KRUNKIT:t}, libkrun ${LIBKRUN:t}, libepoxy ${LIBEPOXY:t} (Homebrew arm64 builds), gvproxy $GVPROXY_VERSION (sha256 $GVPROXY_SHA256, arm64 slice), bridge $src" > $O/share/steamvm/VERSION
echo "steamvm: $O ($(cat $O/share/steamvm/VERSION))"
