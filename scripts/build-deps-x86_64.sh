#!/bin/zsh
# The x86_64 MetalSharp Wine's media libraries (Wine 11.17 under Rosetta, MetalSharp's regular build): GStreamer for
# winegstreamer.so and FFmpeg for winedmo.so, as x86_64, minos 15.0, into deps/prefix-x86_64. They are the bundles'
# missing pieces: the shipped modules link @rpath/libgst*-1.0.0.dylib (GStreamer 1.26), @rpath/libglib-2.0.0.dylib
# and friends, a bare libintl.dylib, and @rpath/libav{format,codec}.61 / libavutil.59 (FFmpeg 7.1).
# Packaged into MetalSharp-Wine-x86_64.app's Contents/lib, lib/gstreamer-1.0 and libexec/gstreamer-1.0, where
# GStreamer finds its plugins relative to libgstreamer.
# Each package is stamped on success; rerun to resume. Usage: build-deps-x86_64.sh [pkg...]
set -e
source ${0:A:h}/env.sh
export PREFIX=$ROOT/deps/prefix-x86_64
DL=$ROOT/downloads; SRC=$ROOT/deps/src-x86_64; BLD=$ROOT/deps/build-x86_64; STAMP=$ROOT/deps/stamps-x86_64
mkdir -p $DL $SRC $BLD $STAMP $PREFIX $ROOT/logs

export CC="/usr/bin/clang -arch x86_64"
export CXX="/usr/bin/clang++ -arch x86_64"
export OBJC="/usr/bin/clang -arch x86_64"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib -mmacosx-version-min=15.0 -Wl,-headerpad_max_install_names"
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
export PATH=$PREFIX/bin:$PATH

GST=1.26.7
typeset -A URL
URL=(
  gettext     https://ftp.gnu.org/gnu/gettext/gettext-0.26.tar.xz
  pcre2       https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.46/pcre2-10.46.tar.bz2
  libffi      https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz
  glib        https://download.gnome.org/sources/glib/2.86/glib-2.86.0.tar.xz
  ffmpeg      https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz
  gstreamer   https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-$GST.tar.xz
  gstbase     https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-$GST.tar.xz
  gstgood     https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-$GST.tar.xz
  gstbad      https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST.tar.xz
  gstlibav    https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-$GST.tar.xz
)
ORDER=(syspc gettext pcre2 libffi glib ffmpeg gstreamer gstbase gstgood gstbad gstlibav)

fetch() {  # fetch <pkg> -> sets S to the extracted source dir
  local f=$DL/$1-${URL[$1]:t}
  [[ -s $f ]] || curl -fL --retry 3 -o $f ${URL[$1]}
  rm -rf $SRC/$1 && mkdir -p $SRC/$1 && tar -xf $f -C $SRC/$1
  S=$(echo $SRC/$1/*(/[1]))
  B=$BLD/$1; rm -rf $B; mkdir -p $B
}
ac() { (cd $B && $S/configure --prefix=$PREFIX --host=x86_64-apple-darwin --build=aarch64-apple-darwin \
          --disable-dependency-tracking "$@" && make -j$JOBS && make install) }
ms() {  # drop function probes newer than the deployment target from meson.build lists
  local f; for f in $(sed -n 's/^ac_cv_func_\(.*\)=no$/\1/p' $ROOT/scripts/config.site); do
    find $S -name meson.build -exec sed -i '' "/^ *'$f',\{0,1\} *\$/d" {} +
  done
  meson setup $B $S --prefix=$PREFIX --libdir=lib --buildtype=release -Ddefault_library=shared "$@" && ninja -C $B -j$JOBS && ninja -C $B install; }

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
    gettext)  CONFIG_SITE=$ROOT/scripts/config.site.gettext am_cv_func_iconv_works=yes ac --disable-static --disable-java --disable-csharp --disable-openmp --without-emacs --without-git \
                 --disable-curses --without-included-gettext --with-included-libunistring --with-included-libxml ;;
    pcre2)    ac --disable-static --enable-pcre2-16 --enable-pcre2-32 --enable-jit ;;
    libffi)   ac --disable-static --disable-docs ;;
    glib)     ms -Dtests=false -Dintrospection=disabled -Dman-pages=disabled -Ddocumentation=false -Dnls=enabled \
                 -Dlibelf=disabled -Dsysprof=disabled ;;
    ffmpeg)   (cd $B && $S/configure --prefix=$PREFIX --cc="$CC" --cxx="$CXX" --objcc="$OBJC" --arch=x86_64 \
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
  esac
}

pkgs=(${@:-$ORDER})
for p in $pkgs; do
  [[ -e $STAMP/$p && $# -eq 0 ]] && { echo "== $p: done"; continue; }
  echo "== $p: building"
  build $p > $ROOT/logs/dep-x86_64-$p.log 2>&1 || { echo "== $p: FAILED (logs/dep-x86_64-$p.log)"; tail -30 $ROOT/logs/dep-x86_64-$p.log; exit 1; }
  touch $STAMP/$p; echo "== $p: ok"
done
