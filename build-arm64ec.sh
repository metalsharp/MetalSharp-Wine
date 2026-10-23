#!/bin/sh
# The D3D11 launcher companion as ARM64EC, for an arm64 Wine whose x86_64 programs run as
# ARM64EC (MetalSharp Wine with FEX). Its D3D11 backend there is Mullion, which implements
# D3D10 devices, context state and GDI surfaces itself, so Wine passes
# WFDX_LAUNCHER_FLAG_NATIVE_D3D10 and the companion only interposes device creation.
#   TOOLCHAIN=<llvm-mingw> WINEBUILD=<arm64 Wine's winebuild> ./build-arm64ec.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLCHAIN=${TOOLCHAIN:-}
CC=${TOOLCHAIN:+$TOOLCHAIN/bin/}arm64ec-w64-mingw32-clang
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "arm64ec-w64-mingw32-clang is required; set TOOLCHAIN or add it to PATH" >&2
    exit 1
fi
WINEBUILD=${WINEBUILD:-$(command -v winebuild || true)}
if [ -z "$WINEBUILD" ] || [ ! -x "$WINEBUILD" ]; then
    echo "winebuild is required to mark the companion as a Wine builtin" >&2
    exit 1
fi
WFDXCOMPAT_CFLAGS=${WFDXCOMPAT_CFLAGS:-}
OUT="$ROOT/build/arm64ec-windows"
STAGE="$ROOT/lib/wfdxcompat/aarch64-windows"

mkdir -p "$OUT" "$STAGE"
"$CC" -shared -O2 -Wall -Wextra -Werror -I"$ROOT/include" \
    $WFDXCOMPAT_CFLAGS \
    "$ROOT/src/frontend/module.c" "$ROOT/src/frontend/d3d11_proxy.c" \
    "$ROOT/src/launchers/d3d11_context_state.c" \
    "$ROOT/src/launchers/d3d11_gdi_surface.c" \
    "$ROOT/src/launchers/d3d10_bridge.c" \
    "$ROOT/exports/wfdx-launchers-v1.def" \
    -ldxguid -lgdi32 -o "$OUT/wfdx-launchers-v1.dll"
"$WINEBUILD" --builtin "$OUT/wfdx-launchers-v1.dll"
cp -f "$OUT/wfdx-launchers-v1.dll" "$STAGE/"
echo "staged: $STAGE/wfdx-launchers-v1.dll"
