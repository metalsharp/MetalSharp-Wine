#!/bin/zsh
# Wrap the packaged dist in MetalSharp-Wine.app and sign it with the cross-architecture entitlement
# (low 4GB mappings, x86_64 layout emulation, 4K-page spawn). Restricted entitlements need
# a provisioning profile embedded in a bundle whose identifier matches the profile's App ID.
#
#   SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" PROFILE=path/to.provisionprofile \
#     zsh scripts/sign-app.sh
#
# Without PROFILE the bundle is built and signed ad hoc (structure check only).
set -e
S=${0:A:h}
source $S/env.sh
D=$ROOT/dist/metalsharp-wine-2.1-arm64
APP=$ROOT/dist/MetalSharp-Wine.app
BUNDLE_ID=${BUNDLE_ID:-com.metalsharp.wine}
IDENTITY=${SIGN_IDENTITY:--}
PROFILE=${PROFILE:-$( [[ $IDENTITY != - ]] && ls $ROOT/signing/*.provisionprofile 2>/dev/null | head -1 )}

if [[ -n $PROFILE ]]; then
  security cms -D -i $PROFILE > $ROOT/dist/profile.plist
  appid=$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' $ROOT/dist/profile.plist)
  [[ $appid == *.$BUNDLE_ID ]] || { echo "profile App ID $appid does not match $BUNDLE_ID"; exit 1; }
  TEAM=${appid%%.*}
fi

# Layout: Contents/MacOS holds the Mach-O tools, so @loader_path/../lib still resolves to Contents/lib.
rm -rf $APP; mkdir -p $APP/Contents/MacOS
cp -R $D/lib $D/share $APP/Contents/
for f in $D/bin/*; do file -b $f | grep -q Mach-O && cp $f $APP/Contents/MacOS/; done
mv $APP/Contents/MacOS/wine-launcher $APP/Contents/MacOS/wine 2>/dev/null || true
# Wine finds the wineserver and its tools in <libdir>/../bin; point that at MacOS so the bundle
# never falls back to the absolute build-time BINDIR.
ln -s MacOS $APP/Contents/bin
[[ -n $PROFILE ]] && cp $PROFILE $APP/Contents/embedded.provisionprofile

# Every Windows process runs in the inner loader (ntdll execs it, with 4K pages when entitled), and
# macOS only grants restricted entitlements to an executable under a provisioning profile embedded
# in its own bundle: a standalone Mach-O is killed by AMFI ("no eligible provisioning profiles").
# So the inner loader gets a bundle of its own, wine.app next to ntdll.so, which ntdll prefers.
U=$APP/Contents/lib/wine/aarch64-unix
LOADER_APP=$U/wine.app
mkdir -p $LOADER_APP/Contents/MacOS
mv $U/wine $LOADER_APP/Contents/MacOS/wine
install_name_tool -add_rpath @executable_path/../../../../.. $LOADER_APP/Contents/MacOS/wine 2>/dev/null
ln -s wine.app/Contents/MacOS/wine $U/wine
[[ -n $PROFILE ]] && cp $PROFILE $LOADER_APP/Contents/embedded.provisionprofile
sed -e "s/org\.winehq\.wine/$BUNDLE_ID/" -e 's/@PACKAGE_VERSION@/11.20/' \
    $ROOT/src/wine/loader/wine_info.plist.in > $LOADER_APP/Contents/Info.plist

cat > $APP/Contents/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>wine</string>
  <key>CFBundleName</key><string>MetalSharp Wine</string>
  <key>CFBundleDisplayName</key><string>MetalSharp Wine</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>2.1</string>
  <key>CFBundleVersion</key><string>2.1</string>
  <key>CFBundleGetInfoString</key><string>MetalSharp Wine 2.1 (Wine 11.20, arm64, FEX)</string>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
EOF

ENT=$ROOT/dist/wine.entitlements
cat > $ENT <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-executable-page-protection</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
${TEAM:+  <key>com.apple.application-identifier</key><string>$TEAM.$BUNDLE_ID</string>
  <key>com.apple.developer.team-identifier</key><string>$TEAM</string>
  <key>com.apple.developer.cross-architecture-support</key><true/>}
</dict></plist>
EOF

# Other executables (wineserver, tools) get only the unrestricted runtime exceptions: under the
# hardened runtime the wineserver's file mappings are otherwise rejected by library validation.
ENT_BASE=$ROOT/dist/wine-base.entitlements
cat > $ENT_BASE <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-executable-page-protection</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
</dict></plist>
EOF

opts=(-f -s $IDENTITY)
[[ $IDENTITY != - ]] && opts+=(--timestamp)
# Hardened runtime (needed for notarization) unless HARDENED=0
[[ $IDENTITY != - && ${HARDENED:-1} != 0 ]] && opts+=(-o runtime)
# Inside out: libraries and helpers first, the main executable (with entitlements) last.
# Both loaders get the entitlements: the launcher (Contents/MacOS/wine) execs the inner loader
# bundle, which runs every Windows process.
# share/metalsharp holds the MetalSharp app's dylibs and Mono runtimes (stage-metalsharp-assets.sh)
for f in $(find $APP/Contents/lib $APP/Contents/MacOS $APP/Contents/share/metalsharp -type f 2>/dev/null); do
  [[ $f == $APP/Contents/MacOS/wine || $f == $LOADER_APP/* ]] && continue
  case "$(file -b $f)" in
    *Mach-O*executable*) codesign $opts --entitlements $ENT_BASE $f ;;
    *Mach-O*) codesign $opts $f ;;
  esac
done
codesign $opts --entitlements $ENT $LOADER_APP
codesign $opts --entitlements $ENT $APP/Contents/MacOS/wine
codesign $opts --entitlements $ENT $APP
codesign --verify --strict --deep $APP && echo "signed: $APP ($IDENTITY)"
