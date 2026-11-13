<div align="center">

<img width="80" height="80" alt="MetalSharp Wine" src="https://github.com/user-attachments/assets/87d1a9ab-a9dd-46d6-ad4b-db646ef07b7a" />

# MetalSharp Wine

[![Release](https://img.shields.io/github/v/release/metalsharp/MetalSharp-Wine?style=for-the-badge)](https://github.com/metalsharp/MetalSharp-Wine/releases/latest)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-purple.svg?style=for-the-badge)](LICENSE)

### The Official Open-Source Wine of [MetalSharp](https://github.com/metalsharp/MetalSharp)

</div>

Wine 11.20 as a native arm64 macOS app for Apple silicon. It runs ARM64, x86_64 and 32-bit x86 Windows programs, with x86 code translated by FEX, and Direct3D 9–12 and OpenGL 4.6 on Metal. No Rosetta. It needs **macOS 15 or later**, and 32-bit programs need **macOS 26.4 or later**.

**Early access.** Direct3D 12 most of all is under active development.

## Install

1. Download `MetalSharp-Wine-<version>-arm64.zip` from [Releases](https://github.com/metalsharp/MetalSharp-Wine/releases/latest).
2. Unzip it and move `MetalSharp-Wine.app` to `/Applications`.

MetalSharp uses the app from its Play button, so there's nothing else to set up there. On its own the app has no window: you run it from Terminal, as below.

## Steam

Paste each block into Terminal. Each one sets up what it needs, so they work in a new window too.

**1. Make a prefix for Steam** (`~/Games/steam`; it takes a minute or two):

```bash
APP=/Applications/MetalSharp-Wine.app/Contents; export WINEPREFIX=~/Games/steam WINEDEBUG=-all
"$APP/MacOS/wine" wineboot -u -r && "$APP/MacOS/wineserver" -w
for a in x64 x86; do "$APP/MacOS/wine" "$APP/share/metalsharp/redist/vcrun2022/vc_redist.$a.exe" /install /quiet /norestart; "$APP/MacOS/wineserver" -w; done
```

**2. Install Steam:**

```bash
APP=/Applications/MetalSharp-Wine.app/Contents; export WINEPREFIX=~/Games/steam WINEDEBUG=-all
"$APP/MacOS/wine" "$APP/share/metalsharp/steam/SteamSetup.exe" /S; "$APP/MacOS/wineserver" -w
```

**3. Launch Steam** (run this one whenever you want Steam):

```bash
APP=/Applications/MetalSharp-Wine.app/Contents; export WINEPREFIX=~/Games/steam WINEDEBUG=-all
cd "$WINEPREFIX/drive_c/Program Files (x86)/Steam" && "$APP/MacOS/wine" steam.exe
```

Steam updates itself the first time it starts. To close everything in the prefix:

```bash
WINEPREFIX=~/Games/steam /Applications/MetalSharp-Wine.app/Contents/MacOS/wineserver -k
```

## Other programs

```bash
export WINE=/Applications/MetalSharp-Wine.app/Contents/MacOS/wine
```

```bash
WINEPREFIX=~/Games/my-prefix $WINE ~/Downloads/setup.exe
```

The first run makes the prefix. Switches such as the Direct3D backend are in [docs/USAGE.md](docs/USAGE.md).

## Reporting problems

- **A game's rendering on Mullion** (the default Direct3D backend): [dttdrv/mullion](https://github.com/dttdrv/mullion).
- **On VKD3D-Proton** (`GRAPHICS_BACKEND=vkd3d`): [metalsharp/VKD3D-Proton-MacOS](https://github.com/metalsharp/VKD3D-Proton-MacOS).
- **On DXVK** (`GRAPHICS_BACKEND=dxvk`): [metalsharp/DXVK-MacOS](https://github.com/metalsharp/DXVK-MacOS).
- **Anything else:** this repository's [issues](https://github.com/metalsharp/MetalSharp-Wine/issues).

## More

| | |
|---|---|
| [docs/CHANGES.md](docs/CHANGES.md) | What's new in each release |
| [docs/USAGE.md](docs/USAGE.md) | Running programs, and every switch |
| [docs/FEATURES.md](docs/FEATURES.md) | What's included, how it works on macOS, and limitations |
| [docs/X86_64.md](docs/X86_64.md) | The x86_64 build (`MetalSharp-Wine-x86_64.app`, under Rosetta 2, with D3DMetal) |
| [docs/BUILDING.md](docs/BUILDING.md) | The sources and how to build |
| [docs/LICENSES.md](docs/LICENSES.md) | Licenses. MetalSharp Wine's own material is [AGPL-3.0](LICENSE) |
