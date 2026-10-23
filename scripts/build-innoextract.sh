#!/bin/zsh
# innoextract (unpacks Inno Setup installers, e.g. GOG games, without running them) as a native
# arm64 tool, minos 15.0. Boost and liblzma are linked statically, zlib and bzip2 come from the
# SDK, so the binary only depends on system libraries. Installed as deps/innoextract/innoextract;
# package.sh copies it to bin/. Checksums match boost.io and the Homebrew formulas.
set -e
source ${0:A:h}/env.sh
DL=$ROOT/downloads/innoextract; W=$ROOT/deps/build/innoextract; P=$W/prefix; O=$ROOT/deps/innoextract
mkdir -p $DL $W
fetch() {  # fetch <url> <file> <sha256>
  [[ -f $DL/$2 ]] || curl -fsSL -o $DL/$2 $1
  [[ $(shasum -a 256 $DL/$2 | cut -d' ' -f1) == $3 ]] || { echo "checksum mismatch: $2" >&2; exit 1; }
}
IE=1.9 BOOST=1.83.0 XZ=5.8.4
fetch https://github.com/dscharrer/innoextract/releases/download/$IE/innoextract-$IE.tar.gz innoextract-$IE.tar.gz \
  6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126
fetch https://archives.boost.io/release/$BOOST/source/boost_${BOOST//./_}.tar.bz2 boost_${BOOST//./_}.tar.bz2 \
  6478edfe2f3305127cffe8caf73ea0176c53769f4bf1585be237eb30798c3b8e
fetch https://github.com/tukaani-project/xz/releases/download/v$XZ/xz-$XZ.tar.gz xz-$XZ.tar.gz \
  0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9

cd $W
[[ -f $P/lib/liblzma.a ]] || {
  rm -rf xz-$XZ; tar -xf $DL/xz-$XZ.tar.gz
  (cd xz-$XZ && ./configure --prefix=$P --disable-shared --enable-static --disable-xz --disable-xzdec \
     --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc --disable-nls && make -j$JOBS install) > $W/xz.log 2>&1
}
[[ -f $P/lib/libboost_iostreams.a ]] || {
  rm -rf boost_${BOOST//./_}; tar -xf $DL/boost_${BOOST//./_}.tar.bz2
  (cd boost_${BOOST//./_} && ./bootstrap.sh --with-libraries=iostreams,filesystem,date_time,system,program_options &&
   ./b2 -j$JOBS --prefix=$P link=static variant=release threading=multi architecture=arm address-model=64 \
     cflags="$CFLAGS" cxxflags="$CXXFLAGS" linkflags="-mmacosx-version-min=15.0" -s NO_LZMA=1 -s NO_ZSTD=1 install) > $W/boost.log 2>&1
}
rm -rf innoextract-$IE build; tar -xf $DL/innoextract-$IE.tar.gz
export CMAKE_POLICY_VERSION_MINIMUM=3.5  # innoextract 1.9 predates CMake 4; also needed by its version script
cmake -S innoextract-$IE -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_PREFIX_PATH=$P \
  -DBOOST_ROOT=$P -DBoost_USE_STATIC_LIBS=ON -DLZMA_USE_STATIC_LIBS=ON -DUSE_STATIC_LIBS=ON \
  -DLZMA_INCLUDE_DIR=$P/include -DLZMA_LIBRARY=$P/lib/liblzma.a -DUSE_ARC4=ON > $W/cmake.log 2>&1
make -C build -j$JOBS innoextract > $W/make.log 2>&1
rm -rf $O; mkdir -p $O
cp build/innoextract $O/; strip $O/innoextract; codesign -f -s - $O/innoextract 2>/dev/null
cp innoextract-$IE/LICENSE $O/LICENSE.innoextract; cp boost_${BOOST//./_}/LICENSE_1_0.txt $O/LICENSE.boost
cp xz-$XZ/COPYING.0BSD $O/LICENSE.liblzma
echo "innoextract $IE (Boost $BOOST, xz $XZ, static)" > $O/VERSION
otool -L $O/innoextract; $O/innoextract --version | head -1
