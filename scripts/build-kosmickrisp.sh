#!/bin/zsh
# KosmicKrisp, Mesa's Vulkan driver on Metal, as the alternative to MoltenVK: SharpVK_Switch=1 has the launcher
# (bin/wine) point the Vulkan loader at it instead of MoltenVK. Installs
#   install/lib/libvulkan_kosmickrisp.dylib
#   install/share/vulkan/icd.d/kosmickrisp_icd.json
#   install/share/licenses/mesa/
# or, with the argument x86_64, for MetalSharp-Wine-x86_64.app (package-x86_64.sh KOSMICKRISP=install-x86_64/kosmickrisp),
# whose Wine opens libvulkan.dylib (MoltenVK) itself, with no Vulkan loader:
#   install-x86_64/kosmickrisp/libvulkan_kosmickrisp.dylib   the driver, built for x86_64 (Rosetta)
#   install-x86_64/kosmickrisp/libvulkan_loader.dylib        the Khronos Vulkan loader, which win32u opens in
#                                                            MoltenVK's place with SharpVK_Switch=1
#   install-x86_64/kosmickrisp/kosmickrisp_icd.json          the driver's manifest, beside it
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
VULKAN_REV=vulkan-sdk-1.4.363.0      # Vulkan-Headers and Vulkan-Loader (x86_64 only)
ARCH=arm64; [[ $1 == x86_64 ]] && ARCH=x86_64
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

# Static SPIRV-Tools for the driver, of its architecture.
ST=$W/spirv-tools-prefix; [[ $ARCH == x86_64 ]] && ST=$W/spirv-tools-prefix-x86_64
STB=$W/spirv-tools-build; [[ $ARCH == x86_64 ]] && STB=$W/spirv-tools-build-x86_64
if [[ ! -f $ST/lib/libSPIRV-Tools.a ]]; then
  cmake -S $W/SPIRV-Tools -B $STB -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$ST \
      -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DSPIRV-Headers_SOURCE_DIR=$W/SPIRV-Headers \
      -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
  cmake --build $STB -j$JOBS >/dev/null
  cmake --install $STB >/dev/null
fi

# Mesa's generators need Python 3.10 or later with mako, PyYAML and packaging.
V=$W/venv
[[ -x $V/bin/python3 ]] || { /opt/homebrew/bin/python3 -m venv $V && $V/bin/pip -q install mako pyyaml packaging meson; }
export PATH=$V/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/llvm/bin:$PATH
unset PKG_CONFIG_LIBDIR
export MACOSX_DEPLOYMENT_TARGET=15.0
# The build tools link LLVM shared: Homebrew's translator links libLLVM.dylib, and a second, static copy of LLVM in
# mesa_clc crashes it. They always run natively (arm64); for x86_64 only the driver is cross-built.
mesa_options=(--buildtype=release -Db_ndebug=true -Dplatforms=macos -Dvulkan-drivers=kosmickrisp
         -Dgallium-drivers= -Dopengl=false -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dzstd=disabled
         -Dllvm=enabled -Dshared-llvm=enabled)
NATIVE_PC=/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/opt/spirv-llvm-translator/lib/pkgconfig:/opt/homebrew/opt/libclc/share/pkgconfig:/opt/homebrew/lib/pkgconfig
if [[ $ARCH == arm64 ]]; then
  export PKG_CONFIG_PATH=$ST/lib/pkgconfig:$NATIVE_PC
  B=$W/build
  [[ -f $B/build.ninja ]] || meson setup $B $M $mesa_options -Dc_link_args=-L/opt/homebrew/lib \
      -Dcpp_link_args=-L/opt/homebrew/lib -Dprefix=$W/prefix >/dev/null
else
  export PKG_CONFIG_PATH=$NATIVE_PC   # in a cross build the environment's path is the native tools'
  cross=$W/cross-macos-x86_64.ini
  print -r -- "[binaries]
c = ['/usr/bin/clang', '-arch', 'x86_64']
cpp = ['/usr/bin/clang++', '-arch', 'x86_64']
objc = ['/usr/bin/clang', '-arch', 'x86_64']
objcpp = ['/usr/bin/clang++', '-arch', 'x86_64']
ar = 'ar'
strip = 'strip'
pkg-config = '/opt/homebrew/bin/pkgconf'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$ST/lib/pkgconfig'

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'" > $cross
  # The kernel compilers (mesa_clc, vtn_bindgen2, kk_clc) are the arm64 build's: the x86_64 driver needs no LLVM.
  N=$W/build
  [[ -x $N/src/compiler/clc/mesa_clc && -x $N/src/kosmickrisp/clc/kk_clc ]] || {
    echo "build-kosmickrisp: build the arm64 driver first (build-kosmickrisp.sh): its kernel compilers are used" >&2; exit 1; }
  export PATH=$N/src/compiler/clc:$N/src/compiler/spirv:$N/src/kosmickrisp/clc:$PATH
  B=$W/build-x86_64
  [[ -f $B/build.ninja ]] || meson setup $B $M --cross-file $cross ${mesa_options:#-D*llvm=enabled} \
      -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dprefix=$W/prefix-x86_64 >/dev/null
fi
ninja -C $B >/dev/null
ninja -C $B install >/dev/null

if [[ $ARCH == x86_64 ]]; then
  # The Khronos Vulkan loader, for the x86_64 Wine, which has none.
  fetch https://github.com/KhronosGroup/Vulkan-Headers.git $VULKAN_REV $W/Vulkan-Headers
  fetch https://github.com/KhronosGroup/Vulkan-Loader.git $VULKAN_REV $W/Vulkan-Loader
  cmake -S $W/Vulkan-Headers -B $W/vulkan-headers-build -G Ninja -DCMAKE_INSTALL_PREFIX=$W/vulkan-headers >/dev/null
  cmake --install $W/vulkan-headers-build >/dev/null
  cmake -S $W/Vulkan-Loader -B $W/vulkan-loader-build-x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_PREFIX_PATH=$W/vulkan-headers \
      -DBUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$W/vulkan-loader-x86_64 >/dev/null
  cmake --build $W/vulkan-loader-build-x86_64 -j$JOBS >/dev/null
  cmake --install $W/vulkan-loader-build-x86_64 >/dev/null
  O=$ROOT/install-x86_64/kosmickrisp
  rm -rf $O && mkdir -p $O
  cp $W/prefix-x86_64/lib/libvulkan_kosmickrisp.dylib $O/
  cp -L $W/vulkan-loader-x86_64/lib/libvulkan.1.dylib $O/libvulkan_loader.dylib
  install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib $O/libvulkan_kosmickrisp.dylib 2>/dev/null
  install_name_tool -id @rpath/libvulkan_loader.dylib $O/libvulkan_loader.dylib 2>/dev/null
  for f in $O/*.dylib; do
    [[ $(lipo -archs $f) == x86_64 ]] || { echo "build-kosmickrisp: $f is not x86_64" >&2; exit 1; }
    if otool -L $f | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/|@rpath/libvulkan_)'; then
      echo "build-kosmickrisp: $f links something outside the system (above)" >&2; exit 1
    fi
    codesign -f -s - $f 2>/dev/null
  done
  sed 's#"library_path": *"[^"]*"#"library_path": "./libvulkan_kosmickrisp.dylib"#' \
      $W/prefix-x86_64/share/vulkan/icd.d/kosmickrisp_mesa_icd.x86_64.json > $O/kosmickrisp_icd.json
  cp $M/docs/license.rst $O/LICENSE.mesa 2>/dev/null || true
  cp $W/Vulkan-Loader/LICENSE.txt $O/LICENSE.vulkan-loader 2>/dev/null || true
  echo "kosmickrisp: $O (x86_64; Mesa $(cat $M/VERSION) ${MESA_REV[1,10]}, Vulkan-Loader $VULKAN_REV)"
  exit 0
fi

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
