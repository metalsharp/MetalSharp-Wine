#!/bin/zsh
# WineMetalGL (OpenGL 1.0-4.6 on Metal) for this arm64 Wine: the arm64 core plus the Wine module
# built for arm64 hosts (src/winemetalgl, branch arm64-wine of metalsharp/WineMetalGL). Installs
#   install/lib/wine/{aarch64,i386}-windows/winemetalgl.dll    WGL and GL entry points (builtin)
#   install/lib/wine/aarch64-unix/{winemetalgl.so,libwinemetalgl.dylib}
#   install/lib/winemetalgl/{aarch64,i386}-windows/opengl32.dll  forwarders for a prefix
# It is the default OpenGL: ntdll serves the forwarder for opengl32.dll from lib/winemetalgl.
# OPENGL_BACKEND=wine or WINEMETALGL=0 selects Wine's own opengl32.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
G=$ROOT/src/winemetalgl
I=$ROOT/install

cmake -S $G -B $G/build/arm64-release -G Ninja -DWMGL_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ >/dev/null
cmake --build $G/build/arm64-release --target winemetalgl_core -j$JOBS >/dev/null
sh $G/scripts/build-wine-module-arm64.sh $ROOT/src/wine $G/build/wine-module-arm64 >/dev/null
M=$G/build/wine-module-arm64

cp $M/aarch64-windows/winemetalgl.dll $I/lib/wine/aarch64-windows/
cp $M/i386-windows/winemetalgl.dll $I/lib/wine/i386-windows/
for f in winemetalgl.so libwinemetalgl.dylib; do rm -f $I/lib/wine/aarch64-unix/$f; cp $M/aarch64-unix/$f $I/lib/wine/aarch64-unix/; done
F=$I/lib/winemetalgl
rm -rf $F; mkdir -p $F/aarch64-windows $F/i386-windows
cp $M/aarch64-windows/opengl32.dll $F/aarch64-windows/
cp $M/i386-windows/opengl32.dll $F/i386-windows/
# The loader serves these as Wine's opengl32 (WineMetalGL is the default OpenGL), and only
# accepts modules on its builtin path that carry the Wine builtin signature.
for f in $F/*-windows/opengl32.dll; do
  python3 - $f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
done
cp $G/LICENSE $F/LICENSE
echo "WineMetalGL $(git -C $G describe --tags --always --dirty)" > $F/VERSION
echo "winemetalgl: $(cat $F/VERSION)"
