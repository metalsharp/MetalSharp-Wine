#!/bin/sh
# Installs the WineMetalGL Wine module into a Wine 11.x installation, and optionally into a
# Wine prefix. It works out whether the Wine is an x86_64 Wine (under Rosetta) or a native
# arm64 one (such as MetalSharp's Wine with FEX) and installs the module built for it:
# scripts/build-wine-module.sh or scripts/build-wine-module-arm64.sh, or the matching release
# package.
#
#   scripts/install-wine-module.sh WINE_INSTALL [WINEPREFIX]
#
# WINE_INSTALL is the directory with bin/wine and lib/wine, or a Wine app (WineHQ's "Wine
# Stable.app", whose Wine is in Contents/Resources/wine). For CrossOver, use
# install-crossover.sh. MetalSharp-Wine.app already has WineMetalGL as its OpenGL, and is
# signed, so nothing is installed into it.
#
# Into WINE_INSTALL, without replacing any of Wine's files:
#   x86_64 Wine: lib/wine/x86_64-windows/winemetalgl.dll, lib/wine/i386-windows/winemetalgl.dll
#     lib/wine/x86_64-unix/winemetalgl.so, lib/wine/x86_64-unix/libwinemetalgl.dylib (the core)
#   arm64 Wine: lib/wine/aarch64-windows/winemetalgl.dll (ARM64X: native ARM64 programs and
#     x86_64 ones, which run as ARM64EC), lib/wine/i386-windows/winemetalgl.dll,
#     lib/wine/aarch64-unix/winemetalgl.so, lib/wine/aarch64-unix/libwinemetalgl.dylib
#   (a MetalSharp Wine folder also gets its lib/winemetalgl forwarders updated)
# Into WINEPREFIX (each prefix that should use WineMetalGL): opengl32.dll,
# which forwards to winemetalgl.dll, and winemetalgl.dll itself, in system32
# and syswow64, and the DLL override opengl32=native. Removing the override (or the file) gives the
# prefix Wine's own OpenGL back. A WINEPREFIX that does not exist yet is
# created (wineboot).
#
# From a checkout, MODULE (default build/wine-module, or build/wine-module-arm64 for an arm64
# Wine) and CORE (default build/release/libwinemetalgl.dylib, or build/arm64-release/) choose
# what is installed; from a release package (this script at its top), its module directory
# when it is for the same kind of Wine; and otherwise the matching release downloaded from
# GitHub (fetch_release). WINEMETALGL_ARCH=x86_64 or arm64 overrides the detection.
set -eu

# The release to install when neither a release package (module/ next to this
# script) nor a checkout's build is at hand (a script downloaded on its own,
# or WineMetalGL Settings without the release inside it): downloaded from
# GitHub, checked against the release's SHA256SUMS.txt, and kept in
# WINEMETALGL_CACHE (default ~/Library/Caches/WineMetalGL/releases).
# WINEMETALGL_VERSION picks a version (default: the latest release).
fetch_release() {
    repo=metalsharp/WineMetalGL
    version=${WINEMETALGL_VERSION:-}
    if test -z "$version"; then
        latest=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") ||
            { echo "cannot reach GitHub to find the latest WineMetalGL release" >&2; exit 1; }
        version=${latest##*/winemetalgl-}
        case "$version" in [0-9]*.[0-9]*) ;; *) echo "no WineMetalGL release found ($latest)" >&2; exit 1 ;; esac
    fi
    name=winemetalgl-$version-$ARCH
    cache=${WINEMETALGL_CACHE:-$HOME/Library/Caches/WineMetalGL/releases}/$version
    if ! test -f "$cache/$name/module/$UNIX_DIR/libwinemetalgl.dylib"; then
        echo "Downloading WineMetalGL $version ($ARCH Wine)"
        tmp=$(mktemp -d "${TMPDIR:-/tmp}/winemetalgl.XXXXXX")
        url=https://github.com/$repo/releases/download/winemetalgl-$version
        if ! curl -fsSL -o "$tmp/SHA256SUMS.txt" "$url/SHA256SUMS.txt"; then
            rm -rf "$tmp"; echo "cannot download WineMetalGL $version from $url" >&2; exit 1
        fi
        if ! grep -q " $name.tar.gz\$" "$tmp/SHA256SUMS.txt"; then
            rm -rf "$tmp"
            echo "WineMetalGL $version has no package for an $ARCH Wine (arm64 Wine packages start with 3.7.0)" >&2
            exit 1
        fi
        curl -fsSL -o "$tmp/$name.tar.gz" "$url/$name.tar.gz" ||
            { rm -rf "$tmp"; echo "cannot download WineMetalGL $version from $url" >&2; exit 1; }
        expected=$(awk -v f="$name.tar.gz" '$2 == f { print $1 }' "$tmp/SHA256SUMS.txt")
        actual=$(shasum -a 256 "$tmp/$name.tar.gz" | awk '{ print $1 }')
        if test -z "$expected" || test "$expected" != "$actual"; then
            rm -rf "$tmp"
            echo "$name.tar.gz does not match the release's SHA256SUMS.txt; not installed" >&2
            exit 1
        fi
        mkdir -p "$cache"
        tar -xzf "$tmp/$name.tar.gz" -C "$cache" || { rm -rf "$tmp"; echo "cannot extract $name.tar.gz" >&2; exit 1; }
        rm -rf "$tmp"
        test -f "$cache/$name/module/$UNIX_DIR/libwinemetalgl.dylib" ||
            { echo "$name.tar.gz has no module/ directory" >&2; exit 1; }
    fi
    echo "Using WineMetalGL $version ($cache/$name)"
    MODULE=$cache/$name/module
    CORE=$MODULE/$UNIX_DIR/libwinemetalgl.dylib
}

# pe_machine FILE: a PE file's COFF machine as 4 hex digits (aa64 ARM64, 8664 x86_64, 014c i386).
pe_machine() {
    offset=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
    od -An -tx2 -j$((offset + 4)) -N2 "$1" | tr -d ' '
}

# The Wine builtin signature, which a Wine loader wants on DLLs it serves from its own lib/
# directories (MetalSharp's Wine serves opengl32.dll that way).
stamp_builtin() {
    printf 'Wine builtin DLL\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000' |
        dd of="$1" bs=1 seek=64 conv=notrunc 2>/dev/null
}

HERE=$(cd "$(dirname "$0")" && pwd)
WINE=${1:?usage: install-wine-module.sh WINE_INSTALL [WINEPREFIX]}
WINE=${WINE%/}
PREFIX=${2:-}
ROOT=$(cd "$HERE/.." && pwd)
case "$WINE" in
*CrossOver.app*) echo "This is CrossOver; use install-crossover.sh BOTTLE instead." >&2; exit 1 ;;
esac
if test -d "$WINE/Contents/lib/winemetalgl"; then
    echo "$WINE is MetalSharp Wine, which already uses WineMetalGL as its OpenGL in every prefix"
    echo "(OPENGL_BACKEND=wine selects Wine's own). It is signed, so nothing is installed into it:"
    echo "a newer WineMetalGL comes with a newer MetalSharp Wine."
    exit 0
fi
test -d "$WINE/Contents/Resources/wine/lib/wine" && WINE="$WINE/Contents/Resources/wine"
LIB="$WINE/lib/wine"
test -x "$WINE/bin/wine" || { echo "not a Wine installation (no bin/wine): $WINE" >&2; exit 1; }

# Which Wine this is: the unix side it has, and for a Wine with both (CrossOver 27 carries
# both), the prefix's own system32, else the architecture of its loader.
prefix_ntdll="$PREFIX/drive_c/windows/system32/ntdll.dll"
ARCH=${WINEMETALGL_ARCH:-}
if test -z "$ARCH"; then
    if test -e "$LIB/aarch64-unix/ntdll.so" && ! test -e "$LIB/x86_64-unix/ntdll.so"; then ARCH=arm64
    elif test -e "$LIB/x86_64-unix/ntdll.so" && ! test -e "$LIB/aarch64-unix/ntdll.so"; then ARCH=x86_64
    elif test -n "$PREFIX" && test -f "$prefix_ntdll" && test "$(pe_machine "$prefix_ntdll")" = aa64; then ARCH=arm64
    elif test -n "$PREFIX" && test -f "$prefix_ntdll"; then ARCH=x86_64
    elif file -b "$WINE/bin/wine" | grep -q arm64; then ARCH=arm64
    else ARCH=x86_64
    fi
fi
case "$ARCH" in
x86_64) UNIX_DIR=x86_64-unix; WIN_DIR=x86_64-windows ;;
arm64) UNIX_DIR=aarch64-unix; WIN_DIR=aarch64-windows ;;
*) echo "WINEMETALGL_ARCH must be x86_64 or arm64, not $ARCH" >&2; exit 1 ;;
esac
test -e "$LIB/$UNIX_DIR/ntdll.so" || { echo "not an $ARCH Wine (no $LIB/$UNIX_DIR/ntdll.so)" >&2; exit 1; }
echo "$WINE is an $ARCH Wine"

if test -d "$HERE/module" && test -f "$HERE/module/$UNIX_DIR/winemetalgl.so"; then
    MODULE=${MODULE:-$HERE/module}
    CORE=${CORE:-$HERE/module/$UNIX_DIR/libwinemetalgl.dylib}
elif test -n "${MODULE:-}"; then
    if test "$ARCH" = arm64; then CORE=${CORE:-$ROOT/build/arm64-release/libwinemetalgl.dylib}
    else CORE=${CORE:-$ROOT/build/release/libwinemetalgl.dylib}
    fi
elif test "$ARCH" = x86_64 && test -d "$ROOT/build/wine-module/x86_64-unix" && test -f "$ROOT/CMakeLists.txt"; then
    MODULE=$ROOT/build/wine-module
    CORE=${CORE:-$ROOT/build/release/libwinemetalgl.dylib}
elif test "$ARCH" = arm64 && test -d "$ROOT/build/wine-module-arm64/aarch64-unix" && test -f "$ROOT/CMakeLists.txt"; then
    MODULE=$ROOT/build/wine-module-arm64
    CORE=${CORE:-$ROOT/build/arm64-release/libwinemetalgl.dylib}
else
    test -d "$HERE/module" && echo "This package is not for an $ARCH Wine; getting the one that is."
    fetch_release
fi

for f in "$MODULE/$WIN_DIR/winemetalgl.dll" "$MODULE/i386-windows/winemetalgl.dll" \
         "$MODULE/$UNIX_DIR/winemetalgl.so" "$CORE"; do
    test -e "$f" || { echo "missing: $f" >&2; exit 1; }
done
if test -n "$PREFIX" && test -f "$prefix_ntdll"; then
    case "$ARCH:$(pe_machine "$prefix_ntdll")" in
    x86_64:8664|arm64:aa64|*:014c) ;;
    *) echo "$PREFIX was made by a Wine of the other kind; give an $ARCH Wine's prefix" >&2; exit 1 ;;
    esac
fi

# macOS keeps a signed app from being changed unless the program changing it
# (the terminal) has App Management permission.
install_file() {
    if ! out=$(cp "$1" "$2" 2>&1); then
        echo "$out" >&2
        case "$out" in
        *"Operation not permitted"*)
            echo "macOS did not let this terminal change $WINE. Allow it in System Settings >" >&2
            echo "Privacy & Security > App Management (then reopen it), or install into a copy of Wine outside the app." >&2 ;;
        esac
        exit 1
    fi
}
install_file "$MODULE/$WIN_DIR/winemetalgl.dll" "$LIB/$WIN_DIR/winemetalgl.dll"
if test -d "$LIB/i386-windows"; then
    install_file "$MODULE/i386-windows/winemetalgl.dll" "$LIB/i386-windows/winemetalgl.dll"
else
    echo "This Wine has no i386-windows: 32-bit programs keep Wine's OpenGL."
fi
install_file "$MODULE/$UNIX_DIR/winemetalgl.so" "$LIB/$UNIX_DIR/winemetalgl.so"
if test "$CORE" != "$LIB/$UNIX_DIR/libwinemetalgl.dylib"; then install_file "$CORE" "$LIB/$UNIX_DIR/libwinemetalgl.dylib"; fi
# The x87 acceleration helper (WINEMETALGL_X87), when the module has one. It hooks Rosetta's
# x87 path, so an arm64 Wine (which runs x86 code through its own emulator) has none.
if test "$ARCH" = arm64; then
    :
elif test -e "$MODULE/x86_64-unix/x87sidecar"; then
    install_file "$MODULE/x86_64-unix/x87sidecar" "$LIB/x86_64-unix/x87sidecar"
else
    echo "This module has no x87sidecar: x87 acceleration (WINEMETALGL_X87) stays off."
fi
# Copies keep a valid signature (a release's Developer ID one); others are
# signed ad hoc.
for f in "$LIB/$UNIX_DIR/winemetalgl.so" "$LIB/$UNIX_DIR/libwinemetalgl.dylib" "$LIB/x86_64-unix/x87sidecar"; do
    test -e "$f" || continue
    codesign --verify "$f" 2>/dev/null && continue
    if ! out=$(codesign -f -s - "$f" 2>&1); then
        echo "$out" >&2
        echo "codesign failed: $f" >&2
        exit 1
    fi
done
echo "Installed winemetalgl into $LIB"

# MetalSharp's Wine (a folder of it; the app is handled above) serves WineMetalGL's opengl32.dll
# from lib/winemetalgl in every prefix, so its forwarders are updated and prefixes need nothing.
if test -d "$WINE/lib/winemetalgl"; then
    for arch in aarch64 i386; do
        test -d "$WINE/lib/winemetalgl/$arch-windows" || continue
        install_file "$MODULE/$arch-windows/opengl32.dll" "$WINE/lib/winemetalgl/$arch-windows/opengl32.dll"
        stamp_builtin "$WINE/lib/winemetalgl/$arch-windows/opengl32.dll"
    done
    echo "Updated MetalSharp Wine's WineMetalGL; it is already every prefix's OpenGL, so no prefix needs setting up."
    exit 0
fi

if test -z "$PREFIX"; then
    echo "No prefix given: run again with one (install-wine-module.sh WINE_INSTALL WINEPREFIX) for its programs to use WineMetalGL."
    exit 0
fi
if ! test -e "$PREFIX"; then
    echo "Creating the Wine prefix $PREFIX"
    if ! out=$(WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE/bin/wine" wineboot -i 2>&1); then
        echo "$out" >&2
        echo "wineboot failed: $PREFIX" >&2
        exit 1
    fi
fi
test -d "$PREFIX/drive_c/windows/system32" || { echo "not a Wine prefix: $PREFIX" >&2; exit 1; }
# winemetalgl.dll goes next to the forwarder too: a Wine whose loader
# does not search lib/wine/<arch>-windows for it still finds it in the prefix.
cp "$MODULE/$WIN_DIR/opengl32.dll" "$PREFIX/drive_c/windows/system32/opengl32.dll"
cp "$MODULE/$WIN_DIR/winemetalgl.dll" "$PREFIX/drive_c/windows/system32/winemetalgl.dll"
if test -d "$PREFIX/drive_c/windows/syswow64"; then
    cp "$MODULE/i386-windows/opengl32.dll" "$PREFIX/drive_c/windows/syswow64/opengl32.dll"
    cp "$MODULE/i386-windows/winemetalgl.dll" "$PREFIX/drive_c/windows/syswow64/winemetalgl.dll"
fi
if ! out=$(WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE/bin/wine" reg add 'HKCU\Software\Wine\DllOverrides' \
    /v opengl32 /d native /f 2>&1); then
    echo "$out" >&2
    echo "could not set opengl32=native in $PREFIX" >&2
    exit 1
fi
echo "Installed opengl32.dll (opengl32=native) into $PREFIX"
