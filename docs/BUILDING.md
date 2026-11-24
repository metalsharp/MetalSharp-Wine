# Building MetalSharp Wine

## Source

The modified sources are kept in this repository as branches, and `main` links them as submodules in `src/`, pinned to the commits each release is built from:

| Submodule | Branch | Contents |
|---|---|---|
| `src/wine` | [`source/wine`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wine) | Wine 11.22 + WineForge + MetalSharp (`wine-11.22` tag plus 107 commits). `patches/wine` is the same work as patches against `wine-11.20`; the branch also merges Wine 11.22 and its macOS fixes (#64, #107) |
| `src/wine-x86_64` | [`source/wine-x86_64`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wine-x86_64) | **x86_64 build:** Wine 11.17 as ported by WineForge, plus 59 MetalSharp commits. `scripts/fetch-sources.sh x86_64` fetches it ([`docs/X86_64.md`](X86_64.md)) |
| `src/FEX` | [`source/fex`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/fex) | FEX-2610 + MetalSharp (34 commits). It is not upstreamed |
| `src/winemetalgl` | [`source/winemetalgl`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/winemetalgl) | WineMetalGL's arm64 Wine module |
| `src/wfdxcompat` | [`source/wfdxcompat`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/wfdxcompat) | WineForge DXCompat, built ARM64EC for Mullion: the Rockstar launcher companion |
| `src/linuxhost` | [`source/linuxhost`](https://github.com/metalsharp/MetalSharp-Wine/tree/source/linuxhost) | The Linux module host: runs the x86_64 Linux code Easy Anti-Cheat's EOS bootstrapper loads into itself. Developed in [metalsharp/Proton](https://github.com/metalsharp/Proton) (`darwin/linuxhost`) |
| `src/mullion` | [aaf2tbz/mullion](https://github.com/aaf2tbz/mullion) `metalsharp-2.1-main` | Mullion (DXMT fork). Developed there and sent upstream to [dttdrv/mullion](https://github.com/dttdrv/mullion) as pull requests |
| `src/metalvulkan` | [metalsharp/MetalVulkan](https://github.com/metalsharp/MetalVulkan) `main` | MetalVulkan: MetalSharp's MoltenVK (Vulkan 1.4 on Metal), and VKD3D-Proton (`GRAPHICS_BACKEND=vkd3d`, D3D12) and DXVK (`dxvk/`, with d7vk: DXGI and Direct3D 11 to 3) on it. VKD3D-Proton and MoltenVK are patches on the upstream projects; DXVK is in the repository, built as ARM64X and i386 for the dxvk lane and the vkd3d lane alike |
| `src/metalvr` | [metalsharp/MetalVR](https://github.com/metalsharp/MetalVR) `main` | MetalVR: an OpenXR runtime for Windows VR games on Mullion's Direct3D 11 (`metalvr_openxr.dll`), and `metalvrd`, the native server that composes and encodes their frames and streams them to a headset through ALVR. `scripts/build-metalvr.sh` builds it with its own scripts and installs it ([its `docs/METALSHARP_WINE.md`](https://github.com/metalsharp/MetalVR/blob/main/docs/METALSHARP_WINE.md)). No release has carried it yet |

The branches are full source trees with their upstream history (Wine's is large), which is why they are linked rather than merged into `main`. `legacy-1.x` holds the previous MetalSharp Wine (VKMT) history.

The same changes are in `patches/` as git patches against the upstream tags. `patches/wine/0042` is a pointer to WineForge's Steam bridge commit, because that commit vendors about 66 MB.

| Path | Contents |
|---|---|
| `scripts/` | Build, staging, test, audit, benchmark, packaging and signing scripts |
| `patches/` | `wine/`, `fex/`, `mullion/`, `winemetalgl/`, `mesa/` (applied by `build-kosmickrisp.sh`), and `wine-x86_64/` (the x86_64 build, against WineForge `a05564cf`) |
| `resources/` | The app icon (`MetalSharp-Wine.icon`, an Icon Composer document) |
| `tests/src/` | Smoke tests and benchmarks (native ARM64, x86_64 and i386) |
| `docs/` | Status, testing, performance, the WineForge port plan, and provenance and licenses (`PROVENANCE.md`) |

## Building

You need:
- Xcode 26 and its Metal Toolchain (`xcodebuild -downloadComponent MetalToolchain`).
- A macOS 15 SDK (`MacOSX15.x.sdk`, from Xcode 16), as `MACOS15_SDK`. Packaging uses it to hold the payloads that were linked on a newer macOS to macOS 15, after checking they need nothing newer (`scripts/macos-floor.py`).
- These Homebrew tools: cmake, ninja, meson, pkgconf, bison, flex, nasm, gettext, gperf, zstd, xz, osslsigncode and gh (signed in), plus llvm, spirv-llvm-translator and libclc for KosmicKrisp and Zink, and a Rust toolchain (`cargo`) for MetalVR.
- For an app that runs, a Developer ID certificate (`SIGN_IDENTITY`) and a provisioning profile with the cross-architecture entitlement (`PROFILE`, or the first one in `signing/`).

```bash
git clone --recurse-submodules https://github.com/metalsharp/MetalSharp-Wine.git
```

Or, in an existing clone:

```bash
zsh scripts/fetch-sources.sh
```

Then:

```bash
MACOS15_SDK=<path>/MacOSX15.5.sdk SIGN_IDENTITY=<Developer ID> zsh scripts/build-all.sh
```

That builds `dist/MetalSharp-Wine.app`, with everything the release carries, in this order (`build-all.sh --list`):
1. The toolchain (llvm-mingw 20261020, `fetch-toolchain.sh`), the dependencies, and the ARM64X runtimes.
2. Wine, then FEX.
3. Mullion, with its NGX core and NVAPI.
4. WineMetalGL, WFDXCompat, and the Linux module host.
5. The VKD3D-Proton lane with MetalSharp's MoltenVK and the DXBC converter (`build-vkd3d-proton.sh`), and DXVK (`build-dxvk-macos.sh`), both from `src/metalvulkan`.
6. The shader compilers, Mono and Gecko, the fonts, innoextract and cabextract.
7. The MetalSharp app's payloads, KosmicKrisp, Zink and MetalVR.
8. The tests.
9. The Mach-O audit, the package (`dist/metalsharp-wine-<version>-arm64`, and its tarball), and the signed app.

It checks the tools, `MACOS15_SDK` and `src/` before it starts. Each step logs to `logs/build-all/<n>-<step>.log`. When one fails, the run stops and names its log, and `build-all.sh <step>` carries on from there. `SKIP="metalvr zink"` leaves steps out.

Some steps take what this repository does not build:
- **Mullion's NGX core** (DLSS on MetalFX) has to carry Mullion's signature, so `build-mullion.sh` needs one of:
  - `MULLION_NGX_SIGNING_KEY_FILE`, the signing key.
  - `MULLION_SIGNED_NGX_DIR`, a signed core.

  Without either, `build-all.sh` fetches the newest core Mullion's `sign-ngx` workflow signed for an `src/mullion` whose NGX sources are the same (`fetch-signed-ngx.sh`, from `aaf2tbz/mullion`'s draft releases, so `gh` must be signed in as a collaborator). That script says how to sign one when none matches. `MULLION_NGX_UNSIGNED=1` builds without DLSS.
- **The MetalSharp app's payloads** (Goldberg, the Steam installer, FNA/XNA, Unity Mono, SDL3, the shims, the host runtime) come from MetalSharp's bundles in `downloads/metalsharp-bundles/extract` when they are there. Otherwise `fetch-metalsharp-assets.sh` takes them from a MetalSharp Wine release: from `fetch-metalsharp-assets.sh <MetalSharp-Wine.app>`, or, with no argument, from the 8.0 tarball (about 1 GB, checked against its SHA-256).

WineMetalGL's `opengl32.dll` is installed as the builtin `opengl32` (`lib/wine`), as Mullion's DLLs are, so a prefix's System32 copy is WineMetalGL's. Wine's own is in `lib/wine-opengl`, for `OPENGL_BACKEND=wine`. Run `build-winemetalgl.sh` again after `build-wine.sh install`, which puts Wine's back.

Each step is also its own script:
- `build-deps.sh`, `build-arm64ec-runtimes.sh`, `build-wine.sh` and `build-fex.sh`
- `build-llvm15.sh` (which `build-mullion.sh` runs when needed) and `build-mullion.sh`
- `build-winemetalgl.sh`
- `build-wfdxcompat.sh` (after `build-mullion.sh`)
- `build-linuxhost.sh`
- `build-metalvr.sh`: VR. `metalvr-prefix.sh` then makes MetalVR a prefix's OpenXR runtime
- `build-vkd3d-proton.sh`, which runs `src/metalvulkan`'s own scripts, then `stage-moltenvk.sh` and `stage-vkd3d-proton.sh`
- `build-dxvk-macos.sh` then `stage-dxvk.sh`
- `stage-shader-compilers.sh`
- `fetch-addons.sh` (Mono and Gecko) and `fetch-fonts.sh`
- `build-innoextract.sh` and `build-cabextract.sh`
- `fetch-metalsharp-assets.sh`, `stage-metalsharp-assets.sh` and `stage-metalsharp-redist.sh`
- `build-kosmickrisp.sh`, which fetches Mesa and applies `patches/mesa`, then `build-zink.sh`
- `build-tests.sh`, `audit-macho.sh`, `package.sh` and `sign-app.sh`

**The x86_64 build** (`MetalSharp-Wine-x86_64.app`) has its own scripts, all named `*-x86_64.sh`: `build-wine-x86_64.sh`, `build-deps-x86_64.sh`, `build-mullion-x86_64.sh`, `build-winemetalgl-x86_64.sh` and `package-x86_64.sh`. The steps are in [`docs/X86_64.md`](X86_64.md).

`sign-app.sh` builds `MetalSharp-Wine.app` with the hardened runtime and compiles the icon (`actool`, from Xcode). It needs a Developer ID certificate and a provisioning profile with the cross-architecture entitlement.
