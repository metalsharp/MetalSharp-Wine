#!/bin/zsh
# MetalSharp app-level payloads, staged by the MetalSharp app into game folders and prefixes, carried
# in install/share/metalsharp with the bundles' own layout (assets/..., goldberg, steam, ...):
#   goldberg (Steam emulator), the Steam installer and CEF wrapper, FNA/XNA, fnalibs, the FNA
#   MonoKickstart, Unity Mono lanes, SDL3, the dylib shims, the Terraria launchers, the CEF launcher
#   hooks, Mullion's default mullion.conf (the bundles' dxmt.conf) and the compat rules
#   (mscompatdb_rules.toml).
# Mono (arm64 and x86_64) and FAudio go in as archives/<name>.tar.zst (top folder <name>), which the
# app extracts into its runtime/ during setup; compressed, they stay out of signing and notarization
# and keep the app small. Setup needs nothing else from the network.
# Not metalsharp_ntdll_hook.dll. The i386 slices of the FNA kickstart's Mono are removed (macOS 15
# can't run them, and notarization rejects them); x86_64-only dylibs are kept for the app's
# Rosetta routes. sign-app.sh signs every Mach-O here.
#   stage-metalsharp-assets.sh [path/to/metalsharp-bundles/extract]
set -e
source ${0:A:h}/env.sh
X=${1:-$ROOT/downloads/metalsharp-bundles/extract}
O=$ROOT/install/share/metalsharp
rm -rf $O/assets $O/goldberg $O/fnalibs $O/steam $O/tools $O/etc $O/archives  # not runtime/ or redist/ (stage-metalsharp-redist.sh)
mkdir -p $O/assets/wine/etc $O/tools $O/etc
for a in goldberg fna-kickstart xna fnalibs unity-mono sdl3 shims prebuilt-launchers; do
  rsync -a $X/metalsharp-assets/assets/$a $O/assets/
done
# MetalFX off until the app turns it on (factor 1.00 off, as the app writes it, for WineMetalGL)
sed -e 's/^d3d11.metalSpatialUpscaleFactor.*/d3d11.metalSpatialUpscaleFactor = 1.00\
d3d11.metalSpatialUpscale = False/' $X/metalsharp-assets/assets/wine/etc/dxmt.conf > $O/assets/wine/etc/mullion.conf
rsync -a $X/goldberg/ $O/goldberg/
rsync -a $X/metalsharp-steam/steam/ $O/steam/
rsync -a $X/metalsharp-scripts-tools/scripts/tools/cef $O/tools/
cp $X/metalsharp-runtime/runtime/wine/etc/etc/mscompatdb_rules.toml $O/etc/
# fnalibs.tar.zst is identical to assets/fnalibs
diff -rq $X/fnalibs/fnalibs $O/assets/fnalibs >/dev/null || rsync -a $X/fnalibs/fnalibs/ $O/fnalibs/

for f in $(find $O -type f); do
  file -b $f | grep -q Mach-O || continue
  [[ " $(lipo -archs $f 2>/dev/null) " == *" i386 "* ]] && lipo $f -remove i386 -output $f
done
find $O -name metalsharp_ntdll_hook.dll -delete

mkdir -p $O/archives
for a in mono-arm64 mono-x86 faudio; do
  tar -C $X/metalsharp-assets/assets -cf - $a | zstd -q -19 -T0 -o $O/archives/$a.tar.zst
done
echo "metalsharp assets: $(du -sh $O | cut -f1), $(find $O -type f | wc -l | tr -d ' ') files"
