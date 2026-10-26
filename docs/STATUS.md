# MetalSharp Wine arm64 + FEX: status

Last updated: 2026-10-23.

## Summary

| Area | State |
|---|---|
| Build | Wine 11.20 builds as native arm64 macOS (minimum 15.0), with PE archs arm64ec/aarch64/i386 and every requested feature (`scripts/build-all.sh`) |
| ARM64 Windows code | Runs natively: cmd, CPython 3.14, Wine's own programs and test suites |
| x86_64 Windows code | Runs through FEX-2610 (fork, ARM64EC). Checksums are bit-identical to native ARM64 in every benchmark; CPython x64 passes its workload |
| i386 Windows code | Runs through FEX (WoW64, `libwow64fex.dll`) in the signed `MetalSharp-Wine.app`, which has the cross-architecture entitlement (low 4GB, 4K pages). 211 conformance units, 99.996% of checks pass; every benchmark checksum matches native |
| Direct3D | D3D10/11/12 run on Metal through Mullion (DXMT fork, ARM64X) with `GRAPHICS_BACKEND=dxmt`. Tested with smoke tests: D3D11 on ARM64, x86_64 and i386, and D3D12 on ARM64 and x86_64. Mullion is the default when bundled; `GRAPHICS_BACKEND=wine` selects wined3d |
| Rockstar launcher (WFDXCompat) | Rockstar's `Launcher.exe` and `SocialClubHelper.exe` run on Mullion with WFDXCompat's launcher companion (`lib/wfdxcompat/aarch64-windows/wfdx-launchers-v1.dll`, ARM64EC, `scripts/build-wfdxcompat.sh`). ntdll loads it into those two processes only, after `d3d11.dll`, and it interposes D3D11 device creation. `WFDX_LAUNCHER_FLAG_NATIVE_D3D10` sends D3D10 queries to Mullion's own D3D10 device instead of the bridge D3DMetal needed. The default flags are 0x9; hex `WFDXCOMPAT_LAUNCHER_FLAGS` overrides them, and `WFDXCOMPAT_RUNTIME_DIR` defaults to `lib/wfdxcompat`. Tested 2026-10-23 with the Rockstar Games Launcher from a Steam prefix |
| D3D12 Agility SDK | `D3D12GetInterface(CLSID_D3D12SDKConfiguration)` returns an `ID3D12SDKConfiguration` whose `SetSDKVersion` succeeds. Mullion always uses its own `d3d12core.dll`. Test: Mullion `d3d12_sdk_configuration` |
| DXVK (opt-in) | `GRAPHICS_BACKEND=dxvk` runs DXVK-macOS 1.10.x (`src/dxvk-macos`, metalsharp/DXVK-macOS-1.10: Gcenx's DXVK-macOS with its D3D9-on-MoltenVK fixes, Gcenx/DXVK-macOS#21), built as ARM64X (`scripts/build-dxvk-macos.sh`, `scripts/stage-dxvk.sh`), for D3D9/10/11 and DXGI on MoltenVK, in x86_64 programs only (as ARM64EC under FEX). Hollow Knight: Silksong renders correctly at 144 fps on it. Native ARM64 and i386 programs, and D3D12, keep Mullion. Smoke tests: D3D11 (feature level 11_0) and D3D9 (pixel readback) on x86_64 |
| Fonts | Wine's fonts are bundled, and GDI also sees macOS's own core fonts (Arial, Times New Roman, Courier New, Verdana, Tahoma, …). New and updated prefixes get real files in `C:\windows\Fonts` (`arial*`, `times*`, `cour*`, `tahoma*`). Programs that open fonts there by file name need them: Unity's dynamic fonts left Moonscraper's input fields blank without them. The Arial, Times and Courier files are Liberation 2.1.5 built with the Windows family names, as Proton does (`scripts/fetch-fonts.sh`, `scripts/rename-font.py`; this also satisfies the OFL's reserved-name rule). A real Microsoft font in the prefix is never overwritten. `wine.inf` maps the CJK Windows families to Hiragino, Heiti, Songti and Apple SD Gothic Neo (`HKCU\Software\Wine\Fonts\Replacements`). Segoe UI is not mapped, and corefonts are an app download. Tests: `tests/src/fontinfo.c`, and Moonscraper on a fresh prefix |
| Controllers | winebus uses its SDL bus (bundled SDL2) plus IOHID, as MetalSharp #724 does, so Wine's own XInput/DirectInput builtins see controllers. MetalSharp's `lib/metalsharp` xinput/dinput DLLs are Wine 11.12 builtins, older than ours, and are not bundled |
| OpenGL | WineMetalGL 3.7.0 (OpenGL 4.6 on Metal), built for this arm64 Wine. It is the **default OpenGL** when bundled: ntdll serves WineMetalGL's `opengl32.dll` forwarder, so prefixes need no setup. `OPENGL_BACKEND=wine` or `WINEMETALGL=0` selects Wine's own (Apple's GL 2.1/4.1). The `glinfo` and `d3d9info` smoke tests pass on ARM64, x86_64 and i386. wined3d's D3D9 on it reports a DX11-class adapter |
| WineForge features | Launcher compatibility, msync, PE patches, native fullscreen, GCMouse and the native Steam bridge are ported. See `docs/WINEFORGE-PORT-PLAN.md` |
| No Rosetta / no hardware TSO | Holds. FEX is built without its unix library, so it never enables hardware TSO. Memory ordering is software-emulated (LRCPC/LSE); the ordering test reports 0 violations |
| Audit | Mach-O audit: arm64-only, minos ≤ 15.0, no newer libc symbols, no foreign library paths. Strict relocatable audit of `dist/` |
| Packaging | Relocatable `dist/` bundle with FEX, all dependencies, licenses and a `.tar.xz` with SHA-256. `scripts/sign-app.sh` wraps it as `dist/MetalSharp-Wine.app` (bundle name "MetalSharp Wine"; `wine --version` reports `wine-11.20-… (MetalSharp Wine 2.1)` from `src/wine/METALSHARP_VERSION`), signed with the Developer ID and provisioning profile. It is signed with the hardened runtime, so it can be notarized (`HARDENED=0` opts out) |

## Wine patches (`patches/wine/`, branch `metalsharp-arm64`)

1. **configure**
   - Native aarch64 macOS.
   - Keep the 4GB PAGEZERO; a smaller one is SIGKILLed.
   - Link the loader with `-platform_version macos 15.0 12.0`. Measured: x18 survives only when the main binary's SDK field is 12.0 or lower.
   - OpenGL through CGL, without EGL.
2. **winegcc**
   - Always pass the section alignment for ARM64EC.
   - The link probe always failed for ARM64X, so Wine's intended 64K section alignment was silently dropped.
   - wineboot dropped from 305s to 29s once modules were 64K-aligned.
3. **ntdll: shared user data and W^X**
   - **Relocation:** KUSER_SHARED_DATA moves to 0x17ffe0000 when 0x7ffe0000 isn't mappable. The low 4GB is probed without MAP_FIXED first.
   - **Publishing the address:** to PE code as `__wine_user_shared_data`, and to unix modules through `ntdll_get_user_shared_data()`.
   - **Load emulation:** ARM64 loads from 0x7ffe0000 are emulated in the SIGSEGV handler. Covered: LDR/LDUR/LDP, pre/post-index, register offset, SIMD, LDAR/LDAPR/LDAPUR.
   - **W^X:** W+X host pages are mapped RX and flip to RW on a write fault and back on an execute fault.
4. **ntdll: x86 images:** they get `VPROT_NOEXEC_HOST`, because x86 code is only executed by FEX. That avoids W+X conflicts on 16K pages.
5. **ntdll: store emulation:** a store into its own W+X host page can never complete by flipping, so it is emulated. Covered: STR/STP, STLR/STLUR, STXR/STXP and LSE atomics, serialized by `virtual_mutex`. Native ARM64 Microsoft binaries (python.exe) livelocked without this.
6. **ntdll: ID registers:** synthesized ARM64 ID registers (ISAR0-2, PFR0-1, MMFR1-2, MIDR, CTR) from `hw.optional.arm.FEAT_*`. EL0 can't read them, and even CTR_EL0 traps. FEX uses them to select fast paths.
7. **wine.inf:** `Wow64\amd64` and `Wow64\x86` default to `libarm64ecfex.dll` and `libwow64fex.dll`.
8. **dnsapi:** no multicast DNS lookup of the own `.local` hostname at process attach. That lookup had a 5s timeout, paid twice per cold start.
9. **NtFlushInstructionCache:** returns W+X pages to RX, so JITs don't pay an execute fault.
10. **4K-page exec:** on macOS 26+, an entitled process re-execs its loader with 4K pages, so Windows' page size is native and none of the 16K emulation (items 3-5, 9) is used. Forked children inherit the decision through a flag. `WINE_4K_PAGES=0/1` overrides. Without the entitlement the kernel kills a 4K exec, so unsigned builds stay on 16K pages.
11. **Low 4GB when entitled:** with `com.apple.developer.cross-architecture-support`, PAGEZERO becomes an ordinary no-access reservation from 0x4000 up to the main image. `virtual_init()` unmaps it below 4GB, then reserves the space for Windows. That gives KUSER_SHARED_DATA its real address (0x7ffe0000) and WoW64 its 32-bit address space. Unentitled processes have no such region, so nothing changes for them.
12. **winemac.drv: dock icons:** the icon entries are copied out of the user callback's result before they are converted. That result lives on the callback's stack, which the conversions reuse, so only the first few sizes survived and the dock scaled up a small one (blurry Rockstar icon).

## FEX fork (`patches/fex/`, branch `metalsharp` on FEX-2610)

1. **KUSER rebasing:** constant-address loads from a relocated KUSER_SHARED_DATA are rebased at JIT time, using the delta read from `__wine_user_shared_data`. 1.7µs → 0.3ns per read.
2. **`FlushCodeWrites()`:** called after JIT code writes, so Wine can restore RX without an execute fault. Cold JIT −20%, bench `jit` −25%.
3. **POP to memory:** RSP is no longer updated before a faulting store, so the instruction restarts cleanly.
4. **ARM64EC DF/AF:** DF and AF are carried through exception contexts.
5. **Remote thread termination:** terminations of threads in other processes are ignored.
6. **x87 exception flags:** all eight x87 status-word exception flags (IE..ES) now survive FLDENV, FRSTOR, FXRSTOR and the WoW64 context conversion. FNCLEX and FNINIT clear them all. Before, only IE survived, so `fetestexcept` on i386 read 0 (ucrtbase `misc` failed 32 checks; now 0).

### Build notes
- **No LTO:** lld can't resolve ARM64EC symbols from archives under ThinLTO, and LTO drops `BTCpuSimulateImpl` in the WoW64 build. FEX's own CI also builds without LTO.
- **Generic armv8 host tuning:** `-mcpu=apple-m1` triggers an LLVM ARM64EC SEH unwind bug. The JIT picks instructions from the detected CPU features regardless.
- **llvm-mingw tools first:** `scripts/env.sh` puts `toolchain/mingw-shims` (every `*-w64-mingw32-*` tool from llvm-mingw) ahead of Homebrew. Homebrew's GNU `i686-w64-mingw32-dlltool` used to win. It writes long-format import libraries, and lld silently ignores `-delayload` for those, so every i386 delay import became a load-time import. One result was an init cycle (ucrtbase → user32 → … → secur32 → ucrtbase) that crashed every 32-bit process.
- **llvm-mingw:** the release's aarch64 runtimes have no ARM64EC objects. `scripts/build-arm64ec-runtimes.sh` rebuilds the CRT, compiler-rt and libc++ as ARM64X using llvm-mingw's own scripts at the same tag.

## Test results

See `docs/TESTING.md` for the full conformance results. Every failure seen so far maps to one of these:

| Cause | Examples |
|---|---|
| 16K host pages | 4K-granular guard pages, region sizes, placeholder splitting, section-view offsets. Same as Linux on 16K-page kernels |
| No low 4GB (unsigned builds only) | zero_bits allocations, KUSER at 0x7ffe0000. The signed `MetalSharp-Wine.app` has neither problem |
| Environment | GPU/display tests (d3d9, dxgi, opengl32, vulkan-1) and dinput joystick/force-feedback tests time out or fail while another app holds the GPU and game controllers. In the last run that app was GeForce NOW |
| macOS platform gaps in upstream Wine | directory change notifications, thread affinity, working-set statistics |

Performance is in `docs/PERFORMANCE.md`.

## Open items

- **VC++ 2015-2022 runtime (deferred to the MetalSharp arm64 app):**
  - Not bundled in Wine. Wine already implements msvcp140*, vcruntime140*, vcomp140, concrt140 and vccorlib140.
  - What is missing is MFC (mfc140*, mfcm140*), vcamp140 and vcruntime140_threads.
  - The app will offer the official redistributable as a download at install and stage it into the Steam prefix, then copy it into new prefixes (GOG and others), unmodified and as native DLLs.

- **Notarization:** submit `MetalSharp-Wine.app` with `xcrun notarytool` once credentials are stored, then staple the ticket.
- **i386 needs the signed bundle.** Without the entitlement (macOS 15 to 26.3, or an unsigned build), i386 would need a guest address window across FEX and every WoW64 thunk (around 1,000 pointer-conversion sites). That isn't attempted.
- **i386 under 16K pages:** some gdi32 tests crash, with guest ESP clobbered by a host value. Irrelevant in practice, because i386 only runs entitled, and entitled means 4K pages.
- **Graphics/input units:** the dxgi, opengl32 and vulkan-1 test executables crash, and d3d9 `device` times out. These were already failing before the entitlement work.
- kernel32 loader test 7 is an upstream ARM64 test bug: non-dynamic-base ARM64 images are rejected by design. See TESTING.md.
- **JIT (16K pages only):** one write fault per compiled block remains. Removing it needs per-thread write scopes (MAP_JIT can't be combined with MAP_FIXED, and the toggle doesn't survive sigreturn) or a dual mapping.
- **Native ARM64 binaries with 4K sections (16K pages only):** they take the store-emulation path for data that shares a host page with code. In the signed bundle 4K pages remove this.

## Rebuild

```bash
zsh scripts/build-all.sh
```

```bash
WINE_DIST=dist/metalsharp-wine-2.1-arm64 zsh scripts/run-winetest.sh aarch64 ntdll kernel32
```

```bash
SIGN_IDENTITY="Developer ID Application: …" zsh scripts/sign-app.sh
```

```bash
WINE_APP=dist/MetalSharp-Wine.app WINETEST_PREFIX=prefixes/winetest-i386 zsh scripts/run-winetest.sh i386 ntdll kernel32
```

```bash
WINE_APP=dist/MetalSharp-Wine.app zsh scripts/run-bench.sh
```

- `build-all.sh`: deps → arm64ec runtimes → Wine → FEX → tests → audit → package.
- `run-winetest.sh`: conformance tests. Set `TEST_TREE=build/x64tests` and use arch `x86_64` to run the x64 tests under FEX.
- `sign-app.sh`: builds `dist/MetalSharp-Wine.app` and signs it with the hardened runtime and a secure timestamp. The provisioning profile is read from `signing/*.provisionprofile` (gitignored). Both loaders get the entitlements: `cross-architecture-support`, `allow-jit`, `allow-unsigned-executable-memory`, `disable-executable-page-protection` (Wine maps Windows DLLs from disk as executable) and `disable-library-validation`.
- `run-bench.sh`: writes `docs/PERFORMANCE.md`. With `WINE_APP` set, it adds the i386 column.

## MetalSharp bundles: what is in Wine and what stays in the app

In Wine:
- MoltenVK 1.4.3, Wine Mono and Wine Gecko.
- DXVK 1.10.3 x64 (opt-in).
- innoextract.
- The fonts.
- Our builds replace the bundled Mullion/DXMT, WineMetalGL, FAudio/XAudio2, xinput/dinput and the Vulkan portability patch.

**App payloads carried in `share/metalsharp/`** (`scripts/stage-metalsharp-assets.sh`, 134 MB) keep the bundles' layout; the MetalSharp app stages them into games and prefixes:
- `goldberg/` and `assets/goldberg`: two Goldberg Steam emulator builds.
- `steam/`: the Steam installer and the CEF wrapper.
- `assets/{fna-kickstart,xna,fnalibs,unity-mono,sdl3,shims,prebuilt-launchers}`.
- `tools/cef`: the CEF launcher hooks.
- `assets/wine/etc/dxmt.conf`.
- `etc/mscompatdb_rules.toml`: the compat rules, which already use `graphics_backend = "dxvk"`.

Notes on these payloads:
- The kickstart's i386 slices are removed.
- Some shims are x86_64-only Mach-O (fmod, CSteamworks, FNA3D, SDL3 shims), for the app's Rosetta routes.
- `sign-app.sh` signs every Mach-O under `share/metalsharp`.
- The bundles carry no license files for these payloads.

Also in `share/metalsharp/` (`scripts/stage-metalsharp-redist.sh`):
- `runtime/host`: the MetalSharp host runtime (arm64, ABI 1.0).
- `redist/vcrun2022`: Microsoft's unmodified VC++ 2015-2022 14.44.35211.0 installers. Their checksums are the SHA-256 values in Microsoft's versioned download URLs.
- `redist/corefonts`: the 11 unmodified core-fonts installers. Their checksums match winetricks.

Microsoft's licenses allow redistributing these unmodified installers; Wine itself doesn't use them.

Tested:
- `vc_redist.x64.exe` and `vc_redist.x86.exe /install /quiet /norestart` both exit 0 in a fresh prefix and install the real runtime, MFC included, for 64-bit and 32-bit programs. They are 32-bit bootstrappers, so they need the signed app.
- The corefonts setups don't run under Wine (exit 194). Like winetricks, unpack them with the bundled `bin/cabextract` (1.11, `scripts/build-cabextract.sh`), e.g. `cabextract -L -d <prefix>/drive_c/windows/Fonts arial32.exe`. That gives Microsoft's Arial 2.82, which replaces the Liberation file.

**Not carried:** `metalsharp_ntdll_hook.dll`.

**FFmpeg:** 8.0 (LGPL; videotoolbox and audiotoolbox) is bundled in `lib/` (libavcodec, libavformat, libavutil, libswresample, libswscale, libavfilter, libavdevice). Wine's `winedmo` links it by `@rpath`. Test `tests/src/mfdecode.c`: Media Foundation decodes AAC to PCM on arm64 and x86_64.

**Not usable on arm64:** D3DMetal/GPTK (x86_64 Mach-O), and WFDXCompat's D3DMetal-only parts. Only its launcher companion is carried; its D3D12 frontend for GTA V Enhanced is not ported. VKD3D-Proton is an opt-in D3D12 backend for x86_64 programs (`GRAPHICS_BACKEND=vkd3d`, from `src/vkd3d-proton`), alongside Mullion. The newer `lib/dxvk` with d3d8 is also left out.

## Checked against MetalSharp Wine 1.0 (VKMT)

1.0 had its own FEX provider inside Wine (`dlls/xtajit64/vkmt`), and most of its Java, Steam and no-TSO patches target that layer, which 2.0 doesn't have. So instead of porting code, each area was tested on 2.0 (2026-10-22, signed app, fresh prefix).

| Area | Result on 2.0 |
|---|---|
| Java | Works with no extra patches. Temurin 8 JRE x64 (8u492, Server VM) and i386 (8u472, Client VM). `java -version`, plus a Nashorn JIT stress script with hot loops, GC under allocation and 4 threads of atomics: correct results, 2.1 s on x64 and 1.4 s on i386 |
| MSI | Works. 7-Zip 25.01 x64 and x86 MSIs: silent install, AppSearch, registry, the installed program runs, uninstall. The VC++ and Mono/Gecko installs also go through msiexec |
| CEF / Chromium | Fixed in 2.0 (patch 0056). Without it, `cefclient` (CEF 109) runs JavaScript but its page stays blank: Chromium's GPU process can't draw into the browser's windows on macOS. A Chromium browser process (`libcef.dll`, `chrome_elf.dll` or `msedge_elf.dll` next to the exe) now gets `--in-process-gpu`, whether it's started directly or by another program. store.steampowered.com loads over HTTPS (one known glitch: the header menu labels are drawn over the page). 1.0's CEF wrapper and child hook are in `share/metalsharp/tools/cef`. Its Steam `chrome_elf` byte patch was pinned to one Steam build, for a 1.0-specific bug, and isn't needed |
| WebView2 | Works with MetalSharp's EVE Online flags (`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--no-sandbox --in-process-gpu --disable-gpu --disable-d3d11 --enable-unsafe-swiftshader --use-gl=angle --use-angle=swiftshader-webgl"`). The Fixed Version runtime 149.0.4022.98 navigates, and JavaScript and `ExecuteScript` work (`tests/src/webview2info.c`). Without the SwiftShader flags, the GPU channel fails (`DCompositionCreateDevice` is not implemented). Not a default yet |
| Electron | Not a target |

## innoextract

`scripts/build-innoextract.sh` builds innoextract 1.9 for arm64 (Boost 1.83 and xz 5.8.4 static, sources checked against boost.io and Homebrew). `package.sh` puts it in `bin/`, and the signed app has it at `Contents/MacOS/innoextract`. Tested 2026-10-22 on the Moonscraper Chart Editor 1.5.13 installer (Inno Setup 6.1): the integrity test passed and it extracted 84 MB. The extracted Unity x64 build ran through Mullion D3D11.

## Latest validation (2026-10-22, signed MetalSharp-Wine.app, 4K pages)

| | units | checks | pass |
|---|---|---|---|
| Native ARM64 | 286 | 21.64M | 99.995% |
| x86_64 via FEX (ARM64EC) | 211 | 20.72M | 99.995% |
| i386 via FEX (WoW64) | 211 | 20.78M | 99.996% |

- Every benchmark checksum and CPython result matches native on all three architectures ([PERFORMANCE.md](PERFORMANCE.md)).
- On an idle machine CPython runs at 1.67× native for x86_64 and 2.42× for i386. Startup: cold 482 / 495 / 525 ms, warm 25 / 31 / 64 ms (native / x86_64 / i386).
- i386 `atomic` is 8× because `InterlockedIncrement64` is a `lock cmpxchg8b` retry loop on a 32-bit CPU (8 contending threads). i386 `int` is 2.3× because 64-bit integer math on a 32-bit ISA takes more instructions. Neither is FEX overhead.
- Graphics/input, rerun with no other GPU user: d3d9 and d3d11 run, with failures that need DXMT/DXVK. dxgi, opengl32 and vulkan-1 still crash, as before today's changes, and remain open.
- No Rosetta, no hardware TSO.
