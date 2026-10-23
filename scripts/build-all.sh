#!/bin/zsh
# Full from-scratch build: dependencies, ARM64X runtimes, Wine, FEX, tests, audit, package.
set -e
S=${0:A:h}
zsh $S/build-deps.sh
[[ -e ${0:A:h:h}/deps/stamps/arm64ec-runtimes ]] || { zsh $S/build-arm64ec-runtimes.sh && touch ${0:A:h:h}/deps/stamps/arm64ec-runtimes; }
zsh $S/build-wine.sh
zsh $S/build-fex.sh
zsh $S/build-tests.sh
source $S/env.sh
zsh $S/audit-macho.sh $ROOT/deps/prefix $ROOT/install
zsh $S/package.sh
STRICT=1 zsh $S/audit-macho.sh $ROOT/dist
echo "build-all: ok"
