<div align="center">

<img width="80" height="80" alt="MetalSharp Wine" src="https://github.com/user-attachments/assets/87d1a9ab-a9dd-46d6-ad4b-db646ef07b7a" />


# MetalSharp Wine

[![Release](https://img.shields.io/github/v/release/metalsharp/MetalSharp-Wine?style=for-the-badge)](https://github.com/metalsharp/MetalSharp-Wine/releases/latest)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-purple.svg?style=for-the-badge)](LICENSE)

</div>

___

MetalSharp Wine is Wine 11.20 built as a native arm64 macOS application for Apple Silicon. A FEX-Emu fork translates x86 Windows code to ARM64. In it's current state, it does not work as just an .app, you must use terminal commands to run it like Sakiragur. 

- **No Rosetta:** none of it is x86 code run through Rosetta.
- **No hardware TSO:** FEX emulates the x86 memory model with ordinary ARM64 instructions.

It runs ARM64, x86_64 and 32-bit x86 Windows programs, with Direct3D 9 through 12 and OpenGL 4.6 on Metal. It needs **macOS 15 or later**; i386 needs **macOS 26.4 or later**.

| Windows code | How it runs |
|---|---|
| ARM64 (aarch64) | natively |
| x86_64 | FEX through ARM64EC (`libarm64ecfex.dll`); Wine's own DLLs are native ARM64X hybrids |
| i386 | FEX through WoW64 (`libwow64fex.dll`), in the signed app (cross-architecture entitlement: low 4GB, 4K pages) |

## Download

[Releases](https://github.com/metalsharp/MetalSharp-Wine/releases) has:
- `MetalSharp-Wine-2.0-arm64.zip`: the signed and notarized `MetalSharp-Wine.app`. Use this one; i386 needs it.
- `metalsharp-wine-2.0-arm64.tar.xz`: the same Wine as a relocatable folder (`bin/wine`). It is ad-hoc signed, so x86_64 and ARM64 only.

## Use It: 

It's a lot like Sakiragur in its current state until it is included with MetalSharp where it's just a 'Play' option inside the launcher. 

1) _**Download the .zip and move the .app from inside into /applications.**_

___

### Terminal Commands: 

2) _**First point a shell variable at the Wine Inside the app:**_

`export WINE=/Applications/MetalSharp-Wine.app/Contents/MacOS/wine`

3) _**Create a prefix:**_

`WINEPREFIX=~/Games/my-prefix $WINE wineboot -i`

(It takes a minute the first time. The prefix comes with Wine Mono (.NET), Gecko and the core fonts already set up, and x86 programs run through FEX automatically). 

4) _**Run an installer or game by passing the .exe path:**_ 

`WINEPREFIX=~/Games/my-prefix $WINE ~/Downloads/setup.exe`

`WINEPREFIX=~/Games/my-prefix $WINE "C:\\Program Files\\MyGame\\Game.exe"` 

4.1) _**Running from inside the game’s folder also works and helps some games find their files:**_

`cd ~/Games/my-prefix/drive_c/Program\ Files/MyGame && WINEPREFIX=~/Games/my-prefix $WINE Game.exe` 

5) _**For Steam: run SteamSetup.exe (from Valve, or share/metalsharp/steam inside the app) the same way, then start Steam with:**_

`WINEPREFIX=~/Games/my-prefix $WINE "C:\\Program Files (x86)\\Steam\\steam.exe"` 

6) _**Useful switches, set before the $WINE command like WINEPREFIX:**_ 

* Direct3D: Mullion on Metal is the default. GRAPHICS_BACKEND=dxvk uses DXVK (x86_64 games only), and GRAPHICS_BACKEND=wine uses Wine’s own wined3d.
* OpenGL: WineMetalGL 4.6 is the default. OPENGL_BACKEND=wine switches back to Wine’s own.
* Sync: WINEMSYNC=1 turns on msync, which helps performance.
* Settings: WINEPREFIX=~/Games/my-prefix $WINE winecfg opens Wine’s settings.

## What's included

| Area | Details |
|---|---|
| Direct3D 10/11/12 | **Mullion** (DXMT fork) on Metal, the default. ARM64X, so native ARM64 and x86_64 programs use one build; there's also an i386 build (D3D10/11). `GRAPHICS_BACKEND=wine` selects wined3d |
| Direct3D 9/10/11 (opt-in) | **DXVK 1.10.3** for macOS (x86_64) on MoltenVK: `GRAPHICS_BACKEND=dxvk` |
| OpenGL | **WineMetalGL 3.6.0** (OpenGL 4.6 on Metal), the default. `OPENGL_BACKEND=wine` selects Wine's own |
| Vulkan | MoltenVK 1.4.3 (MetalSharp build) |
| Media | FFmpeg 8.0 (LGPL, VideoToolbox/AudioToolbox) and GStreamer 1.26 |
| .NET / HTML | Wine Mono 11.3.0 (arm64, x86, x86_64) and Wine Gecko 2.47.4 (x86, x86_64) |
| Sync | msync (shared-memory synchronization; `WINEMSYNC=1`) |
| Fonts | Metric-compatible Arial, Times New Roman and Courier New files (renamed Liberation) in every prefix's `C:\windows\Fonts`. CJK Windows families are mapped to the macOS system fonts |
| Controllers | winebus with SDL2 and IOHID (XInput and DirectInput) |
| Tools | `innoextract` 1.9 (Inno Setup / GOG installers) and `cabextract` 1.11 |
| Compatibility | WineForge's launcher fixes (Steam, Epic, EA, Ubisoft, Battle.net, Rockstar), PE patches, native fullscreen, GCMouse and the native Steam bridge |
| MetalSharp app payloads | `share/metalsharp/`: Goldberg, the Steam installer and wrapper, FNA/XNA, fnalibs, the FNA kickstart, Unity Mono runtimes, SDL3, shims, Terraria launchers, CEF hooks, `dxmt.conf`, compat rules, the host runtime, and Microsoft's unmodified VC++ 2015-2022 and core-fonts installers |

Results and design notes are in [`docs/STATUS.md`](docs/STATUS.md); measured performance is in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

## Source

The modified sources are kept in this repository as branches, and `main` links them as submodules in `src/`, pinned to the commits each release is built from:

| Submodule | Branch | Contents |
|---|---|---|
| `src/wine` | [`source/wine`](../../tree/source/wine) | Wine 11.20 + MetalSharp (`wine-11.20` tag plus 56 commits) |
| `src/FEX` | [`source/fex`](../../tree/source/fex) | FEX-2610 + MetalSharp (7 commits). It is not upstreamed |
| `src/winemetalgl` | [`source/winemetalgl`](../../tree/source/winemetalgl) | WineMetalGL's arm64 Wine module |
| `src/mullion` | [aaf2tbz/mullion](https://github.com/aaf2tbz/mullion) `i386-build` | Mullion (DXMT fork). Developed there and sent upstream to [dttdrv/mullion](https://github.com/dttdrv/mullion) as pull requests |

The branches are full source trees with their upstream history (Wine's is large), which is why they are linked rather than merged into `main`. `legacy-1.x` holds the previous MetalSharp Wine (VKMT) history.

The same changes are in `patches/` as git patches against the upstream tags. `patches/wine/0042` is a pointer to WineForge's Steam bridge commit, because that commit vendors about 66 MB.

| Path | Contents |
|---|---|
| `scripts/` | Build, staging, test, audit, benchmark, packaging and signing scripts |
| `patches/` | `wine/`, `fex/`, `mullion/`, `winemetalgl/` |
| `tests/src/` | Smoke tests and benchmarks (native ARM64, x86_64 and i386) |
| `docs/` | Status, testing, performance and the WineForge port plan |

## Building

You need Xcode, the Metal Toolchain, and these Homebrew tools: cmake, ninja, meson, pkgconf, autoconf, automake, libtool, bison, flex, nasm, gettext and gperf.

```bash
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp-Wine.git
```

Or, in an existing clone:

```bash
zsh scripts/fetch-sources.sh
```

```bash
zsh scripts/build-all.sh
```

That builds:
1. The dependencies.
2. The ARM64X runtimes.
3. Wine.
4. FEX.
5. The tests.

It then audits the Mach-O files and packages `dist/metalsharp-wine-2.0-arm64`.

The add-ons have their own scripts:
- `build-llvm15.sh` and `build-mullion.sh`
- `build-winemetalgl.sh`
- `stage-moltenvk.sh` and `stage-dxvk.sh`
- `fetch-addons.sh` (Mono and Gecko) and `fetch-fonts.sh`
- `build-innoextract.sh` and `build-cabextract.sh`
- `stage-metalsharp-assets.sh` and `stage-metalsharp-redist.sh`

`sign-app.sh` builds `MetalSharp-Wine.app` with the hardened runtime. It needs a Developer ID certificate and a provisioning profile with the cross-architecture entitlement.

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

The bundle is signed with the hardened runtime, as notarization requires. Wine maps Windows DLLs from disk as executable code, which the hardened runtime only allows through the `disable-executable-page-protection` entitlement. Without the entitlement, i386 would need a full guest address-translation layer.

**Unsigned builds, or macOS 15 through 26.3:**
- x86_64 and ARM64 programs work.
- The page size is 16K, so 4K-granular memory tests in Wine's test suite fail, as they do on Linux with 16K pages.
- Native ARM64 programs with 4K sections take the slower W^X emulation path.

## Licenses

- **MetalSharp Wine's own material** (scripts, docs, tests and build glue): [AGPL-3.0](LICENSE), like MetalSharp.
- **Wine and the Wine changes:** LGPL-2.1-or-later.
- **FEX and its fork:** MIT.
- **Mullion/DXMT:** LGPL-2.1-or-later.
- **WineMetalGL:** LGPL-3.0.
- **Everything else:** each redistributed component keeps its own license, and the app carries them all in `share/licenses/`.
- **Microsoft's VC++ and core-fonts installers** are included unmodified under Microsoft's redistribution terms.
