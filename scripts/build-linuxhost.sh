#!/bin/zsh
# Build the Linux module host (src/linuxhost: linuxhost.dll, x86_64 Windows code that FEX runs) and install it as
# install/lib/linuxhost. A program that loads Linux code into itself, as Easy Anti-Cheat's EOS bootstrapper loads
# EAC's Linux module under Proton, gets the host from ntdll (load_linux_host), which defaults LINUXHOST_RUNTIME_DIR
# to lib/linuxhost.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compiler
H=$ROOT/src/linuxhost
R=$ROOT/install/lib/linuxhost
B=$ROOT/build/linuxhost
[[ -f $H/build.sh ]] || { echo >&2 "!! src/linuxhost is not checked out: zsh scripts/fetch-sources.sh"; exit 1; }
# build.sh also builds the host's test program and test libraries, into the same directory
rm -rf $B; bash $H/build.sh $MINGW $B
rm -rf $R; mkdir -p $R/x86_64-windows
cp $B/linuxhost.dll $R/x86_64-windows/
echo "Linux module host $(git -C $H rev-parse --short HEAD)" > $R/VERSION
echo "linuxhost: $R ($(cat $R/VERSION))"
