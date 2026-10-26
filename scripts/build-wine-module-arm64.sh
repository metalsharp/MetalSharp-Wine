#!/bin/sh
# Builds the WineMetalGL Wine module (src/wine) for a native arm64 Wine, such as MetalSharp's
# Wine with FEX: winemetalgl.dll and opengl32.dll as ARM64X (aarch64-windows: native ARM64
# programs and x86_64 ones, which run as ARM64EC) and as i386 (WoW64), and winemetalgl.so for
# the arm64 unix side, which runs all of them (32-bit calls through Wine's WoW64 thunks). The
# core is the arm64 one (scripts/stage-arm64-core.sh, build/arm64-release).
#
#   scripts/build-wine-module-arm64.sh WINE_INSTALL_OR_SOURCE [OUTDIR]
#
# Needs llvm-mingw (arm64ec-w64-mingw32-clang, i686-w64-mingw32-clang) on PATH.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WINE=${1:?usage: build-wine-module-arm64.sh WINE_INSTALL_OR_SOURCE [OUTDIR]}
OUT=${2:-$ROOT/build/wine-module-arm64}
GEN=$OUT/gen
OBJ=$OUT/obj
mkdir -p "$GEN" "$OBJ/aarch64" "$OBJ/i386" "$OBJ/unix" "$OUT/aarch64-windows" "$OUT/i386-windows" "$OUT/aarch64-unix"

python3 "$ROOT/tools/gen/wine_gen.py" --registry "$ROOT/third_party/khronos/gl.xml" \
    --extensions "$ROOT/tools/gen/extensions.txt" --out "$GEN" >/dev/null

# ---------------------------------------------------------------- PE side
for arch in aarch64 i386; do
    case $arch in
    aarch64) cc="arm64ec-w64-mingw32-clang -marm64x" ;;  # ARM64X: native ARM64 + ARM64EC halves
    i386)    cc=i686-w64-mingw32-clang ;;
    esac
    objs=
    for src in "$ROOT"/src/wine/pe/*.c "$GEN/wine_gl_pe.gen.c"; do
        obj="$OBJ/$arch/$(basename "${src%.c}").o"
        $cc -c -O2 -Wall -Werror -I"$ROOT/src/wine/include" -I"$ROOT/src/wine/pe" -I"$GEN" "$src" -o "$obj"
        objs="$objs $obj"
    done
    # shellcheck disable=SC2086
    $cc -shared -o "$OUT/$arch-windows/winemetalgl.dll" $objs "$GEN/winemetalgl.def" \
        -lgdi32 -luser32 -lkernel32 -lntdll $( [ $arch = i386 ] && echo -Wl,--kill-at )
    printf 'int __stdcall DllMain(void *i, unsigned int r, void *p) { return 1; }\n' > "$OBJ/$arch/forwarder.c"
    $cc -c -O2 "$OBJ/$arch/forwarder.c" -o "$OBJ/$arch/forwarder.o"
    if [ $arch = i386 ]; then
        $cc -shared -nostdlib -o "$OUT/$arch-windows/opengl32.dll" "$OBJ/$arch/forwarder.o" "$GEN/opengl32.def" \
            -Wl,--entry,DllMain@12 -lkernel32
    else
        # ARM64EC code needs the toolchain's EC runtime helpers, so no -nostdlib here
        $cc -shared -o "$OUT/$arch-windows/opengl32.dll" "$OBJ/$arch/forwarder.o" "$GEN/opengl32.def" \
            -Wl,--entry,DllMain -lkernel32
    fi
    # Wine loads winemetalgl.so for a builtin winemetalgl.dll only (winebuild --builtin).
    python3 - "$OUT/$arch-windows/winemetalgl.dll" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ"
    e_lfanew = struct.unpack_from("<I", header, 60)[0]
    assert e_lfanew >= 64 + 32, "no room for the Wine builtin signature"
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done

# -------------------------------------------------------------- unix side
UNIX_CC=${UNIX_CC:-/usr/bin/clang}
CFLAGS="-arch arm64 -mmacosx-version-min=15.0 -O2 -fPIC -Wall -DWINE_UNIX_LIB -D__WINESRC__ -D_WIN64"
if test -d "$WINE/include/wine/windows"; then WINEINC="-I$WINE/include/wine/windows -I$WINE/include"
elif test -f "$WINE/include/windef.h"; then WINEINC="-I$WINE/include"
else echo "no Wine headers in $WINE" >&2; exit 1; fi
INCS="-I$ROOT/src/wine/include -I$ROOT/src/wine/unix -I$GEN $WINEINC"
uobjs=
for src in "$ROOT"/src/wine/unix/*.c "$GEN/wine_gl_unix.gen.c"; do
    obj="$OBJ/unix/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$UNIX_CC" $CFLAGS $INCS -c "$src" -o "$obj"
    uobjs="$uobjs $obj"
done
"$UNIX_CC" -arch arm64 -mmacosx-version-min=15.0 -O2 -fPIC -fobjc-arc -Wall -c "$ROOT/src/wine/unix/unix_window.m" \
    -o "$OBJ/unix/unix_window.o"
# shellcheck disable=SC2086
"$UNIX_CC" -arch arm64 -mmacosx-version-min=15.0 -shared -o "$OUT/aarch64-unix/winemetalgl.so" $uobjs "$OBJ/unix/unix_window.o" \
    -framework AppKit -framework Metal -framework QuartzCore -Wl,-undefined,dynamic_lookup -install_name @rpath/winemetalgl.so
if test -f "$ROOT/build/arm64-release/libwinemetalgl.dylib"; then
    cp "$ROOT/build/arm64-release/libwinemetalgl.dylib" "$OUT/aarch64-unix/"
fi
echo "Built: $OUT/{aarch64,i386}-windows/{winemetalgl,opengl32}.dll, $OUT/aarch64-unix/winemetalgl.so"
