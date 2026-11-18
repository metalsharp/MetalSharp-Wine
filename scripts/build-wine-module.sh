#!/bin/sh
# Builds the WineMetalGL Wine module (src/wine): winemetalgl.dll and
# opengl32.dll for x86_64 and i386 Windows programs, and winemetalgl.so, the
# unix library the 64-bit Wine process runs for both.
#
#   scripts/build-wine-module.sh WINE_INSTALL [OUTDIR]
#
# WINE_INSTALL is any Wine 11.x installation (its include/wine headers) or a
# Wine 11.x source tree (its include directory); Wine itself is not rebuilt
# or changed. The PE side needs llvm-mingw
# (x86_64-w64-mingw32-clang, i686-w64-mingw32-clang) on PATH. The core
# (libwinemetalgl.dylib) is built by the CMake release preset.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WINE=${1:?usage: build-wine-module.sh WINE_INSTALL [OUTDIR]}
OUT=${2:-$ROOT/build/wine-module}
GEN=$OUT/gen
OBJ=$OUT/obj
mkdir -p "$GEN" "$OBJ/x86_64" "$OBJ/i386" "$OBJ/unix" "$OUT/x86_64-windows" "$OUT/i386-windows" "$OUT/x86_64-unix"

python3 "$ROOT/tools/gen/wine_gen.py" --registry "$ROOT/third_party/khronos/gl.xml" \
    --extensions "$ROOT/tools/gen/extensions.txt" --out "$GEN" >/dev/null

# ---------------------------------------------------------------- PE side
for arch in x86_64 i386; do
    case $arch in x86_64) cc=x86_64-w64-mingw32-clang ;; i386) cc=i686-w64-mingw32-clang ;; esac
    objs=
    for src in "$ROOT"/src/wine/pe/*.c "$GEN/wine_gl_pe.gen.c"; do
        obj="$OBJ/$arch/$(basename "${src%.c}").o"
        "$cc" -c -O2 -Wall -Werror -I"$ROOT/src/wine/include" -I"$ROOT/src/wine/pe" -I"$GEN" "$src" -o "$obj"
        objs="$objs $obj"
    done
    # shellcheck disable=SC2086
    "$cc" -shared -o "$OUT/$arch-windows/winemetalgl.dll" $objs "$GEN/winemetalgl.def" \
        -lgdi32 -luser32 -lkernel32 -lntdll -Wl,--kill-at
    printf 'BOOL __stdcall DllMain(void *i, unsigned int r, void *p) { return 1; }\n' |
        sed 's/BOOL/int/' > "$OBJ/$arch/forwarder.c"
    "$cc" -c -O2 "$OBJ/$arch/forwarder.c" -o "$OBJ/$arch/forwarder.o"
    # a version resource, as Windows' opengl32.dll has: programs read it to see that OpenGL is installed
    "${cc%clang}windres" -O res "$ROOT/src/wine/rc/opengl32.rc" -o "$OBJ/$arch/opengl32.res"
    "$cc" -shared -nostdlib -o "$OUT/$arch-windows/opengl32.dll" "$OBJ/$arch/forwarder.o" "$OBJ/$arch/opengl32.res" "$GEN/opengl32.def" \
        -Wl,--entry,DllMain$( [ $arch = i386 ] && echo @12 ) -lkernel32
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
# Apple's clang: llvm-mingw's plain clang may come first on PATH.
UNIX_CC=${UNIX_CC:-/usr/bin/clang}
CFLAGS="-arch x86_64 -mmacosx-version-min=15.0 -O2 -fPIC -Wall -DWINE_UNIX_LIB -D__WINESRC__ -D_WIN64"
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
"$UNIX_CC" -arch x86_64 -mmacosx-version-min=15.0 -O2 -fPIC -fobjc-arc -Wall -c "$ROOT/src/wine/unix/unix_window.m" \
    -o "$OBJ/unix/unix_window.o"
# shellcheck disable=SC2086
"$UNIX_CC" -arch x86_64 -mmacosx-version-min=15.0 -shared -o "$OUT/x86_64-unix/winemetalgl.so" $uobjs "$OBJ/unix/unix_window.o" \
    -framework AppKit -framework Metal -framework QuartzCore -Wl,-undefined,dynamic_lookup
echo "Built: $OUT/{x86_64,i386}-windows/{winemetalgl,opengl32}.dll, $OUT/x86_64-unix/winemetalgl.so"

# x87 acceleration for 32-bit programs (unix_x87.c): its helper goes next to
# winemetalgl.so. Without it the module works, and x87 code stays slow.
"$ROOT/scripts/build-x87sidecar.sh" "$OUT" ||
    echo "warning: x87sidecar was not built (scripts/build-x87sidecar.sh); WINEMETALGL_X87 will have no helper" >&2
