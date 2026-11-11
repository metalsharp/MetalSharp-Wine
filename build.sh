#!/bin/bash
# Build the Linux module host: its test program, linuxhost.dll and a test library.
#   darwin/linuxhost/build.sh <llvm-mingw dir> <output dir>
# hosttest.exe is an x86_64 Windows program (FEX runs it under MetalSharp Wine), hello.so an x86_64
# Linux shared library built without a C library.
set -e
MINGW=${1:?usage: build.sh <llvm-mingw dir> <output dir>}
OUT=${2:?usage: build.sh <llvm-mingw dir> <output dir>}
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
"$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -Wall -mfsgsbase -DARRAY_SIZE'(a)=(sizeof(a)/sizeof((a)[0]))' \
    "$HERE/elfload.c" "$HERE/glibc.c" "$HERE/syscall.c" "$HERE/process.c" "$HERE/libc.c" "$HERE/libc_io.c" "$HERE/test/hosttest.c" \
    -lshell32 -o "$OUT/hosttest.exe"
# linuxhost.dll: the host for programs that load Linux code themselves (process.c)
"$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -Wall -shared -mfsgsbase -DARRAY_SIZE'(a)=(sizeof(a)/sizeof((a)[0]))' \
    "$HERE/elfload.c" "$HERE/glibc.c" "$HERE/syscall.c" "$HERE/process.c" "$HERE/libc.c" "$HERE/libc_io.c" "$HERE/dllmain.c" -lshell32 -o "$OUT/linuxhost.dll"
"$MINGW/bin/clang" --target=x86_64-unknown-linux-gnu -O2 -fPIC -shared -nostdlib -fuse-ld=lld \
    -ftls-model=global-dynamic -Wl,--hash-style=gnu "$HERE/test/hello.c" -o "$OUT/hello.so"
echo "built $OUT/hosttest.exe, $OUT/linuxhost.dll and $OUT/hello.so"
