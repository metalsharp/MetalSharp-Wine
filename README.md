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

## Run Steam

These steps use the arm64 app, `MetalSharp-Wine.app`. Paste each block into Terminal. Each one sets up what it needs, so it works in a new Terminal window too.

**1. Make a Wine prefix for Steam.** A prefix is a separate Windows install; this one goes in `~/Games/steam`. This takes a minute or two.

```bash
APP=/Applications/MetalSharp-Wine.app/Contents
export WINEPREFIX=~/Games/steam WINEDEBUG=-all
"$APP/MacOS/wine" wineboot -u -r && "$APP/MacOS/wineserver" -w
for a in x64 x86; do
  "$APP/MacOS/wine" "$APP/share/metalsharp/redist/vcrun2022/vc_redist.$a.exe" /install /quiet /norestart
  "$APP/MacOS/wineserver" -w
done
```

**2. Install Steam.**

```bash
APP=/Applications/MetalSharp-Wine.app/Contents
export WINEPREFIX=~/Games/steam WINEDEBUG=-all
"$APP/MacOS/wine" "$APP/share/metalsharp/steam/SteamSetup.exe" /S
"$APP/MacOS/wineserver" -w
```

**3. Start Steam.** Run this whenever you want Steam. The first time, Steam updates itself.

```bash
APP=/Applications/MetalSharp-Wine.app/Contents
export WINEPREFIX=~/Games/steam WINEDEBUG=-all
cd "$WINEPREFIX/drive_c/Program Files (x86)/Steam" && "$APP/MacOS/wine" steam.exe
```

**To quit** Steam and everything else running in the prefix:

```bash
WINEPREFIX=~/Games/steam /Applications/MetalSharp-Wine.app/Contents/MacOS/wineserver -k
```

## Run other programs

Point `WINEPREFIX` at a folder for the prefix, and give Wine the program to run. The first run creates the prefix.

```bash
export WINE=/Applications/MetalSharp-Wine.app/Contents/MacOS/wine
WINEPREFIX=~/Games/my-prefix $WINE ~/Downloads/setup.exe
```

To run an installed game, start it from its own folder; some games need that to find their files:

```bash
cd ~/Games/my-prefix/drive_c/Program\ Files/MyGame
WINEPREFIX=~/Games/my-prefix $WINE Game.exe
```

## Graphics backends

Each game uses one graphics backend. The defaults suit most games; to try another, set its switch before the `$WINE` command, the same way as `WINEPREFIX`.

| What the game uses | Default | Alternatives |
|---|---|---|
| Direct3D 10, 11, 12 | **Mullion** (on Metal) | `GRAPHICS_BACKEND=vkd3d`: VKD3D-Proton for Direct3D 12, with DXVK for 11 down to 3 <br> `GRAPHICS_BACKEND=dxvk`: DXVK for Direct3D 11 down to 3 <br> `GRAPHICS_BACKEND=wine`: Wine's own |
| Direct3D 9 and older | Wine's own | `GRAPHICS_BACKEND=dxvk` |
| OpenGL | **WineMetalGL** (OpenGL 4.6 on Metal) | `OPENGL_BACKEND=wine`: Wine's own |
| Vulkan | **MoltenVK** | |

For example:

```bash
GRAPHICS_BACKEND=vkd3d WINEPREFIX=~/Games/my-prefix $WINE Game.exe
```

The x86_64 app also has Apple's **D3DMetal** (`GRAPHICS_BACKEND=d3dmetal`). Every switch, including sync, CPU emulation, mouse and per-game settings, is in [docs/USAGE.md](docs/USAGE.md).

## Reporting problems

Open the issue where the fix will be made:

| The problem | Where to report it |
|---|---|
| A game draws wrong on **Mullion** (the default Direct3D backend) | [dttdrv/mullion](https://github.com/dttdrv/mullion/issues) |
| A game draws wrong on **VKD3D-Proton**, **DXVK** or **MoltenVK** | [metalsharp/MetalVulkan](https://github.com/metalsharp/MetalVulkan/issues) |
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
