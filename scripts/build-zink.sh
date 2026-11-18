#!/bin/zsh
# Zink, Mesa's OpenGL on Vulkan, as the alternative to WineMetalGL: with SharpGL_Switch=1 ntdll serves its
# opengl32.dll and libgallium_wgl.dll from lib/zink instead of WineMetalGL's forwarder. It runs on Wine's Vulkan, so on
# MoltenVK, or on KosmicKrisp with SharpVK_Switch=1. Installs
#   install/lib/zink/aarch64-windows/{opengl32,libgallium_wgl}.dll   ARM64EC, for FEX's x86_64 programs
#   install/lib/zink/i386-windows/{opengl32,libgallium_wgl}.dll      for 32-bit programs
# or, with the argument x86_64, for MetalSharp-Wine-x86_64.app (package-x86_64.sh ZINK=install-x86_64/zink):
#   install-x86_64/zink/{x86_64,i386}-windows/{opengl32,libgallium_wgl}.dll
# Built from the Mesa scripts/build-kosmickrisp.sh fetches (run it first) with llvm-mingw: Windows' GDI/WGL frontend,
# the Zink driver, no LLVM (no llvmpipe). Vulkan is loaded at run time (vulkan-1.dll).
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
M=$ROOT/src/mesa
W=$ROOT/build/zink
I=$ROOT/install; O_ROOT=$I/lib/zink
lanes=(i386:i686:x86:i386-windows aarch64:arm64ec:aarch64:aarch64-windows)
if [[ $1 == x86_64 ]]; then
  O_ROOT=$ROOT/install-x86_64/zink
  lanes=(i386:i686:x86:i386-windows x86_64:x86_64:x86_64:x86_64-windows)
fi
V=$ROOT/build/kosmickrisp/venv
[[ -f $M/src/gallium/drivers/zink/meson.build && -x $V/bin/python3 ]] || { echo "build-zink: run build-kosmickrisp.sh first (Mesa and its Python)" >&2; exit 1; }
export PATH=$V/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH
unset PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
mkdir -p $W

for lane in $lanes; do
  name=${lane%%:*}; rest=${lane#*:}; triple=${rest%%:*}; rest=${rest#*:}; family=${rest%%:*}; dir=${rest#*:}
  cross=$W/cross-$name.ini
  cat > $cross <<EOF
[binaries]
c = '$MINGW/bin/$triple-w64-mingw32-clang'
cpp = '$MINGW/bin/$triple-w64-mingw32-clang++'
ar = '$MINGW/bin/llvm-ar'
strip = '$MINGW/bin/llvm-strip'
windres = '$MINGW/bin/$triple-w64-mingw32-windres'
dlltool = '$MINGW/bin/llvm-dlltool'
pkg-config = 'false'

[host_machine]
system = 'windows'
cpu_family = '$family'
cpu = '$triple'
endian = 'little'
EOF
  B=$W/build-$name
  [[ -f $B/build.ninja ]] || meson setup $B $M --cross-file $cross --buildtype=release -Db_ndebug=true \
      -Dplatforms=windows -Dgallium-drivers=zink -Dvulkan-drivers= -Dopengl=true -Degl=disabled \
      -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dzstd=disabled -Dmicrosoft-clc=disabled \
      -Dzlib:default_library=static -Dprefix=$W/prefix-$name >/dev/null
  ninja -C $B >/dev/null
  ninja -C $B install >/dev/null
  O=$O_ROOT/$dir
  rm -rf $O && mkdir -p $O
  cp $W/prefix-$name/bin/opengl32.dll $W/prefix-$name/bin/libgallium_wgl.dll $O/
  # ntdll serves them from its builtin path, which takes only modules carrying the Wine builtin signature.
  for f in $O/*.dll; do
    python3 - $f <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96
    f.seek(64)
    f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY
  done
  if $MINGW/bin/llvm-objdump -p $O/libgallium_wgl.dll | grep "DLL Name" | grep -viE 'api-ms-win|kernel32|user32|gdi32|advapi32|ntdll|msvcrt|ucrtbase'; then
    echo "build-zink: $O/libgallium_wgl.dll imports a DLL Wine does not have (above)" >&2; exit 1
  fi
done
mkdir -p $I/share/licenses/mesa && cp $M/docs/license.rst $I/share/licenses/mesa/ 2>/dev/null || true
echo "zink: $O_ROOT (Mesa $(cat $M/VERSION))"
