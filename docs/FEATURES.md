# What's in MetalSharp Wine

It runs ARM64, x86_64 and 32-bit x86 Windows programs, with Direct3D 9 through 12 and OpenGL 4.6 on Metal. Both builds need **macOS 15 or later**; i386 on the arm64 build needs **macOS 26.4 or later**.

| Windows code | How it runs |
|---|---|
| ARM64 (aarch64) | natively |
| x86_64 | FEX through ARM64EC (`libarm64ecfex.dll`); Wine's own DLLs are native ARM64X hybrids |
| i386 | FEX through WoW64 (`libwow64fex.dll`), in the signed app (cross-architecture entitlement: low 4GB, 4K pages) |

## Components

| Area | Details |
|---|---|
| Direct3D 10/11/12 | **Mullion** (DXMT fork) on Metal, the default. ARM64X, so native ARM64 and x86_64 programs use one build; there's also an i386 build (D3D10/11). `GRAPHICS_BACKEND=wine` selects wined3d |
| Direct3D 9/10/11 (opt-in) | **DXVK-MacOS 3.1.1-macos.3** ([metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS), DXVK 3.1.1 ported to MoltenVK, with MetalFX), built as ARM64X and run by x86_64 programs as ARM64EC under FEX: `GRAPHICS_BACKEND=dxvk` |
| Direct3D 12 (opt-in) | **VKD3D-Proton-MacOS 5.4** ([metalsharp/VKD3D-Proton-MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS)), ARM64X, with DXVK 2.7.1's DXGI and D3D10/11 (ARM64X too) for swap chains and D3D11On12, on MetalSharp's MoltenVK: `GRAPHICS_BACKEND=vkd3d` (x86_64 programs, and native ARM64 ones; D3D9 there comes from DXVK-macOS). Apple's Metal Shader Converter turns its compute, vertex, mesh, amplification and pixel shaders, DXIL and DXBC (through DirectXShaderCompiler's dxilconv, `libdxbcconverter.dylib`), into Metal libraries directly; geometry and tessellation shaders and what only SPIR-V can emulate stay on SPIR-V (`METALSHARP_MSC=0` turns the converter off, `METALSHARP_MSC=compute` keeps it to compute, `METALSHARP_MSC_DXBC=0` keeps DXBC shaders on SPIR-V) |
| Upscaling and DLSS | **MetalFX** upscaling from MetalSharp's MetalFX setting on every lane: Mullion, VKD3D-Proton, DXVK-MacOS and WineMetalGL. The game is shown the display divided by the upscale factor, and its saved resolution follows the main display. **DLSS** in games that ship it, run on MetalFX: Super Resolution on the temporal scaler, Ray Reconstruction on the denoising scaler, and Frame Generation on the frame interpolator (Mullion and VKD3D-Proton). Each lane has its own NGX core and NVAPI, and `winemac.conf` sets the GPU a DLSS game sees |
| Tracing | `MULLION_TRACE_<CATEGORY>=1` or `heavy` (Mullion: gpu, sync, resource, memory, shader, present, pass, api, cpu) and `METALSHARP_TRACE=<category>[+heavy]` (VKD3D-Proton and MoltenVK) write a log per game: `~/Library/Logs/Mullion/<exe>/mullion.log`, and `vkd3d.log` and `moltenvk.log` in `~/.metalsharp/logs/trace/<exe>/` |
| Shader compilers | Microsoft's DXC 1.9 (`dxcompiler.dll`, `dxil.dll`; ARM64 and x86) and the ARM64 `d3dcompiler_47.dll` are installed into every prefix, for programs that compile HLSL at run time |
| OpenGL | **WineMetalGL 3.9.0** (OpenGL 4.6 on Metal), the default, with MetalFX upscaling and a frame rate limit (`WINEMETALGL_MAX_FRAME_RATE`). `OPENGL_BACKEND=wine` selects Wine's own, whose Vulkan renderer now runs at feature level 10 and above on MoltenVK |
| Vulkan | MoltenVK 1.4.3 (MetalSharp build from VKD3D-Proton-MacOS: the pipeline cache is saved during long compiles and keyed on the shader sources, and compiled Metal libraries and binary archives are cached) |
| Media | FFmpeg 8.0 (LGPL, VideoToolbox/AudioToolbox) and GStreamer 1.26 |
| .NET / HTML | Wine Mono 11.3.0 (arm64, x86, x86_64) and Wine Gecko 2.47.4 (x86, x86_64) |
| Sync | msync (shared-memory synchronization; on by default, `WINEMSYNC=0` turns it off) |
| Fonts | Metric-compatible Arial, Times New Roman and Courier New files (renamed Liberation) in every prefix's `C:\windows\Fonts`. CJK Windows families are mapped to the macOS system fonts |
| Controllers | winebus with SDL2 and IOHID (XInput and DirectInput) |
| Tools | `innoextract` 1.9 (Inno Setup / GOG installers) and `cabextract` 1.11 |
| Compatibility | WineForge's launcher fixes (Steam, Epic, EA, Ubisoft, Battle.net, Rockstar), PE patches and the native Steam bridge. Rockstar's launcher runs on Mullion with WFDXCompat's launcher companion. `HideWineExports` hides Wine from programs that refuse to run under it (the PlayStation PC SDK in Sony's ports) |
| Displays and windows | Display modes Apple silicon reports without the "safe" flag are listed. A hung window no longer stops other windows painting, and child windows owned by another process are drawn. Wine comes to the front when it shows a window |
| Fullscreen and mouse | WineForge's **native macOS fullscreen** (macOS 27: fullscreen games get their own Space, and cursor confinement survives switching apps). ELDEN RING and MetalFX games stay out of it (the `NativeFullscreen` Mac Driver option). Fullscreen games keep the cursor in their window, and Cmd+left click toggles that for any window and **GCMouse raw input** (Elvin Hayatov's WineHQ MR !11799), with MetalSharp's fix-up: raw GCMouse moves the game hasn't read yet are summed, like ordinary mouse moves, instead of costing a wineserver round trip at every report of a high-polling-rate mouse (Wine patches 0038, 0039, 0065) |
| App bundle | Developer ID signed, hardened and notarized, with the cross-architecture entitlement. Windows programs run in an inner loader bundle (`lib/wine/aarch64-unix/wine.app`) that carries the provisioning profile, so they get 4K pages and the low 4GB |
| MetalSharp app payloads | `share/metalsharp/`: Goldberg, the Steam installer and wrapper, FNA/XNA, fnalibs, the FNA kickstart, Unity Mono runtimes, SDL3, shims, Terraria launchers, CEF hooks, `mullion.conf` and `winemac.conf`, compat rules, the host runtime, and Microsoft's unmodified VC++ 2015-2022 and core-fonts installers |

Results and design notes are in [`docs/STATUS.md`](STATUS.md); measured performance is in [`docs/PERFORMANCE.md`](PERFORMANCE.md).

## How the Mac-specific problems are solved

These are patches to Wine and FEX; see `docs/STATUS.md` for details.

- **x18 / TEB.** Windows ARM64 keeps the thread pointer (TEB) in x18, and macOS clears it on every syscall. Measured result: the kernel preserves x18 only when the main executable's build-version SDK is 12.0 or older. The loader is therefore linked with `-platform_version macos 15.0 12.0`.
- **4GB PAGEZERO.** Without an entitlement, nothing below 4GB can be mapped. KUSER_SHARED_DATA moves from 0x7ffe0000 to 0x17ffe0000.
  - Wine publishes the new address (`__wine_user_shared_data`).
  - ARM64 loads from the old address are emulated in the fault handler.
  - FEX rebases constant-address loads at JIT time: 0.3 ns per read instead of 1.7 µs.
- **16K pages and W^X.**
  - Wine's ARM64 modules are linked with 64K sections. This fixes a winegcc bug that silently dropped the alignment for ARM64EC.
  - x86 images never get host execute permission.
  - Pages that really need write and execute switch between RX and RW on demand.
  - Stores from code into its own page are emulated, as are LSE atomics and exclusive stores.
  - Wine's `NtFlushInstructionCache` and FEX's code-write notifications keep JIT overhead to one fault per compiled block.
- **CPU features.** macOS doesn't let EL0 read the ID registers, so Wine builds them from the `hw.optional.arm.FEAT_*` sysctls. FEX uses them to pick LSE, LRCPC, AFP and other fast paths.
- **Startup.** dnsapi used to look up the host's own `.local` name through multicast DNS at every process start (5 seconds). A cold start went from 10.5s to about 0.6s.

## Limitations

**i386 (32-bit x86) programs** need the low 4GB of address space. On macOS 26.4 and later Apple's `com.apple.developer.cross-architecture-support` entitlement provides it. `scripts/sign-app.sh` builds `dist/MetalSharp-Wine.app` with that entitlement, using your Developer ID certificate and a provisioning profile for the bundle's App ID. In that bundle:
- i386 works.
- KUSER_SHARED_DATA is at its real address.
- Every process runs with 4K pages, so none of the 16K workarounds are active.

macOS only grants that entitlement to an executable whose own bundle embeds the provisioning profile; on release builds of macOS 27.2 a standalone executable that claims it is killed at launch. Every Windows process runs in Wine's inner loader, so `sign-app.sh` gives the inner loader a bundle of its own (`lib/wine/aarch64-unix/wine.app`, with the profile), and Wine runs it from there.

The bundle is signed with the hardened runtime, as notarization requires. Wine maps Windows DLLs from disk as executable code, which the hardened runtime only allows through the `disable-executable-page-protection` entitlement. Without the entitlement, i386 would need a full guest address-translation layer.

**Unsigned builds, or macOS 15 through 26.3:**
- x86_64 and ARM64 programs work.
- The page size is 16K, so 4K-granular memory tests in Wine's test suite fail, as they do on Linux with 16K pages.
- Native ARM64 programs with 4K sections take the slower W^X emulation path.
