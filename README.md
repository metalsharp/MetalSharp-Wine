<div align="center">

<img width="80" height="80" alt="MetalSharp Wine" src="https://github.com/user-attachments/assets/87d1a9ab-a9dd-46d6-ad4b-db646ef07b7a" />

# MetalSharp Wine

[![Release](https://img.shields.io/github/v/release/metalsharp/MetalSharp-Wine?style=for-the-badge)](https://github.com/metalsharp/MetalSharp-Wine/releases/latest)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-purple.svg?style=for-the-badge)](LICENSE)

**Run Windows games and programs on Apple silicon Macs.**

The open-source Wine behind [MetalSharp](https://github.com/metalsharp/MetalSharp).

</div>

---

## What it is

MetalSharp Wine is [Wine 11.22](https://www.winehq.org/) built as a native macOS app for Apple silicon. It runs Windows programs directly, with their graphics drawn through Apple's Metal.

- **64-bit and 32-bit Windows programs.** In the main (arm64) app, ARM64 programs run natively and x86 ones are translated by [FEX](https://github.com/FEX-Emu/FEX), natively on Apple silicon.
- **Direct3D 3 to 12 and OpenGL up to 4.6**, on Metal.
- **Upscaling and DLSS:** MetalFX upscaling on every graphics backend, and DLSS in games that ship it, run on MetalFX.
- **Ready to play:** .NET (Wine Mono), the core fonts and the Visual C++ runtime installers come with it. Steam, Epic, EA, Ubisoft, Battle.net and Rockstar launchers have fixes built in.

> **Early access.** It is improving quickly, and bug reports and contributions are welcome.

## Requirements

- A Mac with Apple silicon (M1 or later)
- **macOS 15** or later
- **macOS 26.4** or later for 32-bit Windows programs

## Download

Get it from the [latest release](https://github.com/metalsharp/MetalSharp-Wine/releases/latest). There are two apps:

| Download | What it is | Pick it if… |
|---|---|---|
| `MetalSharp-Wine-<version>-arm64.zip` | `MetalSharp-Wine.app`: native Apple silicon, x86 code through FEX | Most people. This is the main build. |
| `MetalSharp-Wine-<version>-x86_64.zip` | `MetalSharp-Wine-x86_64.app`: x86_64 Wine under Rosetta 2, with Apple's D3DMetal | You want D3DMetal, or a game runs better under Rosetta. See [docs/X86_64.md](docs/X86_64.md). |

## Install

1. Download the `.zip` and unzip it.
2. Move the app to your **Applications** folder.

**With MetalSharp,** that's all: MetalSharp starts the app when you press **Play**, and MetalSharp's **FEX | x86_64** toggle picks which app it uses.

**On its own,** you run the app from **Terminal**, as below.

## Graphics lanes

Each program runs on one graphics lane. Pick a lane with `GRAPHICS_BACKEND`, set on the command that starts the program.

| Lane | Supports | Route |
|---|---|---|
| **Mullion** | Direct3D 10, 11 and 12 | `GRAPHICS_BACKEND=dxmt` (the arm64 app's default) |
| **VKD3D-Proton** | Direct3D 12, with DXVK for Direct3D 3 to 11 | `GRAPHICS_BACKEND=vkd3d` |
| **DXVK** | Direct3D 3 to 11 | `GRAPHICS_BACKEND=dxvk` |
| **D3DMetal** (x86_64 app) | Direct3D 10, 11 and 12 | `GRAPHICS_BACKEND=d3dmetal` |
| **WineMetalGL** | OpenGL 1.0 to 4.6 | Used for OpenGL by default |

## Run a program

Set `WINE` to the app you installed:

```bash
export WINE=/Applications/MetalSharp-Wine.app/Contents/MacOS/wine
# or, for the x86_64 app:
export WINE=/Applications/MetalSharp-Wine-x86_64.app/Contents/MacOS/wine
```

**1. Create a prefix.** A prefix is a folder holding its own Windows install. This makes one in `~/Games/my-prefix`; it takes a minute or two.

```bash
WINEPREFIX=~/Games/my-prefix "$WINE" wineboot -i
```

**2. Run an installer or program in it** on the lane you want:

```bash
WINEPREFIX=~/Games/my-prefix GRAPHICS_BACKEND=vkd3d "$WINE" ~/Downloads/setup.exe
```

**3. Run an installed program** from its own folder, which helps it find its files:

```bash
cd ~/Games/my-prefix/drive_c/Program\ Files/MyGame
WINEPREFIX=~/Games/my-prefix GRAPHICS_BACKEND=dxmt "$WINE" Game.exe
```

**To quit** everything running in the prefix:

```bash
WINEPREFIX=~/Games/my-prefix "${WINE%/wine}/wineserver" -k
```

Every other setting is in [docs/USAGE.md](docs/USAGE.md).

## Reporting problems

Open the issue where the fix will be made:

| The problem | Where to report it |
|---|---|
| A game draws wrong on **Mullion** | [dttdrv/mullion](https://github.com/dttdrv/mullion/issues) |
| A game draws wrong on **VKD3D-Proton** or **DXVK** | [metalsharp/MetalVulkan](https://github.com/metalsharp/MetalVulkan/issues) |
| An OpenGL game draws wrong on **WineMetalGL** | [metalsharp/WineMetalGL](https://github.com/metalsharp/WineMetalGL/issues) |
| Anything else | [this repository's issues](https://github.com/metalsharp/MetalSharp-Wine/issues) |

It helps to include your Mac model, macOS version, the MetalSharp Wine version, the game, and what you tried.

## Documentation

| Page | What's in it |
|---|---|
| [What's new](docs/CHANGES.md) | Changes in each release |
| [Usage](docs/USAGE.md) | Running programs, and every switch |
| [Features](docs/FEATURES.md) | What's included, how it works on macOS, and its limits |
| [The x86_64 build](docs/X86_64.md) | `MetalSharp-Wine-x86_64.app`, under Rosetta 2 with D3DMetal |
| [Building](docs/BUILDING.md) | The sources, and how to build both apps |
| [Contributing](CONTRIBUTING.md) | How to send a change |
| [Licenses](docs/LICENSES.md) | Every component's license |

## License

MetalSharp Wine's own material is licensed under the [AGPL-3.0](LICENSE). Wine, FEX and the other components keep their own licenses, listed in [docs/LICENSES.md](docs/LICENSES.md).
