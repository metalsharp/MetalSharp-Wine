#!/bin/zsh
# Full release validation from committed sources: build, package, audit, conformance tests
# (native ARM64 and x86_64 through FEX), benchmarks, and regenerated reports.
set -e
S=${0:A:h}
source $S/env.sh
D=$ROOT/dist/metalsharp-wine-2.0-arm64
NATIVE=(ntdll kernelbase kernel32 advapi32 msvcrt ucrtbase ws2_32 gdi32 d3d9 d3d11 dxgi opengl32 vulkan-1
        winmm mmdevapi dsound mfplat mf bcrypt secur32 crypt32 winspool.drv user32 ole32 comctl32 dinput oleaut32 shell32 shlwapi)
X64=(ntdll kernelbase kernel32 msvcrt ucrtbase msvcp140 advapi32 bcrypt crypt32 ws2_32 gdi32 user32 ole32 oleaut32 shlwapi shell32)

echo "== build"
zsh $S/build-wine.sh build; zsh $S/build-wine.sh install; zsh $S/build-fex.sh; zsh $S/build-tests.sh
echo "== package + audit"
zsh $S/package.sh
zsh $S/audit-macho.sh $ROOT/deps/prefix $ROOT/install
STRICT=1 zsh $S/audit-macho.sh $D
echo "== conformance (native ARM64)"
rm -rf $ROOT/prefixes/winetest $ROOT/logs/winetest/aarch64
set +e
WINE_DIST=$D zsh $S/run-winetest.sh aarch64 $NATIVE > $ROOT/logs/winetest-aarch64-final.log 2>&1
echo "== conformance (x86_64 via FEX)"
rm -rf $ROOT/prefixes/winetest-x64 $ROOT/logs/winetest/x86_64
WINE_DIST=$D TEST_TREE=$ROOT/build/x64tests WINETEST_PREFIX=$ROOT/prefixes/winetest-x64 \
  zsh $S/run-winetest.sh x86_64 $X64 > $ROOT/logs/winetest-x86_64-final.log 2>&1
set -e
echo "== benchmarks"
rm -rf $ROOT/prefixes/bench
zsh $S/run-bench.sh $D > /dev/null
python3 $S/summarize-tests.py $ROOT > /dev/null
echo "validate-release: done"
