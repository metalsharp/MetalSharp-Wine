#!/bin/zsh
# The VKD3D lane's own NGX core and NVAPI (nvngx.dll, nvapi64.dll: DLSS on MetalFX, ARM64X) for stage-vkd3d-proton.sh.
# NGX loads only a core signed with the Mullion NGX key, which MetalVulkan's release workflow holds, so this takes
# them from the newest MetalVulkan release whose ngx/ is the same as src/metalvulkan's, from its
# vkd3d-proton-<tag>-arm64 asset, checks nvngx.dll's signature against src/metalvulkan's certificate, and prints
# their folder:
#   zsh scripts/fetch-metalvulkan-ngx.sh
# When no release matches (ngx/ changed since the last one), release MetalVulkan, or build and sign them in
# src/metalvulkan with its scripts/build-ngx-arm64x.sh and NGX_SIGNING_KEY_FILE.
set -e
source ${0:A:h}/env.sh
WS=$ROOT/src/metalvulkan
REPO=metalsharp/MetalVulkan
CERT=$WS/ngx/signing/mullion-ngx.pem
command -v gh >/dev/null || { echo "fetch-metalvulkan-ngx: needs gh (brew install gh; gh auth login)" >&2; exit 1; }
command -v osslsigncode >/dev/null || { echo "fetch-metalvulkan-ngx: brew install osslsigncode" >&2; exit 1; }
for tag in $(gh release list --repo $REPO --limit 30 --exclude-drafts --json tagName,createdAt \
               -q 'sort_by(.createdAt) | reverse | .[].tagName'); do
  git -C $WS cat-file -e "$tag^{commit}" 2>/dev/null || git -C $WS fetch -q origin tag $tag 2>/dev/null || continue
  git -C $WS diff --quiet $tag HEAD -- ngx || continue
  D=$ROOT/downloads/metalvulkan-ngx/$tag
  if [[ ! -f $D/nvngx.dll || ! -f $D/nvapi64.dll ]]; then
    rm -rf $D; mkdir -p $D/asset
    gh release download $tag --repo $REPO -p "vkd3d-proton-$tag-arm64.tar.zst" -D $D/asset >/dev/null 2>&1 ||
      { rm -rf $D; continue; }
    tar -C $D/asset -xf $D/asset/vkd3d-proton-$tag-arm64.tar.zst
    cp $D/asset/vkd3d-proton-$tag-arm64/aarch64-windows/{nvngx,nvapi64}.dll $D/ 2>/dev/null || { rm -rf $D; continue; }
    rm -rf $D/asset
  fi
  osslsigncode verify -CAfile $CERT -in $D/nvngx.dll >/dev/null 2>&1 ||
    { echo "fetch-metalvulkan-ngx: $tag's nvngx.dll is not signed with $CERT" >&2; rm -rf $D; continue; }
  echo "fetch-metalvulkan-ngx: $tag (ngx/ unchanged since it)" >&2
  echo $D
  exit 0
done
echo "fetch-metalvulkan-ngx: no MetalVulkan release with a signed NGX core for src/metalvulkan's ngx/ ($(git -C $WS rev-parse --short HEAD)); see this script" >&2
exit 1
