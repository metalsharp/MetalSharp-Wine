# Provenance and licenses

MetalSharp Wine combines its own work with code from several projects. This file records where that code comes from, who wrote it, and the license it is under. The original copyright and license notices in each source file take precedence over this summary.

## WineForge

Much of the game and launcher compatibility work comes from [WineForge](https://github.com/Alien4042x/WineForge), written by Radim Vesely and licensed under the LGPL 2.1 or later, like Wine. WineForge itself builds on Wine and on CodeWeavers' CrossOver and macOS work.

MetalSharp Wine ported these WineForge changes onto its own Wine tree. The commit messages of those ports say "From WineForge/MetalSharp", which means "taken from WineForge, as carried in the MetalSharp runtime". They don't mean the code was written for MetalSharp. Its author is Radim Vesely, and the ports keep his copyright notices.

| Patch (`patches/wine/`) | WineForge origin | What MetalSharp Wine changed |
| --- | --- | --- |
| 0020 kernelbase launcher compatibility | Launcher policy for Steam, Epic, EA App, Battle.net, Ubisoft Connect and Rockstar; stable unchanged-refcount module lookup | Ported; later Chromium GPU-thread change (0056) is marked `MetalSharp:` |
| 0021 advapi32 Battle.net ACLs | Battle.net update-permission and managed-games ACL projection | Ported |
| 0022 services | Rockstar service pipe timeout, Epic EOS session GUID | Ported; comment updated for FEX (0028) |
| 0023 shell32 known folders | Rockstar known-folder pre-creation | Ported |
| 0024 ws2_32 Epic EOSH | Epic EOSH MainService progress handling | Ported |
| 0025 win32u composited paint | WM_PAINT fairness for WS_EX_COMPOSITED windows | Ported |
| 0026 winegstreamer | Fallback when D3D11 video textures are unsupported | Ported |
| 0030 winemac EA App redraw | EA App redraw after focus returns | Ported with authorship kept |
| 0033 server directory polling | Epic install directory change polling on macOS | Ported |
| 0034 server BUILTIN\Users access | Epic install directory ACLs | Ported |
| 0035 msync | WFUSync, WineForge's opt-in userspace synchronization backend | Renamed msync; arm64 and long-running-server fixes (0037, 0046, 0047, 0070) |
| 0038 winemac GCMouse | Elvin Hayatov's WineHQ MR !11799, ported to WineForge's Wine by Radim Vesely | Ported with authorship kept; coalescing change (0065) is ours |
| 0039 winemac native fullscreen | Native macOS fullscreen lifecycle | Ported with authorship kept |
| 0040 ntdll PE patch framework | Verified PE patch loader (Steam overlay) | Ported |
| 0042 native Steam bridge | Native macOS Steam client bridge | Ported with authorship kept; the patch file is a pointer because the commit vendors about 66 MB |
| 0048 Direct3D runtime routing | Graphics and launcher policy, DXMT runtime routing | Adapted to route to Mullion |
| 0058 WFDXCompat launcher companion | WFDXCompat D3D frontend loader and Rockstar launcher routing | Adapted to run on Mullion instead of D3DMetal |
| 0072 provenance notices | | Restores WineForge notices (see below) |

Some WineForge notices were rebranded on the way into MetalSharp. In particular, "Copyright (C) 2026 Radim Vesely for WineForge" became "... for MetalSharp", and the `WineForge-Internal`/`WineForge-Feature` provenance markers became MetalSharp ones. Patch 0072 restores the original notices in every affected file. Where MetalSharp Wine modified WineForge code, the file now says so next to the original notice. Patches before 0072 are kept as published, so 0020 and 0021 still show the rebranded header that 0072 corrects.

[WFDXCompat](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wfdxcompat), WineForge's DirectX compatibility companion by Radim Vesely, is carried with its LGPL 2.1 license. MetalSharp Wine's change to it (running the companion on Mullion as ARM64EC) is a separate commit on top.

## WineForge's Wine 11.17 (the x86_64 build)

MetalSharp Wine's x86_64 build ([X86_64.md](X86_64.md)) is built on Radim Vesely's own port of WineForge to Wine 11.17: [WineForge](https://github.com/Alien4042x/WineForge)'s `main` up to `a05564cf`, LGPL 2.1 or later. Branch `source/wine-x86_64` carries that history unchanged, with his authorship. MetalSharp's 59 commits sit on top of it (`patches/wine-x86_64/`, against `a05564cf`).

- **0001, "MetalSharp runtime as shipped":** the source of the Wine in MetalSharp's `bundles` release.
- **0002 to 0050, and 0052:** MetalSharp Wine's fixes, carried over from the arm64 build's patches. Two of them are by their original authors: 0043 (GCMouse raw input, Elvin Hayatov) and 0044 (native fullscreen, Radim Vesely).
- **0051 and 0053 to 0059:** the x86_64 build's own work.

The x86_64 build doesn't take CrossOver's source release separately. The CrossOver fixes listed below came over with MetalSharp Wine's patches: 0020 to 0028, 0048 and 0049.

## CrossOver

Some macOS display, painting and wined3d fixes come from the Wine in CodeWeavers' CrossOver 26.3 source release, which is LGPL 2.1 or later like Wine. CrossOver marks them as hacks with its bug numbers, and the ports keep those markers. Where MetalSharp Wine changed the code, the source says "Modified for MetalSharp Wine" next to it.

| Patch (`patches/wine/`) | CrossOver origin | What MetalSharp Wine changed |
| --- | --- | --- |
| 0088 winemac display modes | Hack 18576: modes without kDisplayModeSafeFlag on Apple silicon | On for every native arm64 build (CrossOver checks for Rosetta) |
| 0089 winemac focus events | Hack 18896: keep a window's focus event when Wine makes it key | Ported |
| 0090 winemac BC7 textures | Hack 23422: decode BC7 2D array textures for OpenGL, credited to nastys; bcdec by iOrange (MIT or Unlicense) | Decodes every layer, keeps edge blocks inside the rows, handles a failed allocation |
| 0091 win32u cross-process child windows | Hack 23950: shared-memory window surface for a child of another process's window | Adapted to Wine 11.20's window surfaces; no longer leaks a surface and its section per window |
| 0092 win32u painting | Hacks 20969, 23394, 23427: WM_NCPAINT as a notification, WM_ERASEBKGND with a timeout | Ported |
| 0093 wined3d driver properties | Vulkan driver ID query | Ported |
| 0094 wined3d viewports | Bug 22877: no multiple viewports on MoltenVK | Ported |
| 0095 wined3d render area | Hack 20098: render area of attachment-less draws trimmed to the viewport on MoltenVK | Viewport's sign and framebuffer bounds fixed |
| 0096 wined3d swapchain images | Bug 18838: three swapchain images on MoltenVK | Ported |
| 0097 wined3d swapchain size | Hack 23854: a swapchain of at least 1x1 | Ported |
| 0098 wined3d GL format check | Hack 18775: no framebuffer check for GL_RGB_422_APPLE cube maps | Ported |

0099 (Wine brought to the front when it shows a window), 0100 (wined3d's Vulkan renderer at feature level 10 and above on MoltenVK) 0101 (a NativeFullscreen Mac Driver option for WineForge's native fullscreen, off for ELDEN RING) and 0102 (a game's saved resolution fitted to the main display after it changed) are MetalSharp Wine's own.

## Proton

Patches 0141–0143 come from the port of Valve's [Proton](https://github.com/ValveSoftware/Proton) 11.0 to macOS in [metalsharp/Proton](https://github.com/metalsharp/Proton), branch `proton_11.0-macos`.

| Patch (`patches/wine/`) | Origin |
| --- | --- |
| 0141 per-game rules | Written for the port. The rules file it reads (`resources/appcompat.env`) is generated from Proton's launcher, `proton` (BSD-3-Clause, Valve Corporation), by metalsharp/Proton's `appcompat/extract.py` |
| 0142 EasyAntiCheat and BattlEye | The final state of 17 commits in [Valve's Wine](https://github.com/ValveSoftware/wine) (LGPL 2.1 or later), ported to Wine 11.20 as one commit; `programs/belauncher` is Valve's, unchanged. metalsharp/Proton's `ledger/` names the Valve commits |
| 0143 Linux system calls | Written for the port's Linux module host |

Patches 0144–0157 are fixes to MetalSharp Wine found by running Wine's test suite, and 0158 sets the version to 7.0.

## Other upstream projects

| Project | Authors | License | Source of what we ship |
| --- | --- | --- | --- |
| [Wine](https://www.winehq.org/) | The Wine project | LGPL 2.1 or later | `source/wine` branch; `patches/wine/` against the upstream tag |
| CrossOver macOS work | CodeWeavers | LGPL 2.1 or later (as part of Wine) | Same |
| Wine (x86_64 build), on [WineForge](https://github.com/Alien4042x/WineForge)'s Wine 11.17 | The Wine project, Radim Vesely | LGPL 2.1 or later | `source/wine-x86_64` branch; `patches/wine-x86_64/` against WineForge `a05564cf` |
| [FEX](https://github.com/FEX-Emu/FEX) | FEX-Emu contributors | MIT | `source/fex` branch; `patches/fex/` |
| [Mullion](https://github.com/dttdrv/mullion), based on [DXMT](https://github.com/3Shain/dxmt) | Feifan He for CodeWeavers, Mullion contributors | LGPL 2.1 or later | aaf2tbz/mullion, branch `metalsharp-2.1-main` |
| [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) | Hans-Kristian Arntzen and contributors | LGPL 2.1 | metalsharp/VKD3D-Proton-MacOS |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK), SPIRV-Cross | The Khronos Group, Brenwill Workshop and contributors | Apache 2.0 | metalsharp/VKD3D-Proton-MacOS (`sources/MoltenVK`) |
| [DXVK](https://github.com/doitsujin/dxvk) | Philip Rebohle and contributors | zlib | Unmodified release |
| WineMetalGL | MetalSharp | LGPL 3.0 | `source/winemetalgl` branch |
| GStreamer, FreeType, GnuTLS, SDL2, FFmpeg and the other libraries in `deps/` | Their authors | Their own licenses | Unmodified upstream releases |
| DXC, d3dcompiler_47 | Microsoft | Microsoft redistribution terms | Unmodified redistributables |
| Metal Shader Converter library (`libmetalirconverter.dylib`, 4.0 beta 3) | Apple Inc. | Apple's Software License Agreement for Metal Shader Converter (proprietary) | Unmodified and Apple-signed, from metalsharp/VKD3D-Proton-MacOS's `deps-metal-shaderconverter-4.0b3` release |
| D3DMetal (Game Porting Toolkit 4.0 beta 3; x86_64 build only) | Apple Inc. | Apple's Game Porting Toolkit license (proprietary, non-commercial distribution) | Unmodified and Apple-signed, from Apple's Game Porting Toolkit |

### Apple's Metal Shader Converter

`lib/libmetalirconverter.dylib` is Apple's Metal Shader Converter library. It is proprietary and the only closed-source component in the bundle. MoltenVK loads it at runtime to convert the DXIL shaders of the VKD3D-Proton lane (`GRAPHICS_BACKEND=vkd3d`) into Metal libraries, which skips compiling MSL text at run time ([MSC-BRIDGE-CONTRACT.md](MSC-BRIDGE-CONTRACT.md)). It is used for nothing else. `METALSHARP_MSC=0` turns it off, and if it is missing, shaders go through SPIR-V as before.

Apple's agreement, section 2B, allows installing, using, testing and distributing the library "for the sole purpose of shader conversion", which is what MoltenVK uses it for. The bundle ships only the library, as Apple published it:
- **Unmodified:** section 2E forbids modifying or reverse engineering it. `scripts/package.sh` leaves it out of its rpath rewriting and re-signing, and `scripts/sign-app.sh` keeps Apple's signature on it.
- **Apple hardware only, with its notices:** sections 2C and 2D limit it to Apple-branded systems, which is the only place MetalSharp Wine runs, and require its notices to be kept. Apple's agreement and acknowledgements ship in `share/licenses/metal-shaderconverter/`.

Apple's agreement governs; the details are in VKD3D-Proton-MacOS's README, under third-party licenses.

### Apple's D3DMetal (x86_64 build)

`MetalSharp-Wine-x86_64.app` carries D3DMetal from Apple's Game Porting Toolkit 4.0 beta 3: `D3DMetal.framework` and its Wine modules (`d3d11`, `d3d12`, `dxgi`, `nvapi64`, `nvngx-on-metalfx` and the rest), in `share/metalsharp/runtime/d3dmetal-gptk4-beta3`. It is proprietary, runs the `GRAPHICS_BACKEND=d3dmetal` lane only, and is not in the arm64 build.

Apple's Game Porting Toolkit license allows distributing it, with these conditions:
- **Non-commercial only (section 2A(iii)):** MetalSharp Wine is free, and D3DMetal is shipped with it for nothing.
- **Shipped whole (section 2C):** the framework may be distributed separately only in its entirety, and the redistributables individually. The app carries the framework whole.
- **Apple hardware only (section 2C):** MetalSharp Wine runs only on Macs.
- **Unmodified (section 2D):** the app doesn't modify or re-sign D3DMetal. It keeps Apple's signature, as `scripts/package-x86_64.sh` does for all Apple-signed code.
- **Notices kept (section 2A):** Apple's license and acknowledgements ship next to it and in `share/licenses/d3dmetal/`.

Apple's license governs.

Every bundle carries these licenses in `share/licenses/` and next to each runtime in `lib/` (for example `lib/mullion/LICENSE` and `lib/wfdxcompat/LICENSE`).

A release's binaries are built from the commits of the branches above at the time of that release, and the modified LGPL components' source is published there. Local debugging changes that aren't committed never go into a release build.
