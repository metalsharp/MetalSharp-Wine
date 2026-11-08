#!/bin/zsh
# WineMetalGL (OpenGL 1.0-4.6 on Metal) for the x86_64 MetalSharp Wine (Wine 11.17 under Rosetta, MetalSharp's
# regular build), from the same source as build-winemetalgl.sh: the x86_64 core and the Wine module for x86_64 hosts.
# Installs install-x86_64/winemetalgl/
#   wine/{x86_64,i386}-windows/winemetalgl.dll    WGL and GL entry points (builtin)
#   wine/x86_64-unix/{winemetalgl.so,libwinemetalgl.dylib}
#   lib/{x86_64,i386}-windows/opengl32.dll        forwarders, Wine builtins: the x86_64 Wine's ntdll serves them for
#                                                 opengl32.dll from lib/winemetalgl (OPENGL_BACKEND=wine or
#                                                 WINEMETALGL=0 selects Wine's own)
#   X86_WINE_SRC=<the x86_64 Wine's source> build-winemetalgl-x86_64.sh
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
G=$ROOT/src/winemetalgl
W=${X86_WINE_SRC:-$ROOT/src/wine-x86_64}
OUT=$ROOT/install-x86_64/winemetalgl
export PATH=$ROOT/toolchain/llvm-mingw-20261020-ucrt-macos-universal/bin:$PATH

cmake -S $G -B $G/build/x86_64-release -G Ninja -DWMGL_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ >/dev/null
cmake --build $G/build/x86_64-release --target winemetalgl_core -j$JOBS >/dev/null
sh $G/scripts/build-wine-module.sh $W $G/build/wine-module-x86_64 >/dev/null
M=$G/build/wine-module-x86_64

rm -rf $OUT; mkdir -p $OUT/wine/x86_64-windows $OUT/wine/i386-windows $OUT/wine/x86_64-unix $OUT/lib/x86_64-windows $OUT/lib/i386-windows
cp $M/x86_64-windows/winemetalgl.dll $OUT/wine/x86_64-windows/
cp $M/i386-windows/winemetalgl.dll $OUT/wine/i386-windows/
cp $M/x86_64-unix/winemetalgl.so $OUT/wine/x86_64-unix/
cp $(find $G/build/x86_64-release -name libwinemetalgl.dylib | head -1) $OUT/wine/x86_64-unix/
# the module finds its core next to it, wherever Wine is
install_name_tool -id @rpath/libwinemetalgl.dylib $OUT/wine/x86_64-unix/libwinemetalgl.dylib 2>/dev/null
old=$(otool -L $OUT/wine/x86_64-unix/winemetalgl.so | awk 'NR>1 && /winemetalgl\.so|libwinemetalgl/ {print $1}' | grep -v "^@" | head -5)
for o in ${(f)old}; do install_name_tool -change $o @loader_path/libwinemetalgl.dylib $OUT/wine/x86_64-unix/winemetalgl.so; done
install_name_tool -id @rpath/winemetalgl.so $OUT/wine/x86_64-unix/winemetalgl.so 2>/dev/null
otool -L $OUT/wine/x86_64-unix/winemetalgl.so | grep -i metalgl
cp $M/x86_64-windows/opengl32.dll $OUT/lib/x86_64-windows/
cp $M/i386-windows/opengl32.dll $OUT/lib/i386-windows/
for f in $OUT/lib/*-windows/opengl32.dll; do
  python3 - $f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
cp $G/LICENSE $OUT/lib/LICENSE
echo "WineMetalGL $(git -C $G describe --tags --always --dirty) (x86_64)" > $OUT/lib/VERSION
file $OUT/wine/*/* $OUT/lib/*-windows/*
echo "winemetalgl x86_64: $(cat $OUT/lib/VERSION)"
