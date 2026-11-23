#!/bin/zsh
# The MetalSharp app's payloads that this repository does not build (Goldberg, the Steam installer, FNA/XNA, Unity
# Mono, SDL3, the shims, the host runtime...: see stage-metalsharp-assets.sh), for stage-metalsharp-assets.sh and
# stage-metalsharp-redist.sh. Those scripts read MetalSharp's bundles from downloads/metalsharp-bundles/extract when
# they are there. Without them, this script puts a release's share/metalsharp in
# downloads/metalsharp-release-assets/metalsharp, from
#   fetch-metalsharp-assets.sh <MetalSharp-Wine.app | its Contents | a dist/metalsharp-wine-* | a share/metalsharp>
# or, with no argument, from the MetalSharp Wine release below (its tarball, about 1 GB, checked against the SHA-256
# in that release's SHA256SUMS). The stage scripts then take everything from there but what this repository makes
# (resources/winemac.conf, the Microsoft redistributables).
set -e
source ${0:A:h}/env.sh
R=$ROOT/downloads/metalsharp-release-assets
RELEASE=v8.0 NAME=metalsharp-wine-8.0-arm64
SHA=654a108bd9688bf1d11d4d994a3e4544050e0a9837323a6c56b84bd0bdac823d

if [[ -d $ROOT/downloads/metalsharp-bundles/extract && -z $1 ]]; then
  echo "metalsharp assets: from downloads/metalsharp-bundles/extract"; exit 0
fi
if [[ -n $1 ]]; then
  F=${1:A}
  for s in $F/share/metalsharp $F/Contents/share/metalsharp $F; do
    [[ -d $s/assets && -d $s/runtime/host ]] && { src=$s; break; }
  done
  [[ -n $src ]] || { echo "fetch-metalsharp-assets: no share/metalsharp (with assets/ and runtime/host) in $1" >&2; exit 1; }
  rm -rf $R; mkdir -p $R
  cp -c -R -p $src $R/metalsharp 2>/dev/null || cp -R -p $src $R/metalsharp
else
  [[ -d $R/metalsharp/assets && $(cat $R/RELEASE 2>/dev/null) == $RELEASE ]] &&
    { echo "metalsharp assets: from MetalSharp Wine $RELEASE (downloaded)"; exit 0; }
  T=$ROOT/downloads/$NAME.tar.xz
  if [[ ! -f $T ]]; then
    curl -fL --progress-bar -o $T.part https://github.com/metalsharp/MetalSharp-Wine/releases/download/$RELEASE/$NAME.tar.xz
    mv $T.part $T
  fi
  [[ $(shasum -a 256 $T | cut -d' ' -f1) == $SHA ]] || { echo "checksum mismatch: $T" >&2; exit 1; }
  rm -rf $R; mkdir -p $R
  tar -C $R -xJf $T $NAME/share/metalsharp
  mv $R/$NAME/share/metalsharp $R/metalsharp; rm -rf $R/$NAME
  echo $RELEASE > $R/RELEASE
fi
chmod -R u+w $R/metalsharp
echo "metalsharp assets: $R/metalsharp ($(du -sh $R/metalsharp | cut -f1))"
