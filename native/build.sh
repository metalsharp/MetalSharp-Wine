#!/bin/sh
# Build the native Linux module host's test program: build.sh <output dir>
set -e
src=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh <output dir>}
mkdir -p "$out"
# The SDK version 12.0 in the binary keeps x18 across context switches: Linux code uses it as a scratch register,
# and macOS clears it for programs built against later SDKs (Wine's loader is linked the same way).
clang -arch arm64 -mmacosx-version-min=15.0 -Wl,-platform_version,macos,15.0,12.0 -O1 -g -Wall -Wno-unused-function \
  -o "$out/eoshost" "$src/eoshost.c" "$src/elfload.c" "$src/tlsdesc.S" "$src/glibc.c" "$src/glibc_thread.c" \
  "$src/glibc_signal.c" "$src/glibc_stdio.c" "$src/glibc_net.c" "$src/glibc_locale.c" "$src/glibc_dl.c" \
  "$src/glibc_proc.c" "$src/glibc_asm.S"
