#!/bin/sh
# Stages the release for a native arm64 Wine (CrossOver 27's arm64 Wine, MetalSharp's Wine with
# FEX): the arm64 Wine module (scripts/build-wine-module-arm64.sh) with the arm64 core. The
# x86_64 release (scripts/stage-release.sh) is the package for x86_64 Wine under Rosetta; the
# install scripts in either package pick the one that matches the Wine they install into.
#
#   scripts/stage-release-arm64.sh WINE_HEADERS OUTPUT
#
# WINE_HEADERS is a Wine 11.x installation or source tree, for the module's build. Needs
# llvm-mingw (arm64ec-w64-mingw32-clang, i686-w64-mingw32-clang) on PATH.
#
# install-wine-module.sh, install-crossover.sh
# module/
#   aarch64-unix/     winemetalgl.so, libwinemetalgl.dylib (lib/wine/aarch64-unix)
#   aarch64-windows/  winemetalgl.dll (lib/wine/aarch64-windows), opengl32.dll (prefix system32);
#                     ARM64X: native ARM64 programs and x86_64 ones, which run as ARM64EC
#   i386-windows/     winemetalgl.dll (lib/wine/i386-windows), opengl32.dll (prefix syswow64)
set -eu

if test "$#" -ne 2; then
    printf '%s\n' "usage: $0 /absolute/path/to/wine-headers /absolute/path/to/output" >&2
    exit 2
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wine_headers=$1
output=$2
build="$project_root/build/arm64-release"
core="$build/libwinemetalgl.dylib"
module="$project_root/build/wine-module-arm64"

if test "$output" = /; then
    printf '%s\n' 'refusing to stage over /' >&2
    exit 1
fi
if test -e "$output"; then
    printf 'output already exists; refusing to replace it: %s\n' "$output" >&2
    exit 1
fi

# Build the arm64 core and the module from this checkout, so the staged libraries are the
# commit recorded below.
cmake -S "$project_root" -B "$build" -G Ninja -DWMGL_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$build" --target winemetalgl_core >/dev/null
rm -rf "$module"
"$project_root/scripts/build-wine-module-arm64.sh" "$wine_headers" "$module" >/dev/null

mkdir -p "$output/module/aarch64-unix" "$output/module/aarch64-windows" "$output/module/i386-windows"
cp "$core" "$output/module/aarch64-unix/libwinemetalgl.dylib"
cp "$module/aarch64-unix/winemetalgl.so" "$output/module/aarch64-unix/winemetalgl.so"
for arch in aarch64 i386; do
    cp "$module/$arch-windows/winemetalgl.dll" "$module/$arch-windows/opengl32.dll" "$output/module/$arch-windows/"
done
# Signatures as in stage-release.sh: WINEMETALGL_SIGN_IDENTITY (Developer ID Application) with
# the hardened runtime and a secure timestamp, otherwise ad hoc.
for library in "$output"/module/aarch64-unix/*; do
    if test -n "${WINEMETALGL_SIGN_IDENTITY:-}"; then
        codesign --force --options runtime --timestamp --sign "$WINEMETALGL_SIGN_IDENTITY" "$library"
    else
        codesign --force --sign - "$library"
    fi
done
cp "$project_root/scripts/install-wine-module.sh" "$output/install-wine-module.sh"
cp "$project_root/scripts/install-crossover.sh" "$output/install-crossover.sh"
cp "$project_root/README.md" "$output/README.md"
cp "$project_root/LICENSE" "$output/LICENSE"
cp -R "$project_root/LICENSES" "$output/LICENSES"

commit=$(git -C "$project_root" rev-parse HEAD)
if ! git -C "$project_root" diff --quiet HEAD; then commit="$commit (uncommitted changes)"; fi
printf 'WineMetalGL (arm64 Wine) staged from %s\nWine headers: %s\nCommit: %s\n' \
    "$project_root" "$wine_headers" "$commit" > "$output/STAGED-FROM.txt"

(
    cd "$output"
    find . -type f ! -name SHA256SUMS -print | sort | while IFS= read -r path; do
        shasum -a 256 "$path"
    done > SHA256SUMS
)

# Checks: what is there, the Mach-O and PE architectures, the signatures, and the Wine builtin
# signature that makes Wine load winemetalgl.so for winemetalgl.dll.
for path in module/aarch64-unix/winemetalgl.so module/aarch64-unix/libwinemetalgl.dylib \
    module/aarch64-windows/winemetalgl.dll module/aarch64-windows/opengl32.dll \
    module/i386-windows/winemetalgl.dll module/i386-windows/opengl32.dll \
    install-wine-module.sh install-crossover.sh LICENSE SHA256SUMS; do
    test -f "$output/$path" || { printf 'missing staged artifact: %s\n' "$path" >&2; exit 1; }
done
for path in "$output"/module/aarch64-unix/*; do
    test "$(lipo -archs "$path")" = arm64 || { printf 'not arm64: %s\n' "$path" >&2; exit 1; }
    codesign --verify "$path" 2>/dev/null || { printf 'not validly signed: %s\n' "$path" >&2; exit 1; }
done
pe_machine() {  # pe_machine FILE: the COFF machine, as 4 hex digits
    offset=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
    od -An -tx2 -j$((offset + 4)) -N2 "$1" | tr -d ' '
}
for dll in winemetalgl opengl32; do
    test "$(pe_machine "$output/module/aarch64-windows/$dll.dll")" = aa64 ||
        { printf 'not an ARM64X DLL: aarch64-windows/%s.dll\n' "$dll" >&2; exit 1; }
    test "$(pe_machine "$output/module/i386-windows/$dll.dll")" = 014c ||
        { printf 'not an i386 DLL: i386-windows/%s.dll\n' "$dll" >&2; exit 1; }
done
for arch in aarch64 i386; do
    test "$(dd if="$output/module/$arch-windows/winemetalgl.dll" bs=1 skip=64 count=16 2>/dev/null)" = "Wine builtin DLL" ||
        { printf 'no Wine builtin signature: %s-windows/winemetalgl.dll\n' "$arch" >&2; exit 1; }
done
printf 'Staged the arm64 Wine release in %s\n' "$output"
