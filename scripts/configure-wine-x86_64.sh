#!/bin/zsh
# Configures an x86_64 build tree for src/wine-x86_64 (Wine 11.22 + WineForge + MetalSharp), for build-wine-x86_64.sh:
#
#   X86_WINE_BUILD=<new build dir> [X86_DEPS=<an x86_64 MetalSharp-Wine-x86_64.app/Contents>] zsh scripts/configure-wine-x86_64.sh
#
# The tree is configured for x86_64 (--enable-archs=i386,x86_64, clang -arch x86_64, minimum macOS 15.0); its PE side
# needs Homebrew's mingw-w64, bison and flex. The libraries it links and loads by name (gnutls, FreeType, SDL2,
# MoltenVK) are the x86_64 ones the app ships, from X86_DEPS (default: an installed MetalSharp-Wine-x86_64.app),
# with Homebrew's headers (SDL2's from the arm64 build's deps/prefix); GStreamer and FFmpeg are build-deps-x86_64.sh's
# (deps/prefix-x86_64). Kerberos, netapi and EGL are left out: the app has no x86_64 builds of them, and winemac draws
# OpenGL with CGL.
set -e
ROOT=${0:A:h:h}
B=${X86_WINE_BUILD:?set X86_WINE_BUILD to the build directory to configure}
S=$ROOT/src/wine-x86_64
X=${X86_DEPS:-$HOME/.metalsharp/runtime/MetalSharp-Wine-x86_64.app/Contents}
MEDIA=${MEDIA:-$ROOT/deps/prefix-x86_64}
BREW=${HOMEBREW_PREFIX:-/opt/homebrew}

[[ -f $S/configure ]] || { echo "no source at $S (zsh scripts/fetch-sources.sh x86_64)" >&2; exit 1; }
for f in $X/lib/wine/x86_64-unix/libgnutls.30.dylib $X/lib/libfreetype.6.dylib $X/lib/libSDL2-2.0.0.dylib \
         $X/lib/wine/x86_64-unix/libMoltenVK.dylib $MEDIA/lib/libgstreamer-1.0.dylib $ROOT/deps/prefix/include/SDL2/SDL.h; do
  [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done
whence -p x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc not found (brew install mingw-w64)" >&2; exit 1; }
export PATH=$BREW/opt/bison/bin:$BREW/opt/flex/bin:$PATH

# -lSDL2 and -lMoltenVK need unversioned names, which the app doesn't have for SDL2
L=$B/deps-link
mkdir -p $L
ln -sf $X/lib/libSDL2-2.0.0.dylib $L/libSDL2.dylib
ln -sf $X/lib/wine/x86_64-unix/libMoltenVK.dylib $L/libMoltenVK.dylib

cd $B
$S/configure --build=x86_64-apple-darwin --host=x86_64-apple-darwin --enable-archs=i386,x86_64 \
  --disable-tests --disable-winemenubuilder \
  --with-coreaudio --with-cups --with-ffmpeg --with-freetype --with-gettext --with-gnutls --with-gstreamer \
  --with-mingw --with-opencl --with-pcap --with-pthread --with-sdl --with-vulkan \
  --without-alsa --without-capi --without-dbus --without-gphoto --without-inotify --without-krb5 --without-netapi \
  --without-oss --without-pulse --without-sane --without-udev --without-usb --without-v4l2 --without-wayland --without-x \
  LDFLAGS="-L$L" \
  CC="clang -arch x86_64 -mmacosx-version-min=15.0" CXX="clang++ -arch x86_64 -mmacosx-version-min=15.0" \
  CPP="clang -arch x86_64 -mmacosx-version-min=15.0 -E" \
  GNUTLS_CFLAGS="-I$BREW/opt/gnutls/include" GNUTLS_LIBS="-L$X/lib/wine/x86_64-unix -lgnutls" \
  FREETYPE_CFLAGS="-I$BREW/opt/freetype/include/freetype2" FREETYPE_LIBS="-L$X/lib -Wl,-rpath,$X/lib -lfreetype" \
  FFMPEG_CFLAGS="-I$MEDIA/include" FFMPEG_LIBS="-L$MEDIA/lib -lavformat -lavcodec -lavutil" \
  GSTREAMER_CFLAGS="$(PKG_CONFIG_PATH=$MEDIA/lib/pkgconfig pkg-config --cflags gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0)" \
  GSTREAMER_LIBS="-L$MEDIA/lib -lgstvideo-1.0 -lgstaudio-1.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lintl" \
  SDL2_CFLAGS="-I$ROOT/deps/prefix/include/SDL2" SDL2_LIBS="-L$L -lSDL2" \
  > $B/configure.log 2>&1 || { tail -5 $B/configure.log >&2; exit 1; }
grep -E "SONAME_LIB(GNUTLS|FREETYPE|SDL2|MOLTENVK)" include/config.h
echo "configured $B from $S"
