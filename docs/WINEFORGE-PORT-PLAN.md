# Porting the WineForge (MetalSharp x86_64/Rosetta) patches to arm64 Wine + FEX

## Status (2026-10-22)

| Phase | State |
|---|---|
| 1. Launcher and app compatibility | **Done.** Also includes `server/change.c` directory polling, `file.c` `BUILTIN\Users`, and upstream WineForge's GCMouse raw input and native fullscreen. |
| 2. msync (`WINEMSYNC=1` or `WINEWFUSYNC=1`) | **Done, with the listed fixes.** It also fixes a wineserver use-after-free when abandoning msync mutexes, which upstream WineForge also has. Uncontended waits drop from about 15 µs to 0.5 µs. `ntdll:sync` and `kernel32:sync` pass on ARM64 and i386. |
| 3. x87 | **Done.** In FEX's WoW64 frontend, `ROSETTA_X87_PATH` maps to `X87ReducedPrecision` (fex 0007). |
| 4. PE patches, cross-process writes | **Done.** The Ubisoft i386 patch is kept, and `NtWriteVirtualMemory` notifies the target's FEX. |
| Steam bridge (`WINEFORGE_STEAM_BRIDGE=1`) | **Ported and builds.** `lsteamclient` is ARM64X plus i386 with an arm64 unix side. Not run here: it needs the native macOS Steam client and the WineForge-Steam hook. |
| 5. Direct3D | **Mullion (DXMT fork) is integrated.** D3D10/11/12 on Metal for native ARM64 and x86_64 (ARM64X build), and D3D10/11 for i386. It is the default when bundled (`lib/mullion`); `GRAPHICS_BACKEND=wine` opts out. |
| 5. WFDXCompat | Open. It was written for D3DMetal gaps; whether it is needed on Mullion is undecided. |
| 5. WineMetalGL | **Done.** The Wine module is built for arm64 hosts: ARM64X and i386 `winemetalgl.dll`/`opengl32.dll`, plus an arm64 `.so` running the 3.7.0 arm64 core. That took an `__x86_64__`-only exception reporter and a stubbed Rosetta x87 hook, which WineMetalGL 3.7.0 carries upstream (metalsharp/WineMetalGL#72). OpenGL 4.6 compatibility and core profiles work on ARM64, x86_64 and i386. It is the default OpenGL (`OPENGL_BACKEND=wine` or `WINEMETALGL=0` opts out). `scripts/winemetalgl-prefix.sh` still sets it up per prefix for other Wines. |


**Source:** WineForge's merged tree (`merged-wine-11.17`, CrossOver 26.3 sources plus WineForge), diffed against upstream `wine-11.17`. That gives 86 files and about 13.8k diff lines.

**Target:** this tree, Wine 11.20 on native arm64. x86_64 runs under FEX ARM64EC and i386 under FEX WoW64. There is no Rosetta.

**Rule of thumb:**
- Anything that patches or works around **Rosetta** or the **x86_64 unix side** is either dropped or reimplemented in FEX.
- **Windows-side behaviour** carries over: app and launcher compatibility, sync, and policy.

## Not portable: D3DMetal

GPTK 4.0 beta 3 ships D3DMetal as **x86_64-only Mach-O**: `D3DMetal.framework`, `libd3dshared.dylib`, its `libdxcompiler`/`libmetalirconverter` dylibs, and every `x86_64-unix/*.so`. All of them were checked with `lipo`.

An arm64 Wine process can't load them, and FEX only translates Windows PE code, not macOS Mach-O. Without Rosetta, D3DMetal is unusable until Apple ships arm64 slices. DirectX therefore goes through **DXMT** (D3D10/11 → Metal), **DXVK + MoltenVK** and **vkd3d(-proton)**. All of these are open source and can be built for arm64. DXMT is vendored at `MetalSharp-src/vendor/dxmt` and already has a `build-arm64ec.txt`.

## Phase 1: host-independent compatibility (port now)

| Feature | Files |
|---|---|
| Launcher CEF arguments (Steam swiftshader/in-process-gpu, SocialClub, EA, Battle.net, Ubisoft) | `kernelbase/launchers.c`, `process.c` |
| Epic: `r.CEFGPUAcceleration=0` in `BaseWindowsEngine.ini` | `kernelbase/main.c` |
| D2R: unchanged-refcount `GetModuleHandleEx` through `LdrGetDllHandle` | `kernelbase/loader.c` |
| Battle.net ACL projection | `advapi32/launcher_security.c`, `security.c` |
| Epic EOSH MainService localhost shim | `ws2_32/socket.c` |
| services: pipe timeout 10 s → 40 s; `EOS_SESSION_GUID` from `--clientId=` | `programs/services` |
| Pre-create `KFDF_PRECREATE` known folders | `shell32/shellpath.c` |
| WM_PAINT fairness for `WS_EX_COMPOSITED` | `win32u/message.c` |
| Refuse the D3D manager when NV12/P010 textures are unsupported | `winegstreamer/video_decoder.c` |
| FAudio heap fixes not already in FAudio 26.10 | `libs/faudio` |
| winemac: release stuck modifiers after Cmd-Tab, keep the global input source, redraw EA after focus | `winemac.drv` keyboard/window/cocoa_window |
| wgl: window-only `ChoosePixelFormat` filter, `glTexImage3DMultisample` alias | `opengl32/wgl.c` |

## Phase 2: msync (`WINEMSYNC=1`, opt-in)

msync is the same thing as WFUSync. It keeps events, mutexes and semaphores in POSIX shared memory and waits with `os_sync_wait_on_address`/`__ulock`, falling back to the wineserver. All shared state uses SEQ_CST atomics, so it is safe on arm64's weak memory model.

**Port:**
- `ntdll/unix/msync.{c,h}` and `server/msync.{c,h}`
- the hooks in `sync.c`, `inproc_sync.c`, `event`/`mutex`/`semaphore.c`
- the five new server requests: re-add them to `protocol.def` and regenerate with `tools/make_requests`
- keep upstream's ntsync `get_inproc_device_fd()` checks, which WineForge removed

**Fixes to apply while porting:**
1. Size the shm object once at creation. XNU only allows one `ftruncate`, so growth silently fails past the first page of slots.
2. Remove or saturate the unused `ushort refcount`. Today it asserts the server after 65,536 fd fetches.
3. Bound the server's spin on a lock held by a client, and fall back. Otherwise a suspended or dead holder hangs the wineserver.
4. Reuse freed slots.
5. Use a fixed 64K mapping granule on both sides: the server runs with 16K pages, clients with 4K.
6. Use relaxed atomics instead of plain accesses, and a yield hint in the spin loops.

The D3DMetal SysV wrappers in `sync.c`/`server.c`/`syscall.c` are dropped.

## Phase 3: x87 (replaces the Rosetta x87 sidecar)

The sidecar made Rosetta run x87 as fast double-precision code. FEX already has the same mode:
- **`X87ReducedPrecision=1`** uses double-precision host math.
- **The default** is full 80-bit precision through softfloat, which is accurate but slow.

**Mapping:**
- The old `ROSETTA_X87_PATH` (set for the D3D9 pipeline, i386) becomes `FEX_X87REDUCEDPRECISION=1`, or a per-exe `AppConfig/<exe>.json`.
- Wine needs no changes for this.

**Optional FEX work:** the sidecar kept *moves* exact (`fld m80`/`fstp m80`, `fild`/`fistp` of large integers), while FEX's reduced mode rounds them. Fixing that means keeping F80 slots for pure moves in `X87F64.cpp` and `x87StackOptimizationPass.cpp`.

## Phase 4: PE patch framework and process quirks

- **`ntdll/pe_patches.c`:** byte patches applied after image load. The invariant stays: never patch Wine builtins. Our builtins are ARM64EC/ARM64X; the targets are third-party x86 images run by FEX.
  - Keep the Ubisoft i386 `libcef` command-line gate. It compiles into the real i386 ntdll that runs under WoW64.
  - Extend the gate to `__arm64ec__`.
  - Drop the Steam-overlay patch, which only fixes D3DMetal's `dxgi.dll`.
- **Battle.net CEF renderer:** report WRITECOPY as RW in `NtProtectVirtualMemory`, behind the launcher policy. Retest whether it is still needed.
- **Optional:** send `CrossProcessMemoryWrite` after a remote `NtWriteVirtualMemory`, so FEX flushes code that injectors write.

## Phase 5: graphics backends (with DXMT/DXVK/vkd3d)

- **Graphics policy:** port `graphics_policy`, `launcher_policy` and the `dxmt_loader`/`loadorder` routing, retargeted to `aarch64-unix` plus `arm64ec`/`i386` PE directories. This keeps `GRAPHICS_BACKEND` (`wine`|`dxmt`|`dxvk`), `DXMT_RUNTIME_DIR` and the exe-path policies.
- **DXMT:** build it as arm64 unix `winemetal.so` plus ARM64EC and i386 PE DLLs.
- **winemac:** add the `macdrv_functions` surface export if DXMT needs it.
- **WFDXCompat:** rebuild it with `arm64ec-w64-mingw32-clang` (pure C, no asm). Point its `wfdxbackend-d3d12` at vkd3d-proton and its Rockstar D3D11 companion at DXMT. Port the loader hooks (`find_dll_file` alias, export interposition, `__wine_get_wfdx_d3d10_create_device`) behind `WFDXCOMPAT_RUNTIME_DIR`.
- **WineMetalGL:** an arm64 build exists (`wmgl-release/winemetalgl-2.0.0-arm64-core`). Port the `winemac/opengl.c` sidecar hook and the persistent-map path.
- **Epic Metal-view mask and Steam CEF native `vulkan-1` gate:** these only matter once a Metal-backed renderer exists.

## Dropped

| Item | Why |
|---|---|
| D3DMetal loader, client surface glue, SysV wrappers, `libd3dshared` code regions, `__wine_unix_call` export, fake atidxx64/nvapi64/nvngx DLLs | D3DMetal is x86_64-only (see above) |
| Steam-overlay dxgi thunk patch | Fixes D3DMetal's dxgi only |
| GSBase switch, TSD mirror, `localtime` hook, PEB-slot clear (`MS_GSBASE_SWITCH`) | FEX ARM64EC keeps `gs` = the real TEB, and the arm64 unix side never uses x86 GS |
| CW hacks: CET NOP, XGETBV, debug-register fake success, W+X reprotect, Rosetta retranslate, LDT refusal, wow64cpu `lcall` thunk | Rosetta-specific, or already handled by FEX or by our W^X patches |
| freetype `@loader_path` fallback, `pipe2` cloexec | Packaging and macOS-version issues already handled here |
| win32u extension-index fix, Vulkan portability enumeration | Already upstream, or already our patch 0014 |

## Validation

These are smoke tests, not full conformance runs:
- `tests/bin/hello-*` on all three architectures.
- Targeted single units per phase: `ntdll:sync` and `kernel32:sync` with `WINEMSYNC=0/1` for Phase 2, and the ucrtbase/x87 test plus `bench fp` with `X87ReducedPrecision` for Phase 3.
- Small msync benchmarks: event ping-pong and uncontended mutex.
