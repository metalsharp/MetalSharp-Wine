#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLCHAIN=${TOOLCHAIN:-}
if [ -n "$TOOLCHAIN" ]; then
    CC="$TOOLCHAIN/bin/x86_64-w64-mingw32-clang"
    LLVM_READOBJ="$TOOLCHAIN/bin/llvm-readobj"
else
    CC=$(command -v x86_64-w64-mingw32-clang || true)
    LLVM_READOBJ=$(command -v llvm-readobj || true)
fi
if [ -z "$CC" ] || [ ! -x "$CC" ]; then
    echo "x86_64-w64-mingw32-clang is required; set TOOLCHAIN or add it to PATH" >&2
    exit 1
fi
if [ -z "$LLVM_READOBJ" ] || [ ! -x "$LLVM_READOBJ" ]; then
    echo "llvm-readobj is required; set TOOLCHAIN or add it to PATH" >&2
    exit 1
fi
WINEBUILD=${WINEBUILD:-}
if [ -z "$WINEBUILD" ]; then
    WINEBUILD=$(command -v winebuild || true)
    if [ -z "$WINEBUILD" ] && [ -x "$ROOT/../install-archs-i386_x86_64/bin/winebuild" ]; then
        WINEBUILD="$ROOT/../install-archs-i386_x86_64/bin/winebuild"
    fi
fi
if [ -z "$WINEBUILD" ] || [ ! -x "$WINEBUILD" ]; then
    echo "winebuild is required; set WINEBUILD or add it to PATH" >&2
    exit 1
fi
WINE=${WINE:-}
OUT="$ROOT/build/tests"
TEST_STAGE="$OUT/x86_64-windows"

mkdir -p "$OUT" "$TEST_STAGE"
"$ROOT/build.sh"
"$CC" -O1 -g -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/src/core/notifier_registry.c" "$ROOT/tests/notifier_registry.c" \
    -o "$OUT/notifier_registry.exe"
"$CC" -shared -O1 -Wall -Wextra -Werror "$ROOT/tests/fake_backend.c" "$ROOT/tests/fake_backend.def" \
    -o "$OUT/wfdxbackend-test-d3d12.dll"
"$CC" -shared -O1 -g -Wall -Wextra -Werror -I"$ROOT/include" \
    -DWFDX_BACKEND_MODULE='L"wfdxbackend-test-d3d12.dll"' \
    "$ROOT/src/frontend/module.c" "$ROOT/src/frontend/proxy.c" \
    "$ROOT/src/frontend/agility_shim.c" \
    "$ROOT/src/frontend/nv12_resource.c" "$ROOT/src/frontend/resource_notifier.c" \
    "$ROOT/src/frontend/command_list_proxy.c" \
    "$ROOT/src/core/notifier_registry.c" "$ROOT/build/x86_64-windows/d3d12.def" \
    -o "$OUT/d3d12.dll"
"$WINEBUILD" --builtin "$OUT/d3d12.dll"
cp -f "$ROOT/build/x86_64-windows/d3d12core.dll" "$OUT/d3d12core.dll"
cp -f "$OUT/d3d12.dll" "$OUT/d3d12core.dll" "$TEST_STAGE/"
cp -f "$OUT/wfdxbackend-test-d3d12.dll" \
    "$TEST_STAGE/wfdxbackend-test-d3d12.dll"
cp -f "$OUT/wfdxbackend-test-d3d12.dll" \
    "$TEST_STAGE/wfdxbackend-d3d12.dll"
"$CC" -O1 -Wall -Wextra -Werror -I"$ROOT/include" \
    -DWFDX_BACKEND_MODULE_A='"wfdxbackend-test-d3d12.dll"' "$ROOT/tests/forwarder_probe.c" \
    -o "$OUT/forwarder_probe.exe"

"$LLVM_READOBJ" --coff-exports "$OUT/d3d12.dll" | \
    awk '/Name: D3D12CreateDevice$/{seen_name=1} /ForwardedTo: wfdxbackend-d3d12.D3D12CreateDevice$/{bad=1} END{exit !(seen_name && !bad)}'
echo "pe_forwarder_table: PASS"
"$LLVM_READOBJ" --coff-exports "$ROOT/build/x86_64-windows/d3d12core.dll" | \
    awk '/ForwardedTo: wfdxbackend-d3d12.D3D12CoreCreateLayeredDevice$/{create=1} \
         /ForwardedTo: wfdxbackend-d3d12.D3D12CoreGetLayeredDeviceSize$/{size=1} \
         /ForwardedTo: wfdxbackend-d3d12.D3D12CoreRegisterLayers$/{register_layers=1} \
         END{exit !(create && size && register_layers)}'
echo "agility_core_forwarders: PASS"

if [ -n "$WINE" ]; then
    (cd "$OUT" && "$WINE" ./notifier_registry.exe)
    (cd "$OUT" && GRAPHICS_BACKEND=d3dmetal WFDXCOMPAT_RUNTIME_DIR="$OUT" \
        WINEDLLOVERRIDES=d3d12=n,b "$WINE" ./forwarder_probe.exe)
else
    echo "Windows runtime tests: SKIP (set WINE to an explicitly approved runner)"
fi
