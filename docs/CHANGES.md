# Changes

The [release notes](https://github.com/metalsharp/MetalSharp-Wine/releases) have the full list for each release.

## Unreleased

x86_64 build. Assetto Corsa with Custom Shaders Patch runs and joins servers (Wine 11 regressions from Wine 10):

- **CSP initializes:** dbghelp's PDB reader returns public symbols again and names functions the linker folded by their own name; CSP finds Assetto Corsa's functions through them (x86_64 patch 0065).
- **Assetto Corsa starts:** d3dx11 creates shader resource views from files, memory and resources, and saves textures to memory and files, DDS with the DX10 header and every mip and layer (0064).
- **Shaders:** vkd3d-shader reads SM4 `call`, `callc` and label registers, and `D3DReflect` keeps a shader's resources when it can't scan its code (0066); `D3DCompile`'s standard includes are found next to sources named with either path separator, which CSP's sky, clouds and post-processing shaders are (0069).
- **Online:** winevulkan gives DXVK-MacOS functions that fail for Win32 handles of extensions MoltenVK lacks, instead of NULL, which it called for D3D11 shared resources (0067).
- **Dialogs over fullscreen:** a window a program opens while its fullscreen window is on screen (CSP's "Failed to launch the race") shows in front of it (0068).
- **D3DMetal:** WFDXCompat's new D3D11 frontend refuses block-compressed render targets as the D3D11 runtime does; D3DMetal created them, and CSP's track textures rendered with black squares (0070, `source/wfdxcompat`).

Both builds:

- Mullion merged with upstream [dttdrv/mullion](https://github.com/dttdrv/mullion) main (`aaf2tbz/mullion` `metalsharp-2.1-main` at `959aa5ec`), with nothing of upstream's left out; its NGX core is signed by the fork's CI for DLSS.
- **SteamVR games show in a headset** (Mullion `998c32a6`, [aaf2tbz/mullion#3](https://github.com/aaf2tbz/mullion/pull/3)). With `MULLION_METALVR` set, which MetalVR's setup does for the Steam prefix (`metalvr steamvr`), Mullion shares Direct3D 11 buffers and Direct3D 12 textures between processes: SteamVR's compositor learns from such a buffer which of a game's eye images is current, and without it a game ran but was black. Without the variable nothing changes.
- VKD3D-Proton-MacOS 5.4 → 6.0.

arm64 build:

- **32-bit programs read their clocks and `cpuid` again** ([#32](https://github.com/metalsharp/MetalSharp-Wine/issues/32)). Both broke when a program keeps the data it writes on a page with its code (one section, or DEP off), which FEX write-protects to catch self-modifying code.
  - A system call writing to such a page failed with an access violation, so `QueryPerformanceCounter`, `GetSystemTimeAsFileTime`, `GetSystemTime`, `GetLocalTime` and `time()` returned 0. Wine now makes the page writable for the call's write, then puts its protection back and tells FEX it changed (Wine patch 0173).
  - A fault on `mov` to or from an absolute address resumed at an earlier instruction, so `cpuid` ran again with EAX overwritten and answered for the wrong leaf (leaf 1's features read as the vendor string). That `mov` now gets its own resume point (FEX 0036).
- **Battle.net loads again** ([#50](https://github.com/metalsharp/MetalSharp-Wine/issues/50)). 7.0 gave it the software drawing arguments every Chromium browser gets, and with them it crashed in `vkCreateBuffer`. It gets `--in-process-gpu` alone again, as in 6.x (Wine patch 0174). The Mullion above (`959aa5ec`) fixes the 32-bit D3D11 path its login and store draw through, which 7.0's Mullion broke.

## 7.0

Both builds.

- Steam's window no longer freezes under FEX after it loads. Its web helper runs SwiftShader for WebGL only (`--use-angle=swiftshader-webgl`), as on the x86_64 runtime, instead of compositing Steam's UI through ANGLE on SwiftShader (Wine patch 0164).
- Ubisoft Connect no longer freezes under FEX. Its browser (`upc.exe`) gets the same arguments as Steam's web helper and draws its UI in software, instead of through ANGLE on Direct3D 11 (Wine patch 0165).
- Every Chromium-based program (CEF, Chrome, Edge/WebView2, and the Rockstar, EA and Battle.net launchers) draws its UI in software with the arguments Steam and Ubisoft Connect get, so none of them freezes under FEX the way those two did (Wine patch 0166).
- Programs built on Microsoft Edge WebView2, such as NoHesi Launcher, work on both builds:
  - **The runtime:** the first time such a program runs in a prefix, Wine installs the WebView2 Runtime from Microsoft with the bootstrapper the app now carries. This takes about a minute, once per prefix.
  - **No DirectComposition:** WebView2 draws without DirectComposition, which Wine lacks.
  - **x86_64:** the x86_64 build also gets the software drawing every Chromium browser gets under FEX.
  - **WebView2's window** (a child of its program's window, drawn from another process) now shows, with its cursor, on both builds (Wine patches 0167–0168, x86_64 0060–0061).
- Wine Mono 11.3.99, its main branch at `c019b64ba8` (11.3.0 and the 57 commits after it), on both builds. The packages are in MetalSharp Wine's [`wine-mono-11.3.99`](https://github.com/metalsharp/MetalSharp-Wine/releases/tag/wine-mono-11.3.99) release (Wine patches 0170, x86_64 0063).
- Assetto Corsa's launcher works on both builds:
  - **Its assemblies:** `HKCU\Software\Wine\AppDefaults\<program>.exe\Mono` `PrivatePath` adds folders a .NET program's assemblies are found in. MetalSharp sets `launcher\support` for Assetto Corsa, where the launcher keeps `CEF3.dll` (Wine patches 0169, x86_64 0062).
  - **Its culture and menus:** Wine Mono 11.3.99 lets it change its starting culture's number format, and its menus no longer freeze.
  - **Under FEX:** freed memory is no longer treated as DEP-promoted when it's reused. Mono's JIT code there had been left non-executable, so the launcher crashed in CEF startup (FEX 0035).

## 6.5

Both builds. Proton's Wine changes and the test-suite fixes are the arm64 build's; Mullion and VKD3D-Proton-MacOS 5.4 are both builds'.

- Proton's Wine. Programs Steam starts get Proton's per-game rules for 198 games (`share/proton/appcompat.env`), and Valve's EasyAntiCheat and BattlEye support is ported: the BattlEye launcher redirect and `belauncher.exe`, `PROTON_EAC_RUNTIME` and `PROTON_BATTLEYE_RUNTIME`, and the EasyAntiCheat bridge load order. Easy Anti-Cheat's EOS bootstrapper loads EAC's Linux module into itself, as under Proton, through the Linux module host (`lib/linuxhost`). With Elden Ring, EAC starts and keeps running and the game boots, but plays offline: EAC's in-game client never gets into the game. BattlEye has not been tested with a game. The port is [metalsharp/Proton](https://github.com/metalsharp/Proton), branch `proton_11.0-macos`.
- Fourteen fixes found by running Wine's test suite: network data received into write-watched memory (the .NET heap) was lost, four socket bugs could hang a program, socket options now have Windows' values and sizes, and copy-on-write protection of a read-only file view works. Native ARM64 failures in the six core modules went from 301 to 227, and every unit finishes ([`docs/TESTING.md`](TESTING.md)).
- Steam starts again under FEX after Steam's November 2026 update. Its web helper now gets `--disable-gpu-early-init`, which switched off all of Wine's Steam browser flags, so its sandboxed storage service crashed in a loop (Wine patch 0163).
- Mullion rebased onto upstream [dttdrv/mullion](https://github.com/dttdrv/mullion) main, on both builds, with its NGX core signed for DLSS.
- VKD3D-Proton-MacOS 5.3 → 5.4 on both builds.

## 6.0

- The x86_64 build, `MetalSharp-Wine-x86_64.app`: x86_64 Wine 11.17 under Rosetta 2 with every graphics lane, DLSS on MetalFX and Apple's D3DMetal (Game Porting Toolkit 4.0 beta 3 on macOS 27, 3.0 below).
- FEX runs x86 code more like an x86 CPU: x64 debug registers, code that protectors unlock on execute, undefined flags as Intel sets them, x87 state across exceptions, and the TSC's frequency in CPUID.
- Faster x86 callbacks and cold program starts.
- VKD3D-Proton-MacOS 5.3 with the Metal Shader Converter bridge, WineMetalGL 3.9.0 and a newer Mullion.
