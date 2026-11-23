# Changes

The [release notes](https://github.com/metalsharp/MetalSharp-Wine/releases) have the full list for each release.

## 9.0

Both builds:

- **DLLs that an application's own file system serves now load** (#66). Some protectors and packers serve DLLs that are not on disk by hooking `NtOpenFile` and `NtCreateSection` and handing out their own handles. EXOCAD DentalCAD 3.3 and DentalDB load their main assemblies this way. Wine's loader asked the system about such a section, which failed, and then rejected the DLL as the wrong architecture (`0xc000007b`, or `0x800700c1` in .NET). It now reads the image's details from the mapped section, as Windows' loader does (Wine 0193, x86_64 0079).
- Mullion at `aaf2tbz/mullion` `metalsharp-2.1-main` `2a311095` (from `1581eb8a`), with dttdrv/mullion main merged again. Its NGX core is signed by the fork's CI for DLSS.
- **MetalVulkan 7.0** ([metalsharp/MetalVulkan](https://github.com/metalsharp/MetalVulkan), formerly VKD3D-Proton-MacOS) is MetalSharp's Vulkan: MoltenVK 1.4.3 with VKD3D-Proton, and now DXVK too, which moved in from DXVK-MacOS with its history (`dxvk/`). `src/metalvulkan` replaces `src/vkd3d-proton` and `src/dxvk-macos`. DXVK's `ResolveSubresource` can be detoured on this build too (Custom Shaders Patch), and its D3D9 constant updates, push data, spin waits and d7vk's vertex processing run on NEON.
- **MetalFX upscaling no longer holds games to 48 frames a second on a 144 Hz display.** With MetalFX on, a game's display modes are emulated and the screen keeps its own refresh, so a 60 Hz mode's frames waited three of its refreshes. They now wait two, and the game's own limit holds them to 60. The upscaled frame is also the display's size, where it was the window's size before going fullscreen.
- **DLSS gives MetalFX more of what games pass it**: the bias mask as MetalFX's reactive mask, inputs whose content starts inside their textures, and display-resolution motion vectors read directly (macOS 27). Render sizes are within the scales MetalFX takes on the device, and DLSS's statistics report the scaler's memory.
- **Direct3D 11 tessellates on Metal's tessellator** where it tessellates as Direct3D does, about twice as fast at high factors. A draw whose vertex shader reads what the pass's pixel shaders wrote no longer reads stale data.
- **Shader model 6.8** is reported, with its comparison sampling and start locations, and a 6.8 wave size range the GPU cannot run is refused.
- **Geometry shaders run several invocations a mesh threadgroup** where they fit, as Metal Shader Converter does: points-to-quads shaders about 10% cheaper on the CPU.
- NVAPI answers its ray tracing caps and extended acceleration structure builds. An adapter now has a device ID of its vendor's (AMD's 744c by default, where it was 0000), and `dxgi.customSubSysId` and `dxgi.customRevision` are new.
- New options: `d3d11.samplerLodBias` and `d3d12.samplerLodBias`, as DXVK's; `d3d12.ignoreFailedPipelines` (off by default), which gives a game a pipeline Mullion cannot make as one that draws nothing.
- Shader caches are rebuilt once (converter version 127).
- **WineMetalGL 4.1.0** (from 4.0.0): videos a game decodes on a thread of its own play (Call of Duty 4's Bink videos were pink, #58), City of Heroes' blend-state queries are answered, and image atomics work around a Metal bug on the M6.
- **MetalVR 0.2.5** (from 0.2.1), its latest release: MetalFX spatial upscaling of each eye, SteamVR rendering with the headset's own fields of view and IPD, the tracking sample each frame was rendered with, and Touch controllers without SteamVR's grip offset.

x86_64 build:

- **Ubisoft Connect's WebGL keeps the GPU MetalSharp starts it with** (metalsharp/MetalSharp#824): a Chromium browser whose command line already picks ANGLE's backend (`--use-angle=`) no longer gets MetalSharp Wine's software-drawing switches after it, whose `--use-angle=swiftshader-webgl` Chromium took instead. MetalSharp starts Connect with WebGL on ANGLE's D3D11 for the GPU its login's bot protection checks, and on 8.0 a fresh login was blocked. A browser started without `--use-angle=` gets the switches as before (x86_64 0077).

arm64 build:

- **The VKD3D lane (`GRAPHICS_BACKEND=vkd3d`) is one DXVK from Direct3D 12 down to Direct3D 3.** Its DXGI and Direct3D 11 and 10 were DXVK 2.7.1, while Direct3D 9 to 3 and 32-bit programs came from the DXVK lane's DXVK-MacOS. They are now all one DXVK, the same build as the DXVK lane's, with VKD3D-Proton for Direct3D 12. VKD3D-Proton's descriptor copies, spin waits and Metal Shader Converter argument pushes run on NEON, where the ARM64X build had no SIMD at all: CopyDescriptors' set scan is 3.6 to 4.6 times faster from 32 descriptors.
- **64-bit programs load `d3d8.dll`.** Wine built Direct3D 8 for 32-bit programs only, so a 64-bit one's `LoadLibrary("d3d8.dll")` failed on every lane: the DXVK and VKD3D lanes give DXVK's `d3d8.dll` only in place of a builtin the prefix has. It is now built for arm64 too: those lanes run it on DXVK-MacOS, and the Wine lane on wined3d (Wine 0196).
- **Counter-Strike 2 runs on Mullion.**
  - Its D3D11 renderer crashed 25 to 45 s in: a race in its own lock-free buffer pool, which emulation widens. It now runs its job pool on 4 threads (`-threads 4`, unless its command line names a count), which keeps it stable. With 6 or more threads it crashed (Wine 0195).
  - It no longer says the graphics driver is out of date. Direct3D 9 described a GeForce GTX 470 with a 2010 driver, while DXGI described Mullion's AMD GPU. For games on Mullion, Direct3D 9 now describes the same GPU as DXGI (Wine 0194).
- **Games with a lot of hot code start in seconds with multiblock on.** FEX's code buffer stopped growing at 128 MiB and discarded all compiled code whenever it was full. With multiblock, Overwatch's hot code alone was larger than that: the buffer was cleared 328 times during startup, and its window took about 110 s to appear (23 s with multiblock off). The buffer now grows to 1 GiB, which `FEX_MAXCODEBUFFERSIZE` (MiB) changes, and Overwatch's window appears after 26 s with multiblock on (FEX 0045).
- **WineMetalGL's `opengl32.dll` is the builtin `opengl32`**, as Mullion's DLLs are builtins: it is in `lib/wine`, so a prefix's System32 and SysWOW64 copies are WineMetalGL's where they were Wine's. Wine's own moves to `lib/wine-opengl`, which `OPENGL_BACKEND=wine` or `WINEMETALGL=0` still loads (Wine 0192).
- **Zink (`SharpGL_Switch=1`) on MoltenVK has a depth buffer and OpenGL 3.3.** Every depth/stencil image failed to allocate, so a window had none, and the first `wglMakeCurrent` raised `GL_OUT_OF_MEMORY`. Zink now also translates the 10:10:10:2 vertex formats Metal lacks, which held it to OpenGL 3.2 (`patches/mesa` 0002, 0003). OpenGL 4.0 still needs RGB32 texel buffers.
- `scripts/build-all.sh` builds the whole app, with everything the release carries, from a fresh clone. It checks what it needs first and fetches the toolchain, a signed Mullion NGX core and the MetalSharp app's payloads. Each step is logged, and a run picks up from the step that failed (`docs/BUILDING.md`).
- **Battle.net's login and main windows draw again** (#50): on 8.0 both stayed black on a fresh install, every time. Its pages loaded, but with the GPU in process alone Chromium composited them on the GPU, and that never reached the window. `Battle.net.exe` now gets `--in-process-gpu --disable-gpu-compositing` (WebGL keeps the GPU; the pages composite in software, as on the x86_64 build), whether its Launcher starts it or it is started directly (Wine 0190).

## 8.0

Both builds:

- **Two switches for other graphics drivers**, off unless set to `1`:
  - `SharpGL_Switch=1`: OpenGL through Mesa's **Zink** (OpenGL on Vulkan) in WineMetalGL's place (Wine 0175, x86_64 0072; `scripts/build-zink.sh`).
  - `SharpVK_Switch=1`: Vulkan through Mesa's **KosmicKrisp** (Vulkan on Metal, macOS 26 and later) in MoltenVK's place. The arm64 launcher points the Vulkan loader at it; the x86_64 Wine opens the Khronos loader the app now carries (x86_64 0074; `scripts/build-kosmickrisp.sh`).
- **Windowed mode's virtual desktop is a window you can move** (metalsharp/MetalSharp#805): a titled frame the desktop's size, whose windows move with it. `WINEMAC_DESKTOP_FRAME=0` leaves it at the screen's top-left as before (Wine 0177, x86_64 0073).
- **Windows function keys** (metalsharp/MetalSharp#802): `WINEMAC_FKEYS=windows` has the top row send F1-F12 while the program is active, and puts the keyboard's own setting back when it isn't (Wine 0177, x86_64 0073).
- **Steam no longer stops responding when SteamVR starts**: gdi32 exports `D3DKMTGetProcessSchedulingPriorityClass` and `D3DKMTSetProcessSchedulingPriorityClass`, whose absence crashed the 32-bit `steamservice.exe` (Wine 0179, x86_64 0075).
- **MetalVR 0.2.1** ships in both apps (`lib/metalvr`, `bin/metalvrd`): the OpenXR runtime and the server that streams VR games on Mullion to a headset.
- **Mullion alpha 4** ([dttdrv/mullion#297](https://github.com/dttdrv/mullion/pull/297) on `aaf2tbz/mullion` `metalsharp-2.1-main` at `f62d816`): native tessellation, pipelines and archives made at first draw, one pipeline archive per converter version, and the other merged PRs, and performance improvements: faster shader cache and pipeline loads, ray tracing pipelines converted once and compiled up front, compiles moved off the draw path, less work per frame, and DXIL results that match Direct3D's. Its NGX cores are signed by the fork's CI.
- **WineMetalGL 4.0.0**: Foundry Mari's viewport shaders, per-game settings in `winemetalgl.conf` beside a game's `.exe`, 100, 120 and custom frame rate limits, and `opengl32.dll`'s version resource SteamVR checks for.
- **DXVK-MacOS 3.1.1-macos.4**, on the MoltenVK of VKD3D-Proton-MacOS 6.0.
- WFDXCompat at `32e5e73`: the D3D11 frontend for D3DMetal shares resources within a process.

arm64 build:

- **x86_64 programs that ship their own x64 C runtime no longer crash in Wine's DLLs** (#57: EXOCAD's .NET WinForms, in `ImageList_Duplicate`). The compiler gave ARM64EC calls to `memcpy`, `memmove` and `memset` it makes itself no exit thunk, so when such a call reached x64 code it ran the module's first byte; 542 of 7.0's ARM64X modules had them. winecrt0 now gives each module the three exit thunks (Wine 0176).

Also in 8.0, from after 7.0:


x86_64 build. Assetto Corsa with Custom Shaders Patch runs and joins servers (Wine 11 regressions from Wine 10):

- **CSP initializes:** dbghelp's PDB reader returns public symbols again and names functions the linker folded by their own name; CSP finds Assetto Corsa's functions through them (x86_64 patch 0065).
- **Assetto Corsa starts:** d3dx11 creates shader resource views from files, memory and resources, and saves textures to memory and files, DDS with the DX10 header and every mip and layer (0064).
- **Shaders:** vkd3d-shader reads SM4 `call`, `callc` and label registers, and `D3DReflect` keeps a shader's resources when it can't scan its code (0066); `D3DCompile`'s standard includes are found next to sources named with either path separator, which CSP's sky, clouds and post-processing shaders are (0069).
- **Online:** winevulkan gives DXVK-MacOS functions that fail for Win32 handles of extensions MoltenVK lacks, instead of NULL, which it called for D3D11 shared resources (0067).
- **Dialogs over fullscreen:** a window a program opens while its fullscreen window is on screen (CSP's "Failed to launch the race") shows in front of it (0068).
- **D3DMetal:** WFDXCompat's new D3D11 frontend refuses block-compressed render targets as the D3D11 runtime does; D3DMetal created them, and CSP's track textures rendered with black squares (0070, `source/wfdxcompat`).

Both builds:

- Mullion merged with upstream [dttdrv/mullion](https://github.com/dttdrv/mullion) main (`aaf2tbz/mullion` `metalsharp-2.1-main` at `1581eb8a`), with nothing of upstream's left out; its NGX core is signed by the fork's CI for DLSS.
- **SteamVR games show in a headset** (Mullion `998c32a6`, [aaf2tbz/mullion#3](https://github.com/aaf2tbz/mullion/pull/3)). With `MULLION_METALVR` set, which MetalVR's setup does for the Steam prefix (`metalvr steamvr`), Mullion shares Direct3D 11 buffers and Direct3D 12 textures between processes: SteamVR's compositor learns from such a buffer which of a game's eye images is current, and without it a game ran but was black. Without the variable nothing changes.
- VKD3D-Proton-MacOS 5.4 → 6.0.

arm64 build:

- **32-bit programs read their clocks and `cpuid` again** ([#32](https://github.com/metalsharp/MetalSharp-Wine/issues/32)). Both broke when a program keeps the data it writes on a page with its code (one section, or DEP off), which FEX write-protects to catch self-modifying code.
  - A system call writing to such a page failed with an access violation, so `QueryPerformanceCounter`, `GetSystemTimeAsFileTime`, `GetSystemTime`, `GetLocalTime` and `time()` returned 0. Wine now makes the page writable for the call's write, then puts its protection back and tells FEX it changed (Wine patch 0173).
  - A fault on `mov` to or from an absolute address resumed at an earlier instruction, so `cpuid` ran again with EAX overwritten and answered for the wrong leaf (leaf 1's features read as the vendor string). That `mov` now gets its own resume point (FEX 0036).
- **Battle.net loads again** ([#50](https://github.com/metalsharp/MetalSharp-Wine/issues/50)). 7.0 gave it the software drawing arguments every Chromium browser gets, and with them it crashed in `vkCreateBuffer`. It gets `--in-process-gpu` alone again, as in 6.x (Wine patch 0174). The Mullion above (`959aa5ec`) fixes the 32-bit D3D11 path its login and store draw through, which 7.0's Mullion broke.
- **Faster Wine code on Apple Silicon (NEON)** ([#73](https://github.com/metalsharp/MetalSharp-Wine/pull/73), Wine 0182-0188). x86_64 programs reach this code through ARM64EC, so they get the speedups too. Results are bit-exact with the code it replaces.
  - **String and memory functions** in ntdll, msvcrt and ucrtbase: `strlen`, `wcslen`, `memchr`, `memcmp`, `strchr`, `strcmp` and `memmove`. They are 5-17x faster above 32 bytes; `strlen` of 1 KB from an x86_64 program is 240 -> 18 ns.
  - **UTF-8 conversion of ASCII text** (`MultiByteToWideChar`, `WideCharToMultiByte`) is 5-28x faster.
  - **GDI on 32-bit DIBs:**
    - HALFTONE scaling: 3.4x;
    - anti-aliased text: 3.5x;
    - AlphaBlend: 1.4-1.9x;
    - layered-window shape masks: 3x.
  - **WIC** premultiply, unpremultiply and BGR swaps: 4-7x.
  - **d3dx9/10/11:**
    - format conversion between 8-bit channel formats: about 100x;
    - box-filtered mipmaps of 8-bit UNORM formats: 17x.
  - **DirectSound** resampling to a higher rate: 3.2x.
- **Programs that use `rdtscp` or `rdpid` no longer stop with an illegal instruction** ([#70](https://github.com/metalsharp/MetalSharp-Wine/pull/70); Blender 5.3's Cycles render). Both now run and return the processor number Wine reports. CPUID still doesn't advertise them, because the call that answers them is slower (about 45 ns). `FEX_CPUINDEXINTPIDR=1` reads the number from the register macOS keeps it in instead: after a startup check of that undocumented layout, they cost about 8 ns and CPUID advertises them. If the check fails, they stay on the slower path (FEX patches 0043-0044, `tests/src/rdtscp.c`).
- **Overwatch no longer hangs in FEX** ([#65](https://github.com/metalsharp/MetalSharp-Wine/pull/65)): memory, read-file and self-modifying-code notifications and debug register reads no longer wait on the thread list's lock. FEX also writes its code into `MAP_JIT` memory when ntdll offers it, instead of flipping page protections (`METALSHARP_NO_MAPJIT=1` turns that off). Blocks in an image's volatile-metadata ranges no longer fall back to full TSO, and TSO emulation is off by default (`FEX_TSOENABLED=1` turns it back on per game) (FEX patches 0037-0042).

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
