#!/bin/zsh
# A signed Mullion NGX core (nvngx.dll) for build-mullion.sh, which NVIDIA's NGX needs for DLSS on MetalFX: without
# one build-mullion.sh stops, as an unsigned core leaves every game without DLSS. Mullion's sign-ngx workflow signs
# cores with MetalSharp's key, into draft releases of aaf2tbz/mullion tagged ngx-signing-<mullion commit>. This takes
# the newest one whose commit builds the same nvngx.dll as src/mullion's (src/nvngx and what it links unchanged),
# checks its signature against src/mullion's certificate, and prints its folder, for MULLION_SIGNED_NGX_DIR:
#   export MULLION_SIGNED_NGX_DIR=$(zsh scripts/fetch-signed-ngx.sh)
# The drafts are visible to the repository's collaborators only (gh, signed in). When none matches, sign one: upload
# src/mullion's build-arm64ec/src/nvngx/nvngx.dll as unsigned-arm64ec-nvngx.dll to a new draft release
# ngx-signing-<commit> of aaf2tbz/mullion and run the workflow with that tag (gh workflow run sign-ngx.yml
# --repo aaf2tbz/mullion -f tag=ngx-signing-<commit>), or sign it here with MULLION_NGX_SIGNING_KEY_FILE.
set -e
source ${0:A:h}/env.sh
M=$ROOT/src/mullion
REPO=aaf2tbz/mullion
CERT=$M/src/nvngx/signing/mullion-ngx.pem
NGX_SOURCES=(src/nvngx src/util include meson.build build-arm64ec.txt)
command -v gh >/dev/null || { echo "fetch-signed-ngx: needs gh (brew install gh; gh auth login)" >&2; exit 1; }
command -v osslsigncode >/dev/null || { echo "fetch-signed-ngx: brew install osslsigncode" >&2; exit 1; }
for tag in $(gh release list --repo $REPO --limit 50 --json tagName,createdAt \
               -q 'sort_by(.createdAt) | reverse | .[].tagName | select(startswith("ngx-signing-"))'); do
  c=${tag#ngx-signing-}
  git -C $M cat-file -e "$c^{commit}" 2>/dev/null || git -C $M fetch -q origin $c 2>/dev/null || continue
  git -C $M cat-file -e "$c^{commit}" 2>/dev/null || continue
  git -C $M diff --quiet $c HEAD -- $NGX_SOURCES || continue
  D=$ROOT/downloads/ngx/$c
  if [[ ! -f $D/aarch64-windows/nvngx.dll ]]; then
    rm -rf $D; mkdir -p $D/aarch64-windows
    gh release download $tag --repo $REPO -p arm64ec-nvngx.dll -D $D >/dev/null 2>&1 || { rm -rf $D; continue; }
    mv $D/arm64ec-nvngx.dll $D/aarch64-windows/nvngx.dll
  fi
  osslsigncode verify -CAfile $CERT -in $D/aarch64-windows/nvngx.dll >/dev/null 2>&1 ||
    { echo "fetch-signed-ngx: $tag's arm64ec-nvngx.dll is not signed with $CERT" >&2; rm -rf $D; continue; }
  echo "fetch-signed-ngx: $tag (nvngx unchanged since $c)" >&2
  echo $D
  exit 0
done
echo "fetch-signed-ngx: no signed NGX core in $REPO's ngx-signing-* releases for src/mullion's nvngx ($(git -C $M rev-parse --short HEAD)); see this script to sign one" >&2
exit 1
