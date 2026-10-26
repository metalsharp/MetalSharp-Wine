#!/bin/sh
# Installs the WineMetalGL Wine module into a CrossOver bottle. For any other Wine, use
# install-wine-module.sh. A bottle of an x86_64 Wine (CrossOver up to 26, under Rosetta) gets
# the x86_64 module (scripts/build-wine-module.sh); a bottle of CrossOver 27's arm64 Wine gets
# the arm64 one (scripts/build-wine-module-arm64.sh): the bottle's own system32 tells which, so
# this works without CrossOver too. WINEMETALGL_ARCH=x86_64 or arm64 overrides that.
#
#   scripts/install-crossover.sh BOTTLE [CROSSOVER]
#
# BOTTLE is a bottle's name (in ~/Library/Application Support/CrossOver/Bottles)
# or its directory, wherever it is. CROSSOVER is CrossOver.app, or its
# Contents/SharedSupport/CrossOver (default /Applications/CrossOver.app, then
# ~/Applications/CrossOver.app, then any copy Spotlight knows). Run it once per
# bottle. Without CrossOver, the bottle still gets everything that goes into
# it (and WINEMETALGL_HOME the module): the override is then written into its
# user.reg, and CrossOver, once there, finds the module through DllPath.
#
# CrossOver.app is signed and macOS does not let it be changed, so nothing goes
# into it:
#   WINEMETALGL_HOME (default ~/Library/Application Support/WineMetalGL) gets
#     lib/wine/x86_64-windows/winemetalgl.dll, lib/wine/i386-windows/winemetalgl.dll
#     lib/wine/x86_64-unix/winemetalgl.so, lib/wine/x86_64-unix/libwinemetalgl.dylib (the core)
#     or, for an arm64 bottle, lib/wine/aarch64-windows/winemetalgl.dll (ARM64X),
#     lib/wine/i386-windows/winemetalgl.dll and lib/wine/aarch64-unix/{winemetalgl.so,
#     libwinemetalgl.dylib}
#   the bottle's cxbottle.conf adds WINEMETALGL_HOME/lib/wine to [Wine] DllPath
#   the bottle gets opengl32.dll, which forwards to winemetalgl.dll, and
#     winemetalgl.dll itself, in system32 (and syswow64), and the DLL override
#     opengl32=native.
# A CrossOver that ignores [Wine] DllPath (the CrossOver 27 Preview, issue
# #26) is the exception: the module then goes into its lib/wine after all.
# Removing the override (in the bottle's Wine configuration, Libraries) gives
# the bottle CrossOver's OpenGL back; `wine --bottle BOTTLE --dll opengl32=b`
# does so for one run.
#
# From a checkout, MODULE (default build/wine-module, or build/wine-module-arm64 for an arm64
# bottle) and CORE (default build/release/libwinemetalgl.dylib, or build/arm64-release/) choose
# what is installed; from a release package (this script at its top), its module directory
# when it is for the bottle's kind of Wine; and otherwise the matching release downloaded from
# GitHub (fetch_release).
set -eu

usage='usage: install-crossover.sh BOTTLE [CROSSOVER]'
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

HERE=$(cd "$(dirname "$0")" && pwd)
BOTTLE=${1:?$usage}
CX=${2:-}
ROOT=$(cd "$HERE/.." && pwd)

if test -z "$CX"; then
    CX=$({ printf '%s\n' /Applications/CrossOver.app "$HOME/Applications/CrossOver.app"
           mdfind "kMDItemCFBundleIdentifier == 'com.codeweavers.CrossOver'" 2>/dev/null; } |
         while IFS= read -r app; do
             test -x "$app/Contents/SharedSupport/CrossOver/bin/wine" && { printf '%s\n' "$app"; break; }
         done) || :
    test -n "$CX" || echo "CrossOver not found: installing into the bottle only (pass CrossOver's path to use it: $usage)"
fi
if test -n "$CX"; then
    CX=${CX%/}
    test -d "$CX/Contents/SharedSupport/CrossOver" && CX="$CX/Contents/SharedSupport/CrossOver"
    test -x "$CX/bin/wine" && test -x "$CX/bin/cxbottle" || { echo "not CrossOver: $CX" >&2; exit 1; }
fi

case "$BOTTLE" in
*/*) ;;
*) BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE" ;;
esac
test -f "$BOTTLE/cxbottle.conf" && test -d "$BOTTLE/drive_c/windows/system32" ||
    { echo "not a CrossOver bottle: $BOTTLE" >&2; exit 1; }
BOTTLE=$(cd "$BOTTLE" && pwd)

# Which Wine the bottle belongs to: its system32 is ARM64 in a bottle of CrossOver 27's arm64
# Wine and x86_64 in one of an x86_64 Wine. A 32-bit bottle (i386 system32) runs on whichever
# unix side the CrossOver has, x86_64 when it has both (issue #13).
ARCH=${WINEMETALGL_ARCH:-}
if test -z "$ARCH"; then
    case "$(pe_machine "$BOTTLE/drive_c/windows/system32/ntdll.dll" 2>/dev/null)" in
    aa64) ARCH=arm64 ;;
    8664) ARCH=x86_64 ;;
    *) if test -n "$CX" && test -e "$CX/lib/wine/aarch64-unix/ntdll.so" && ! test -e "$CX/lib/wine/x86_64-unix/ntdll.so"
       then ARCH=arm64; else ARCH=x86_64; fi ;;
    esac
fi
case "$ARCH" in
x86_64) UNIX_DIR=x86_64-unix; WIN_DIR=x86_64-windows ;;
arm64) UNIX_DIR=aarch64-unix; WIN_DIR=aarch64-windows ;;
*) echo "WINEMETALGL_ARCH must be x86_64 or arm64, not $ARCH" >&2; exit 1 ;;
esac
echo "$BOTTLE is a bottle of an $ARCH Wine"
if test -n "$CX" && ! test -e "$CX/lib/wine/$UNIX_DIR/ntdll.so"; then
    echo "This CrossOver has no $ARCH Wine, which the bottle needs: $CX" >&2
    exit 1
fi

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
         "$MODULE/$WIN_DIR/opengl32.dll" "$MODULE/i386-windows/opengl32.dll" \
         "$MODULE/$UNIX_DIR/winemetalgl.so" "$CORE"; do
    test -e "$f" || { echo "missing: $f" >&2; exit 1; }
done

DEST=${WINEMETALGL_HOME:-$HOME/Library/Application Support/WineMetalGL}
LIB="$DEST/lib/wine"
mkdir -p "$LIB/$WIN_DIR" "$LIB/i386-windows" "$LIB/$UNIX_DIR"
cp "$MODULE/$WIN_DIR/winemetalgl.dll" "$LIB/$WIN_DIR/winemetalgl.dll"
cp "$MODULE/i386-windows/winemetalgl.dll" "$LIB/i386-windows/winemetalgl.dll"
cp "$MODULE/$UNIX_DIR/winemetalgl.so" "$LIB/$UNIX_DIR/winemetalgl.so"
cp "$CORE" "$LIB/$UNIX_DIR/libwinemetalgl.dylib"
# The x87 acceleration helper (WINEMETALGL_X87), when the module has one. It hooks Rosetta's
# x87 path, so an arm64 bottle (whose Wine emulates x86 itself) has none.
if test "$ARCH" = arm64; then
    :
elif test -e "$MODULE/x86_64-unix/x87sidecar"; then
    cp "$MODULE/x86_64-unix/x87sidecar" "$LIB/x86_64-unix/x87sidecar"
else
    rm -f "$LIB/x86_64-unix/x87sidecar"
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

# CrossOver's wine script sets WINEDLLPATH from the bottle's [Wine] DllPath,
# or, unset, from CrossOver's lib/wine/<arch>-windows directories (the
# defaults below). An entry like those would not do: Wine looks for a DLL's
# unix library next to it there, and a 32-bit program would find the x86_64
# winemetalgl.dll first. A lib/wine entry lets Wine pick the DLL of the
# program's architecture and find winemetalgl.so in x86_64-unix (aarch64-unix in an arm64 bottle).
conf="$BOTTLE/cxbottle.conf"
win32=0
grep -q '^"WineArch" *= *"win64"' "$conf" || win32=1
# shellcheck disable=SC2016 # ${CX_ROOT} is expanded by CrossOver
if test "$win32" = 1; then
    default='${CX_ROOT}/lib/wine/i386-windows:${CX_ROOT}/lib/wine'
else
    default='${CX_ROOT}/lib/wine/'"$WIN_DIR"':${CX_ROOT}/lib/wine/i386-windows:${CX_ROOT}/lib/wine'
fi
awk -v entry="$LIB" -v default="$default" '
    function emit() { if (!done) { print "\"DllPath\" = \"" default ":" entry "\""; done = 1 } }
    /^\[/ { if (wine) emit(); wine = ($0 == "[Wine]"); seen = seen || wine }
    wine && /^"DllPath"[ \t]*=/ {
        if (!index($0, ":" entry "\"") && !index($0, ":" entry ":")) sub(/"[ \t]*$/, ":" entry "\"")
        print; done = 1; next
    }
    { print }
    END { if (wine) emit(); else if (!seen) { print ""; print "[Wine]"; emit() } }
' "$conf" > "$conf.winemetalgl"
mv "$conf.winemetalgl" "$conf"
echo "Added $LIB to the [Wine] DllPath of $conf"

# winemetalgl.dll goes next to the forwarder too: CrossOver's loader finds the
# builtin (through DllPath) only for a DLL it has found in the bottle (issue
# #16: "Could not create window!"). A 32-bit bottle has its i386 DLLs in
# system32.
sys=$BOTTLE/drive_c/windows
if test "$win32" = 1; then
    cp "$MODULE/i386-windows/opengl32.dll" "$MODULE/i386-windows/winemetalgl.dll" "$sys/system32/"
else
    cp "$MODULE/$WIN_DIR/opengl32.dll" "$MODULE/$WIN_DIR/winemetalgl.dll" "$sys/system32/"
    cp "$MODULE/i386-windows/opengl32.dll" "$MODULE/i386-windows/winemetalgl.dll" "$sys/syswow64/"
fi
# CrossOver's wine script picks the bottle with --bottle and ignores WINEPREFIX.
# Without CrossOver, the override goes straight into the bottle's user.reg
# (Wine reads it when the bottle next starts).
if test -n "$CX"; then
    if ! out=$("$CX/bin/wine" --bottle "$BOTTLE" --wait-children reg add \
        'HKCU\Software\Wine\DllOverrides' /v opengl32 /d native /f 2>&1); then
        echo "$out" >&2
        echo "could not set opengl32=native in $BOTTLE" >&2
        exit 1
    fi
else
    reg=$BOTTLE/user.reg
    test -f "$reg" || printf 'WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n' > "$reg"
    awk -v ts="$(date +%s)" '
        function emit() { if (!done) { print "\"opengl32\"=\"native\""; done = 1 } }
        /^\[/ { if (overrides) emit(); overrides = index(tolower($0), "[software\\\\wine\\\\dlloverrides]") == 1 }
        overrides && tolower($0) ~ /^"opengl32"=/ { emit(); next }
        { print }
        END { if (overrides) emit(); if (!done) { print ""; print "[Software\\\\Wine\\\\DllOverrides] " ts; emit() } }
    ' "$reg" > "$reg.winemetalgl"
    mv "$reg.winemetalgl" "$reg"
fi
echo "Installed opengl32.dll (opengl32=native) into $BOTTLE"

# Some CrossOver builds (the CrossOver 27 Preview, issue #26) ignore the
# bottle's [Wine] DllPath: their WINEDLLPATH has CrossOver's own directories
# only, and Steam stops at "Could not load module 'bin/vgui2_s.dll'"
# (winemetalgl.so is not loaded). Wine lists the directories it was given as
# WINEDLLDIR<n> in the bottle's environment; when $LIB is not among them, the
# module goes into CrossOver's lib/wine instead, the one place such a build
# looks. WINEMETALGL_INTO_APP=1 does that without asking Wine.
into_app=${WINEMETALGL_INTO_APP:-}
if test -z "$CX"; then
    test "$into_app" = 1 && { echo "WINEMETALGL_INTO_APP=1 needs CrossOver: $usage" >&2; exit 1; }
    echo "With no CrossOver to ask, CrossOver finds the module through the bottle's DllPath; if a game"
    echo "cannot load winemetalgl, run this again with CrossOver's path (and WINEMETALGL_INTO_APP=1)."
elif test -z "$into_app"; then
    # shellcheck disable=SC1003 # tr gets a backslash
    dirs=$("$CX/bin/wine" --bottle "$BOTTLE" --wait-children cmd /c set WINEDLLDIR 2>/dev/null |
        tr -d '\r' | tr '\\' '/' | grep -i '^WINEDLLDIR[0-9]*=') || dirs=
    if test -z "$dirs"; then
        echo "Could not ask $BOTTLE's Wine for its DLL directories; if a game cannot load winemetalgl,"
        echo "run this again with WINEMETALGL_INTO_APP=1."
    elif printf '%s\n' "$dirs" | grep -qiF "$LIB"; then
        echo "CrossOver's Wine finds the module in $LIB"
    else
        echo "This CrossOver ignores the bottle's [Wine] DllPath; installing the module into it instead."
        into_app=1
    fi
fi
if test "$into_app" = 1; then
    CXLIB=$CX/lib/wine
    if cp "$LIB/$WIN_DIR/winemetalgl.dll" "$CXLIB/$WIN_DIR/winemetalgl.dll" &&
        cp "$LIB/i386-windows/winemetalgl.dll" "$CXLIB/i386-windows/winemetalgl.dll" &&
        cp "$LIB/$UNIX_DIR/winemetalgl.so" "$CXLIB/$UNIX_DIR/winemetalgl.so" &&
        cp "$LIB/$UNIX_DIR/libwinemetalgl.dylib" "$CXLIB/$UNIX_DIR/libwinemetalgl.dylib" &&
        { test "$ARCH" = arm64 || ! test -e "$LIB/x86_64-unix/x87sidecar" ||
          cp "$LIB/x86_64-unix/x87sidecar" "$CXLIB/x86_64-unix/x87sidecar"; }; then
        echo "Installed winemetalgl into $CXLIB"
        echo "This changes CrossOver.app: updating CrossOver removes the module, and this has to run again."
    else
        echo "Could not write into $CXLIB (macOS may protect CrossOver.app: allow Terminal or" >&2
        echo "WineMetalGL Settings under Privacy & Security, App Management, or use a copy of" >&2
        echo "CrossOver.app in ~/Applications). Copy these by hand:" >&2
        echo "  $LIB/$WIN_DIR/winemetalgl.dll -> $CXLIB/$WIN_DIR/" >&2
        echo "  $LIB/i386-windows/winemetalgl.dll -> $CXLIB/i386-windows/" >&2
        echo "  $LIB/$UNIX_DIR/winemetalgl.so -> $CXLIB/$UNIX_DIR/" >&2
        echo "  $LIB/$UNIX_DIR/libwinemetalgl.dylib -> $CXLIB/$UNIX_DIR/" >&2
        test "$ARCH" = x86_64 && test -e "$LIB/x86_64-unix/x87sidecar" && echo "  $LIB/x86_64-unix/x87sidecar -> $CXLIB/x86_64-unix/" >&2
        exit 1
    fi
fi
