#!/bin/zsh
# KosmicKrisp, Mesa's Vulkan driver on Metal, as the alternative to MoltenVK: SharpVK_Switch=1 has the launcher
# (bin/wine) point the Vulkan loader at it instead of MoltenVK. Installs
#   install/lib/libvulkan_kosmickrisp.dylib
#   install/share/vulkan/icd.d/kosmickrisp_icd.json
#   install/share/licenses/mesa/
# Mesa is fetched at MESA_REV into src/mesa. Its build tools (mesa_clc, which precompiles KosmicKrisp's kernels)
# need LLVM, the SPIR-V LLVM translator and libclc from Homebrew; the driver links none of them. SPIRV-Tools is
# built here as static libraries, so the driver links nothing outside the system.
# KosmicKrisp needs Metal 4 (macOS 26). It is built for macOS 15 like the rest of the bundle, so its Metal 4 symbols
# are weak imports: the bundle still starts on macOS 15, where only SharpVK_Switch=1 would fail.
set -e
source ${0:A:h}/env.sh
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
MESA_REV=3c253c7f2363f07bff5167cc6a51148333fc55f3   # Mesa main, 2026-11-17
SPIRV_TOOLS_REV=vulkan-sdk-1.4.363.0
SPIRV_HEADERS_REV=vulkan-sdk-1.4.363.0
M=$ROOT/src/mesa
W=$ROOT/build/kosmickrisp
I=$ROOT/install
for f in llvm spirv-llvm-translator libclc bison flex; do
  [[ -d /opt/homebrew/opt/$f ]] || { echo "build-kosmickrisp: brew install $f" >&2; exit 1; }
done

fetch() {  # fetch <url> <rev> <dir>: a shallow checkout of one commit or tag, fetched again when <rev> changes
  local mark=$W/${3:t}.rev
  [[ -d $3 && -f $mark && $(<$mark) == $2 ]] && return
  rm -rf $3 && git init -q $3
  git -C $3 fetch -q --depth 1 $1 $2 && git -C $3 checkout -q --detach FETCH_HEAD
  mkdir -p $W && print -r -- $2 > $mark
}
fetch https://gitlab.freedesktop.org/mesa/mesa.git $MESA_REV $M
# MetalSharp's patches (patches/mesa), applied once over MESA_REV
for p in $ROOT/patches/mesa/*.patch(N); do
  git -C $M apply --reverse --check $p 2>/dev/null || git -C $M apply $p
done
fetch https://github.com/KhronosGroup/SPIRV-Headers.git $SPIRV_HEADERS_REV $W/SPIRV-Headers
fetch https://github.com/KhronosGroup/SPIRV-Tools.git $SPIRV_TOOLS_REV $W/SPIRV-Tools

# Static SPIRV-Tools for the driver.
ST=$W/spirv-tools-prefix
if [[ ! -f $ST/lib/libSPIRV-Tools.a ]]; then
  cmake -S $W/SPIRV-Tools -B $W/spirv-tools-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$ST \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DSPIRV-Headers_SOURCE_DIR=$W/SPIRV-Headers \
      -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
  cmake --build $W/spirv-tools-build -j$JOBS >/dev/null
  cmake --install $W/spirv-tools-build >/dev/null
fi

# Mesa's generators need Python 3.10 or later with mako, PyYAML and packaging.
V=$W/venv
[[ -x $V/bin/python3 ]] || { /opt/homebrew/bin/python3 -m venv $V && $V/bin/pip -q install mako pyyaml packaging meson; }
export PATH=$V/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/llvm/bin:$PATH
export PKG_CONFIG_PATH=$ST/lib/pkgconfig:/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/opt/spirv-llvm-translator/lib/pkgconfig:/opt/homebrew/opt/libclc/share/pkgconfig
unset PKG_CONFIG_LIBDIR
export MACOSX_DEPLOYMENT_TARGET=15.0
B=$W/build
if [[ ! -f $B/build.ninja ]]; then
  # The build tools link LLVM shared: Homebrew's translator links libLLVM.dylib, and a second, static copy of LLVM
  # in mesa_clc crashes it.
  meson setup $B $M --buildtype=release -Db_ndebug=true -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
      -Dgallium-drivers= -Dopengl=false -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled \
      -Dllvm=enabled -Dshared-llvm=enabled -Dc_link_args=-L/opt/homebrew/lib -Dcpp_link_args=-L/opt/homebrew/lib \
      -Dprefix=$W/prefix >/dev/null
fi
ninja -C $B >/dev/null
ninja -C $B install >/dev/null

D=$I/lib/libvulkan_kosmickrisp.dylib
cp $W/prefix/lib/libvulkan_kosmickrisp.dylib $D
install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib $D 2>/dev/null
if otool -L $D | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/|@rpath/libvulkan_kosmickrisp)'; then
  echo "build-kosmickrisp: $D links something outside the system (above)" >&2; exit 1
fi
codesign -f -s - $D 2>/dev/null
mkdir -p $I/share/vulkan/icd.d
sed 's#"library_path": *"[^"]*"#"library_path": "../../../lib/libvulkan_kosmickrisp.dylib"#' \
    $W/prefix/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json > $I/share/vulkan/icd.d/kosmickrisp_icd.json
mkdir -p $I/share/licenses/mesa && cp $M/docs/license.rst $I/share/licenses/mesa/ 2>/dev/null || true
echo "kosmickrisp: $D (Mesa $(cat $M/VERSION) ${MESA_REV[1,10]})"
