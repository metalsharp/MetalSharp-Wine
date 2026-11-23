#!/bin/zsh
# WineMetalGL (OpenGL 1.0-4.6 on Metal) for this arm64 Wine: the arm64 core plus the Wine module
# built for arm64 hosts (src/winemetalgl, branch arm64-wine of metalsharp/WineMetalGL). Installs, as Wine builtins
# (as Mullion's DLLs are), so a prefix's System32 and SysWOW64 get them:
#   install/lib/wine/{aarch64,i386}-windows/winemetalgl.dll    WGL and GL entry points
#   install/lib/wine/{aarch64,i386}-windows/opengl32.dll       its forwarder to winemetalgl.dll
#   install/lib/wine/aarch64-unix/{winemetalgl.so,libwinemetalgl.dylib}
# Wine's own opengl32.dll moves to install/lib/wine-opengl/{aarch64,i386}-windows (its opengl32.so stays): ntdll
# loads it for OPENGL_BACKEND=wine or WINEMETALGL=0. Run it after build-wine.sh install, which puts Wine's back.
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
W=$I/lib/wine-opengl
rm -rf $I/lib/winemetalgl   # the old layout's forwarders
mkdir -p $W/aarch64-windows $W/i386-windows
for d in aarch64-windows i386-windows; do
  # The loader serves the forwarder as the builtin opengl32, and only accepts modules on its builtin path that carry
  # the Wine builtin signature.
  python3 - $M/$d/opengl32.dll <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
  # Wine's opengl32 is whatever lib/wine has that is not the forwarder (build-wine.sh install just put it there)
  if ! cmp -s $M/$d/opengl32.dll $I/lib/wine/$d/opengl32.dll; then
    [[ -f $I/lib/wine/$d/opengl32.dll ]] || { echo "build-winemetalgl: no Wine opengl32.dll in lib/wine/$d (run build-wine.sh install)" >&2; exit 1; }
    mv -f $I/lib/wine/$d/opengl32.dll $W/$d/opengl32.dll
  fi
  [[ -f $W/$d/opengl32.dll ]] || { echo "build-winemetalgl: lib/wine-opengl/$d has no Wine opengl32.dll (run build-wine.sh install)" >&2; exit 1; }
  cp $M/$d/opengl32.dll $I/lib/wine/$d/opengl32.dll
done
mkdir -p $I/share/licenses/winemetalgl
cp $G/LICENSE $I/share/licenses/winemetalgl/LICENSE
echo "WineMetalGL $(git -C $G describe --tags --always --dirty)" > $I/share/licenses/winemetalgl/VERSION
echo "winemetalgl: $(cat $I/share/licenses/winemetalgl/VERSION)"
