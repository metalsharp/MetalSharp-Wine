# Building MetalSharp Wine

## Source

The modified sources are kept in this repository as branches, and `main` links them as submodules in `src/`, pinned to the commits each release is built from:

| Submodule | Branch | Contents |
|---|---|---|
| `src/wine` | [`source/wine`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wine) | Wine 11.20 + WineForge + MetalSharp (`wine-11.20` tag plus 139 commits) |
| `src/wine-x86_64` | [`source/wine-x86_64`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wine-x86_64) | **x86_64 build:** Wine 11.17 as ported by WineForge, plus 59 MetalSharp commits. `scripts/fetch-sources.sh x86_64` fetches it ([`docs/X86_64.md`](X86_64.md)) |
| `src/FEX` | [`source/fex`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/fex) | FEX-2610 + MetalSharp (34 commits). It is not upstreamed |
| `src/winemetalgl` | [`source/winemetalgl`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/winemetalgl) | WineMetalGL's arm64 Wine module |
| `src/wfdxcompat` | [`source/wfdxcompat`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wfdxcompat) | WineForge DXCompat, built ARM64EC for Mullion: the Rockstar launcher companion |
| `src/linuxhost` | [`source/linuxhost`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/linuxhost) | The Linux module host: runs the x86_64 Linux code Easy Anti-Cheat's EOS bootstrapper loads into itself. Developed in [metalsharp/Proton](https://github.com/metalsharp/Proton) (`darwin/linuxhost`) |
| `src/mullion` | [aaf2tbz/mullion](https://github.com/aaf2tbz/mullion) `metalsharp-2.1-main` | Mullion (DXMT fork). Developed there and sent upstream to [dttdrv/mullion](https://github.com/dttdrv/mullion) as pull requests |
| `src/dxvk-macos` | [metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS) `main` | DXVK-MacOS 3.1, DXVK 3.1.1 ported to MoltenVK: the `GRAPHICS_BACKEND=dxvk` lane, built as ARM64X |
| `src/vkd3d-proton` | [metalsharp/VKD3D-Proton-MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS) `main` | VKD3D-Proton macOS: the `GRAPHICS_BACKEND=vkd3d` lane (D3D12 on Vulkan, with DXVK's DXGI and D3D10/11) and MetalSharp's MoltenVK, as patches on the upstream projects |

The branches are full source trees with their upstream history (Wine's is large), which is why they are linked rather than merged into `main`. `legacy-1.x` holds the previous MetalSharp Wine (VKMT) history.

The same changes are in `patches/` as git patches against the upstream tags. `patches/wine/0042` is a pointer to WineForge's Steam bridge commit, because that commit vendors about 66 MB.

| Path | Contents |
|---|---|
| `scripts/` | Build, staging, test, audit, benchmark, packaging and signing scripts |
| `patches/` | `wine/`, `fex/`, `mullion/`, `winemetalgl/`, and `wine-x86_64/` (the x86_64 build, against WineForge `a05564cf`) |
| `resources/` | The app icon (`MetalSharp-Wine.icon`, an Icon Composer document) |
| `tests/src/` | Smoke tests and benchmarks (native ARM64, x86_64 and i386) |
| `docs/` | Status, testing, performance, the WineForge port plan, and provenance and licenses (`PROVENANCE.md`) |

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

It then audits the Mach-O files and packages `dist/metalsharp-wine-<version>-arm64`.

Packaging, so `build-all.sh` too, needs a macOS 15 SDK (`MACOS15_SDK=<path>/MacOSX15.x.sdk`). Some of the bundles' payloads were linked on a newer macOS without a deployment target. `scripts/macos-floor.py` uses the SDK to check that each one needs nothing macOS 15 lacks, then holds it to macOS 15. Both packagers run it.

The add-ons have their own scripts:
- `build-llvm15.sh` and `build-mullion.sh`
- `build-winemetalgl.sh`
- `build-wfdxcompat.sh` (after `build-mullion.sh`)
- `build-linuxhost.sh`
- `stage-moltenvk.sh` and `stage-vkd3d-proton.sh` (built in `src/vkd3d-proton` with its own scripts)
- `build-dxvk-macos.sh` then `stage-dxvk.sh`
- `stage-shader-compilers.sh`
- `fetch-addons.sh` (Mono and Gecko) and `fetch-fonts.sh`
- `build-innoextract.sh` and `build-cabextract.sh`
- `stage-metalsharp-assets.sh` and `stage-metalsharp-redist.sh`

**The x86_64 build** (`MetalSharp-Wine-x86_64.app`) has its own scripts, all named `*-x86_64.sh`: `build-wine-x86_64.sh`, `build-deps-x86_64.sh`, `build-mullion-x86_64.sh`, `build-winemetalgl-x86_64.sh` and `package-x86_64.sh`. The steps are in [`docs/X86_64.md`](X86_64.md).

`sign-app.sh` builds `MetalSharp-Wine.app` with the hardened runtime and compiles the icon (`actool`, from Xcode). It needs a Developer ID certificate and a provisioning profile with the cross-architecture entitlement.
