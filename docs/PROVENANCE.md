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

## Other upstream projects

| Project | Authors | License | Source of what we ship |
| --- | --- | --- | --- |
| [Wine](https://www.winehq.org/) | The Wine project | LGPL 2.1 or later | `source/wine` branch; `patches/wine/` against the upstream tag |
| CrossOver macOS work | CodeWeavers | LGPL 2.1 or later (as part of Wine) | Same |
| [FEX](https://github.com/FEX-Emu/FEX) | FEX-Emu contributors | MIT | `source/fex` branch; `patches/fex/` |
| [Mullion](https://github.com/dttdrv/mullion), based on [DXMT](https://github.com/3Shain/dxmt) | Feifan He for CodeWeavers, Mullion contributors | LGPL 2.1 or later | aaf2tbz/mullion, branch `metalsharp-2.1` |
| [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) | Hans-Kristian Arntzen and contributors | LGPL 2.1 | metalsharp/VKD3D-Proton-MacOS |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK), SPIRV-Cross | The Khronos Group, Brenwill Workshop and contributors | Apache 2.0 | metalsharp/VKD3D-Proton-MacOS (`sources/MoltenVK`) |
| [DXVK](https://github.com/doitsujin/dxvk) | Philip Rebohle and contributors | zlib | Unmodified release |
| WineMetalGL | MetalSharp | LGPL 3.0 | `source/winemetalgl` branch |
| GStreamer, FreeType, GnuTLS, SDL2, FFmpeg and the other libraries in `deps/` | Their authors | Their own licenses | Unmodified upstream releases |
| DXC, d3dcompiler_47 | Microsoft | Microsoft redistribution terms | Unmodified redistributables |

Every bundle carries these licenses in `share/licenses/` and next to each runtime in `lib/` (for example `lib/mullion/LICENSE` and `lib/wfdxcompat/LICENSE`).

A release's binaries are built from the commits of the branches above at the time of that release, and the modified LGPL components' source is published there. Local debugging changes that aren't committed never go into a release build.
