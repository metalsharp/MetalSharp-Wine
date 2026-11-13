# Using MetalSharp Wine

The [README](../README.md) has the commands to install the app, make a prefix and run Steam. This page has the rest.

## Running a program

```bash
export WINE=/Applications/MetalSharp-Wine.app/Contents/MacOS/wine
```

```bash
WINEPREFIX=~/Games/my-prefix $WINE ~/Downloads/setup.exe
```

```bash
WINEPREFIX=~/Games/my-prefix $WINE "C:\\Program Files\\MyGame\\Game.exe"
```

Running from inside the game's folder also works, and helps some games find their files:

```bash
cd ~/Games/my-prefix/drive_c/Program\ Files/MyGame && WINEPREFIX=~/Games/my-prefix $WINE Game.exe
```

A new prefix (`wineboot -i`) takes a minute the first time. It comes with Wine Mono (.NET), Gecko and the core fonts already set up, and x86 programs run through FEX automatically.

## Switches

Set these before the `$WINE` command, like `WINEPREFIX`.

* Direct3D: Mullion on Metal is the default. For x86_64 games, GRAPHICS_BACKEND=dxvk uses DXVK-macOS (D3D9/10/11 on MoltenVK) and GRAPHICS_BACKEND=vkd3d uses VKD3D-Proton (D3D12 on MoltenVK). GRAPHICS_BACKEND=wine uses Wine’s own wined3d.
* VKD3D-Proton: run it with WINEDEBUG=-all. With Wine's default debug channels, device creation can fail ("Lacking support for single texel alignment").
* OpenGL: WineMetalGL 4.6 is the default. OPENGL_BACKEND=wine switches back to Wine’s own.
* Sync: msync is on by default and is much faster than going through the wineserver. WINEMSYNC=0 turns it off.
* Memory ordering: FEX runs x86 code with its TSO emulation on by default, which games that hand data between threads with plain stores need (Red Dead Redemption 2 hangs without it). FEX_TSOENABLED=0 turns it off, which is faster for programs that don't need it.
* x87: 32-bit programs run x87 floating point in double precision by default, as under Rosetta; full 80-bit emulation is much slower (it made Ori's FMOD audio stutter). FEX_X87REDUCEDPRECISION=0 restores full precision for a program that needs it.
* Mouse: setting UseGCMouse to N (in HKCU\Software\Wine\Mac Driver) goes back to the old mouse input path if GCMouse raw input misbehaves in a game.
* Cursor: a fullscreen game started by Steam or MetalSharp keeps the cursor inside its window. Windowed programs, launchers among them, leave it free. Cmd+left click in a window locks the cursor in it or frees it. METALSHARP_CONFINE_CURSOR=0 turns off the fullscreen default.
* Per game: a `.metalsharp/launch.local.env` file in the game's folder sets environment variables for that game (one `NAME=value` per line). MetalSharp never writes it, so it survives MetalSharp rewriting `launch.env`. An empty `.metalsharp/trace` file there turns on Wine's trace log for that folder's programs.
* Settings: WINEPREFIX=~/Games/my-prefix $WINE winecfg opens Wine’s settings.
