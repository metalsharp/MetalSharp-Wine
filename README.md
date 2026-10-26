<h1 align="center">WineMetalGL</h1>

<p align="center">
  <a href="https://github.com/metalsharp/WineMetalGL/actions/workflows/metal.yml"><img alt="Metal" src="https://img.shields.io/github/actions/workflow/status/metalsharp/WineMetalGL/metal.yml?branch=main&label=Metal&color=purple"></a>
  <a href="https://github.com/metalsharp/WineMetalGL/actions/workflows/build.yml"><img alt="WineMetalGL build" src="https://img.shields.io/github/actions/workflow/status/metalsharp/WineMetalGL/build.yml?branch=main&label=WineMetalGL&color=lightblue"></a>
  <a href="https://github.com/metalsharp/WineMetalGL/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://img.shields.io/github/actions/workflow/status/metalsharp/WineMetalGL/codeql.yml?branch=main&label=CodeQL&color=white"></a>
</p>

### What It Is: 

WineMetalGL is a replacement to Apple's own OpenGL.Framework (Deprecated in 2018), which allows both native Arm64 applications and Wine Apps to receive and use OpenGL 1.0 -> OpenGL 4.6. It is fully conformant with the Khronos GL-CTS Test Suite, and also supports GLES 2 through 3.2. WineMetalGL is OpenGL -> Metal. 

### Quick Start: 

WineMetalGL's main method of installation is with the DMG. It will help you install it into any Wine Prefix / Crossover Bottle. If a new version is released, the application detects this and can update your app and installed runtime to the latest available version. Otherwise, use the scripts from releases in combination with your preferred download (Arm64, x86_64). The below table explains the apps capabilities; 

From 3.7.0, WineMetalGL also runs in native arm64 Wine: CrossOver 27's arm64 bottles and MetalSharp's Wine with FEX, for native ARM64, x86_64 and 32-bit programs. `install-wine-module.sh` and `install-crossover.sh` work out which kind of Wine or bottle they install into, and use or download the matching package (`winemetalgl-<version>-x86_64` or `winemetalgl-<version>-arm64`).

---

| Feature | Use-Case | 
|---|---|
| Install | Install WineMetalGL Into a Wine Found on Your Mac (Its Default Prefix) & Enable / Disable It |
| Custom Install | Choose Any Wine & Prefix in Finder to Install WineMetalGL Into |
| Install Crossover | Install WineMetalGL Into a Crossover Bottle Found Anywhere (Any Copy of CrossOver, or None) & Enable / Disable It |
| Game Fixes | Enable Quick Fixes For Tested Games or Disable Them | 
| x87 Acceleration | Older Titles Use x87 Calls: Enable This Option For Older 32-Bit Games |
| Runtime Logs | Enable Logs Before Launching to See What Fails and Submit an Issue Report | 

<div align="center">

<img width="582" height="911" alt="WineMetalGL Settings" src="https://github.com/user-attachments/assets/9c163b25-40cf-436e-85a8-fda88008bc23" />

</div>

The core,libwinemetalgl.dylib, has no dependency on Apple's OpenGL. It
compiles GLSL with glslang and SPIRV-Cross to the Metal Shading Language and implements the GL state machine, resources and draws on Metal. The Wine modules (winemetalgl.dll / winemetalgl.so, with a forwarding opengl32.dll in the prefix) carries WGL and every GL call to it through the driver ABI in `include/winemetalgl/driver.h`. 

Wine itself is not patched or rebuilt ([Build it yourself](docs/building.md#the-wine-module)).


<!-- BEGIN GENERATED: totals (scripts/docs/generate.py) -->

## Conformance totals | ([Test suite](docs/testing.md#direct3d-through-wined3d))

<table>
<tr>
<td valign="top">
<table>
<tr><th align="left">OpenGL</th><th align="right">Conformance %</th><th align="right">Unsupported %</th></tr>
<tr><td>3.0</td><td align="right">99.88%</td><td align="right">2.28%</td></tr>
<tr><td>3.1</td><td align="right">100%</td><td align="right">2.38%</td></tr>
<tr><td>3.2</td><td align="right">100%</td><td align="right">0.69%</td></tr>
<tr><td>3.3</td><td align="right">100%</td><td align="right">1.11%</td></tr>
<tr><td>4.0</td><td align="right">99.99%</td><td align="right">10.11%</td></tr>
<tr><td>4.1</td><td align="right">99.99%</td><td align="right">10.08%</td></tr>
<tr><td>4.2</td><td align="right">99.98%</td><td align="right">9.74%</td></tr>
<tr><td>4.3</td><td align="right">99.97%</td><td align="right">15.60%</td></tr>
<tr><td>4.4</td><td align="right">covered by KHR-GL46</td><td align="right">-</td></tr>
<tr><td>4.5</td><td align="right">covered by KHR-GL46</td><td align="right">-</td></tr>
<tr><td>4.6</td><td align="right">99.98%</td><td align="right">22.33%</td></tr>
</table>
</td>
<td valign="top">
<table>
<tr><th align="left">GLES</th><th align="right">Conformance %</th><th align="right">Unsupported %</th></tr>
<tr><td>2.0</td><td align="right">100%</td><td align="right">0.60%</td></tr>
<tr><td>3.0</td><td align="right">100%</td><td align="right">0.83%</td></tr>
<tr><td>3.1</td><td align="right">99.98%</td><td align="right">1.24%</td></tr>
<tr><td>3.2</td><td align="right">99.95%</td><td align="right">0.64%</td></tr>
</table>
</td>
</tr>
</table>

<!-- END GENERATED: totals -->

## Docs

- [Conformance results](docs/conformance.md): every Khronos CTS run per
  OpenGL version, configuration and runner, with the reason for each failing
  case.
- [Unsupported](docs/unsupported.md): what WineMetalGL does not support and
  why: the gaps in the compatibility profile, what the
  Unsupported % counts, the failing CTS cases, and every case the CTS reports
  NotSupported.
- [Test suite](docs/testing.md): each test suite, how to run it, and roughly
  how long it takes.
- [Build it yourself](docs/building.md): building the core and the Wine
  module, installing them into a Wine 11.x, and checking that a program uses
  them.
- [Performance](docs/performance.md): frame times against Apple's OpenGL.
- [Logging](docs/logging.md): log levels and categories, the log file, and
  crash reports.
- [List Of OpenGL Games](https://www.pcgamingwiki.com/wiki/List_of_OpenGL_games):
  PCGamingWiki's list of games that render with OpenGL.

Also: the [game corpus](docs/games.md) results, [releasing](docs/release.md), and the [design decisions](docs/decisions/).

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security reports:
[SECURITY.md](SECURITY.md). Code of conduct: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

Copyright (c) 2026 MetalSharp. WineMetalGL is licensed under the GNU Lesser
General Public License, version 3 or (at your option) any later version
(`LGPL-3.0-or-later`): [`LICENSE`](LICENSE), which adds to the GNU General
Public License version 3 in [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt).
Releases up to 2.1.0 were published under the MIT License.

The files of the Wine module adapted from Wine (`src/wine/pe/wgl_formats.c`,
`src/wine/pe/wgl_fonts.c`) are LGPL-2.1-or-later, like Wine
(`LICENSES/Wine-LGPL-2.1.txt`). The vendored glslang and SPIRV-Cross keep
their own licenses (`LICENSES/`).
