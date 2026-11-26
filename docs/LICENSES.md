# Licenses

- **MetalSharp Wine's own material** (scripts, docs, tests and build glue): [AGPL-3.0](../LICENSE), like MetalSharp.
- **Wine and the Wine changes:** LGPL-2.1-or-later. The notices of the third-party code Wine imports are in Wine's `NOTICES.md`, which both apps carry in `share/licenses/wine/`.
- **WineForge** (Radim Vesely; [Alien4042x/WineForge](https://github.com/Alien4042x/WineForge)): LGPL-2.1-or-later, like Wine.
  - The x86_64 build's Wine is WineForge's Wine 11.22 with MetalSharp's changes.
  - Both builds carry WineForge's launcher, game and macOS fixes, and msync, which is WineForge's USync 6, renamed.
  - Its notices are kept in the sources ([PROVENANCE.md](PROVENANCE.md)).
- **WFDXCompat** (WineForge's DirectX compatibility companion, Radim Vesely): LGPL-2.1-or-later. The apps carry its license in `share/licenses/wfdxcompat/` and next to its runtime.
- **The x86_64 build's native Steam bridge** (WineForge's, from Proton's `lsteamclient` with [NotProton](https://github.com/NotProtonNot/NotProton)'s macOS adaptations):
  - ntdll's bridge is LGPL-2.1-or-later.
  - `lsteamclient` comes with Valve's Steamworks SDK license, which its Steam API headers are under. The app carries it in `share/licenses/lsteamclient/`.
- **FEX and its fork:** MIT.
- **Mullion/DXMT:** LGPL-2.1-or-later.
- **VKD3D-Proton:** LGPL-2.1. **DXVK and DXVK-MacOS:** zlib; DXVK-MacOS's dxbc-spirv: MIT. **MoltenVK:** Apache-2.0.
- **WineMetalGL:** LGPL-3.0.
- **MetalVR:** MIT. It links ALVR's `server_core` (MIT), and builds OpenComposite (GPL-3.0) as a separate DLL that is not linked into it.
- **Everything else:** each redistributed component keeps its own license, and the app carries them all in `share/licenses/`.
- **Microsoft's VC++ and core-fonts installers and shader compilers (DXC, d3dcompiler_47)** are included unmodified under Microsoft's redistribution terms.
- **Apple's Metal Shader Converter and, in the x86_64 build, D3DMetal (Game Porting Toolkit)** are included unmodified and Apple-signed under Apple's license.
