#!/bin/zsh
# Static arm64 LLVM 15.0.7 libraries for DXMT's shader converter (airconv), which links the
# components of `llvm-config --libs bitwriter passes`. Installs into deps/llvm15.
set -e
source ${0:A:h}/env.sh
SRC=$ROOT/deps/src/llvm15/llvm-project-15.0.7.src
B=$ROOT/deps/build/llvm15
OUT=$ROOT/deps/llvm15

LIBS=(LLVMPasses LLVMTarget LLVMObjCARCOpts LLVMCoroutines LLVMipo LLVMInstrumentation LLVMVectorize
      LLVMLinker LLVMIRReader LLVMAsmParser LLVMFrontendOpenMP LLVMScalarOpts LLVMInstCombine
      LLVMAggressiveInstCombine LLVMTransformUtils LLVMBitWriter LLVMAnalysis LLVMProfileData LLVMSymbolize
      LLVMDebugInfoPDB LLVMDebugInfoMSF LLVMDebugInfoDWARF LLVMObject LLVMTextAPI LLVMMCParser LLVMMC
      LLVMDebugInfoCodeView LLVMBitReader LLVMCore LLVMRemarks LLVMBitstreamReader LLVMBinaryFormat
      LLVMSupport LLVMDemangle)

mkdir -p $B
cmake -G Ninja -S $SRC/llvm -B $B \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_INSTALL_PREFIX=$OUT \
  -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_ENABLE_PROJECTS= \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF -DBUILD_SHARED_LIBS=OFF
ninja -C $B -j$JOBS $LIBS

rm -rf $OUT; mkdir -p $OUT/lib $OUT/include
for l in $LIBS; do cp $B/lib/lib$l.a $OUT/lib/; done
cp -R $SRC/llvm/include/llvm $SRC/llvm/include/llvm-c $OUT/include/
cp -R $B/include/llvm/. $OUT/include/llvm/
echo "llvm15: $OUT ($(ls $OUT/lib | wc -l | tr -d ' ') libraries)"
