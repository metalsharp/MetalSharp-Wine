#!/bin/zsh
# Build WFDXCompat's Rockstar launcher companion (src/wfdxcompat, wfdx-launchers-v1.dll) as ARM64EC
# and install it as install/lib/wfdxcompat. ntdll loads it into Launcher.exe and SocialClubHelper.exe
# when they run on Mullion (dlls/ntdll/wfdxcompat_loader.c), and defaults WFDXCOMPAT_RUNTIME_DIR to
# lib/wfdxcompat.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC  # macOS flags would reach the PE cross compiler
W=$ROOT/src/wfdxcompat
R=$ROOT/install/lib/wfdxcompat
TOOLCHAIN=$MINGW WINEBUILD=$ROOT/src/wine/tools/winebuild/winebuild sh $W/build-arm64ec.sh
rm -rf $R; mkdir -p $R/aarch64-windows
cp $W/lib/wfdxcompat/aarch64-windows/wfdx-launchers-v1.dll $R/aarch64-windows/
cp $W/LICENSE $R/LICENSE
echo "WFDXCompat $(git -C $W rev-parse --short HEAD)" > $R/VERSION
echo "wfdxcompat: $R ($(cat $R/VERSION))"
