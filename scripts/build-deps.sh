#!/bin/zsh
# Build Wine's host-side dependencies as native arm64, minos 15.0, into deps/prefix.
# Each package is stamped on success; rerun to resume. Usage: build-deps.sh [pkg...]
set -e
source ${0:A:h}/env.sh
DL=$ROOT/downloads; SRC=$ROOT/deps/src; BLD=$ROOT/deps/build; STAMP=$ROOT/deps/stamps
mkdir -p $DL $SRC $BLD $STAMP $PREFIX

GST=1.26.7
typeset -A URL
URL=(
  libpng      https://download.sourceforge.net/libpng/libpng-1.6.50.tar.xz
  freetype    https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz
  gettext     https://ftp.gnu.org/gnu/gettext/gettext-0.26.tar.xz
  gmp         https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
  nettle      https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz
  libunistring https://ftp.gnu.org/gnu/libunistring/libunistring-1.3.tar.xz
  libtasn1    https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz
  libidn2     https://ftp.gnu.org/gnu/libidn/libidn2-2.3.8.tar.gz
  gnutls      https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.10.tar.xz
  pcre2       https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.46/pcre2-10.46.tar.bz2
  libffi      https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz
  glib        https://download.gnome.org/sources/glib/2.86/glib-2.86.0.tar.xz
  ffmpeg      https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz
  gstreamer   https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-$GST.tar.xz
  gstbase     https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-$GST.tar.xz
  gstgood     https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-$GST.tar.xz
  gstbad      https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST.tar.xz
  gstlibav    https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-$GST.tar.xz
  sdl2        https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
  vkheaders   https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  vkloader    https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  moltenvk    https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
)
ORDER=(syspc libpng freetype gettext gmp nettle libunistring libtasn1 libidn2 gnutls pcre2 libffi glib
       ffmpeg gstreamer gstbase gstgood gstbad gstlibav sdl2 vkheaders vkloader moltenvk)

fetch() {  # fetch <pkg> -> sets S to the extracted source dir
  local f=$DL/$1-${URL[$1]:t}
  [[ -s $f ]] || curl -fL --retry 3 -o $f ${URL[$1]}
  rm -rf $SRC/$1 && mkdir -p $SRC/$1 && tar -xf $f -C $SRC/$1
  S=$(echo $SRC/$1/*(/[1]))
  B=$BLD/$1; rm -rf $B; mkdir -p $B
}
ac() { (cd $B && $S/configure --prefix=$PREFIX --disable-dependency-tracking "$@" && make -j$JOBS && make install) }
ms() {  # drop function probes newer than the deployment target from meson.build lists
  local f; for f in $(sed -n 's/^ac_cv_func_\(.*\)=no$/\1/p' $ROOT/scripts/config.site); do
    find $S -name meson.build -exec sed -i '' "/^ *'$f',\{0,1\} *\$/d" {} +
  done
  meson setup $B $S --prefix=$PREFIX --libdir=lib --buildtype=release -Ddefault_library=shared "$@" && ninja -C $B -j$JOBS && ninja -C $B install; }
cm() { cmake -S $S -B $B -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_INSTALL_NAME_DIR=$PREFIX/lib "$@" \
       && ninja -C $B -j$JOBS && ninja -C $B install; }

syspc() {  # pkg-config stubs for libraries the macOS SDK provides
  local d=$PREFIX/lib/pkgconfig; mkdir -p $d
  local -A libs; libs=(zlib "z 1.2.12" bzip2 "bz2 1.0.8" iconv "iconv 1.11" expat "expat 2.6.0" libxml-2.0 "xml2 2.9.13")
  for n in ${(k)libs}; do local v=(${=libs[$n]})
    printf 'Name: %s\nDescription: macOS SDK %s\nVersion: %s\nLibs: -l%s\nCflags:%s\n' $n $n $v[2] $v[1] \
      "$([[ $n == libxml-2.0 ]] && echo " -I$SDKROOT/usr/include/libxml2")" > $d/$n.pc
  done
}

build() {
  [[ $1 == syspc ]] && { syspc; return; }
  fetch $1
  case $1 in
    libpng)   ac --disable-static ;;
    freetype) ac --disable-static --with-png=yes --with-zlib=yes --with-bzip2=yes --with-harfbuzz=no --with-brotli=no ;;
    gettext)  CONFIG_SITE=$ROOT/scripts/config.site.gettext am_cv_func_iconv_works=yes ac --disable-static --disable-java --disable-csharp --disable-openmp --without-emacs --without-git \
                 --disable-curses --without-included-gettext --with-included-libunistring --with-included-libxml ;;
    gmp)      ac --disable-static --enable-cxx ;;
    nettle)   ac --disable-static --disable-documentation --disable-openssl ;;
    libunistring|libtasn1) ac --disable-static ;;
    libidn2)  ac --disable-static --disable-doc ;;
    gnutls)   ac --disable-static --without-p11-kit --disable-doc --disable-tests --disable-tools --disable-guile \
                 --with-default-trust-store-file=/etc/ssl/cert.pem --without-tpm --without-tpm2 --without-zstd --without-brotli ;;
    pcre2)    ac --disable-static --enable-pcre2-16 --enable-pcre2-32 --enable-jit ;;
    libffi)   ac --disable-static --disable-docs ;;
    glib)     ms -Dtests=false -Dintrospection=disabled -Dman-pages=disabled -Ddocumentation=false -Dnls=enabled \
                 -Dlibelf=disabled -Dsysprof=disabled ;;
    ffmpeg)   (cd $B && $S/configure --prefix=$PREFIX --cc="$CC" --cxx="$CXX" --objcc="$OBJC" --arch=aarch64 \
                 --extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS" --enable-shared --disable-static \
                 --disable-programs --disable-doc --disable-debug --enable-videotoolbox --enable-audiotoolbox \
                 --disable-securetransport --disable-xlib --disable-libxcb --disable-sdl2 --install-name-dir=$PREFIX/lib \
               && make -j$JOBS && make install) ;;
    gstreamer) ms -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled -Ddoc=disabled -Dbenchmarks=disabled \
                 -Dlibunwind=disabled -Dlibdw=disabled -Dbash-completion=disabled -Dptp-helper=disabled ;;
    gstbase)  ms -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled -Ddoc=disabled -Dorc=disabled \
                 -Dgl=disabled -Dx11=disabled -Dxvideo=disabled -Dxshm=disabled -Dalsa=disabled -Dcdparanoia=disabled \
                 -Dlibvisual=disabled -Dtremor=disabled -Dpango=disabled -Dogg=disabled -Dvorbis=disabled -Dtheora=disabled \
                 -Dopus=disabled -Dqt5=disabled -Ddrm=disabled -Dxi=disabled ;;
    gstgood)  ms -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dorc=disabled -Dauto_features=disabled \
                 -Disomp4=enabled -Dmatroska=enabled -Davi=enabled -Daudioparsers=enabled -Dwavparse=enabled \
                 -Dflv=enabled -Did3demux=enabled -Dicydemux=enabled -Dapetag=enabled -Dautodetect=enabled \
                 -Dosxaudio=enabled -Ddeinterlace=enabled -Dvideofilter=enabled -Daudiofx=enabled -Drtp=enabled ;;
    gstbad)   ms -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dorc=disabled -Dauto_features=disabled \
                 -Dvideoparsers=enabled -Daudiobuffersplit=enabled -Dmpegtsdemux=enabled \
                 -Dmpegdemux=enabled -Did3tag=enabled -Dasfmux=enabled -Dvideoframe_audiolevel=enabled ;;
    gstlibav) ms -Ddoc=disabled -Dtests=disabled ;;
    sdl2)     cm -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_X11=OFF ;;
    vkheaders) cm ;;
    vkloader) cm -DVULKAN_HEADERS_INSTALL_DIR=$PREFIX -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF ;;
    moltenvk) mkdir -p $PREFIX/lib $PREFIX/share/vulkan/icd.d
              cp $S/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib $PREFIX/lib/
              install_name_tool -id $PREFIX/lib/libMoltenVK.dylib $PREFIX/lib/libMoltenVK.dylib
              sed 's#"library_path": *"[^"]*"#"library_path": "../../../lib/libMoltenVK.dylib"#' \
                  $S/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json > $PREFIX/share/vulkan/icd.d/MoltenVK_icd.json ;;
  esac
}

pkgs=(${@:-$ORDER})
for p in $pkgs; do
  [[ -e $STAMP/$p && $# -eq 0 ]] && { echo "== $p: done"; continue; }
  echo "== $p: building"
  build $p > $ROOT/logs/dep-$p.log 2>&1 || { echo "== $p: FAILED (logs/dep-$p.log)"; tail -30 $ROOT/logs/dep-$p.log; exit 1; }
  touch $STAMP/$p; echo "== $p: ok"
done
