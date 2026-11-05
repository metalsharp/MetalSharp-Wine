# Build it yourself

How to build the core and the Wine module, install them into a Wine, and
check that programs use them. The test suites are in [Test suite](testing.md);
release packages are in [Releasing](release.md).

## Requirements

- macOS 15 or later on Apple silicon, Xcode 16 or later, CMake 3.24 or later,
  Ninja and Python 3.
- Rosetta 2, to run an x86_64 Wine.
- For the Wine module: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
  (`x86_64-w64-mingw32-clang`, `i686-w64-mingw32-clang` on `PATH`) and the
  headers of a Wine 11.x (an installation or its source tree).
- For the guest test programs: Homebrew `mingw-w64`
  (`x86_64-w64-mingw32-gcc`, `i686-w64-mingw32-gcc`).

glslang and SPIRV-Cross are vendored at pinned revisions
([`DEPENDENCIES.md`](../DEPENDENCIES.md)), so the build fetches nothing. The
core links only Apple system frameworks and never links OpenGL.framework.

## The core

```sh
./scripts/build.sh            # build/release/libwinemetalgl.dylib (x86_64, macOS 15)
```

That is the core for an x86_64 Wine under Rosetta. `scripts/stage-arm64-core.sh
/path/to/output` builds the arm64 core and stages it with the driver ABI
header and, with Homebrew's SDL2, SDL3 and GLFW (`brew install sdl2 sdl3
glfw`), the native games' glue (`glue/`, `scripts/build-arm64-glue.sh`); the Wine module is built for x86_64 Wine only, so the arm64 core has
not yet run inside an arm64 Wine.

For development, the core and its tests also build for the machine itself,
without Rosetta:

```sh
cmake -S . -B build/host -DWMGL_HOST_ONLY=ON
cmake --build build/host
build/host/wmgl_host_tests && build/host/wmgl_gpu_tests
```

## The Wine module

WineMetalGL reaches Windows programs through a Wine module of its own, built
outside Wine and installed next to Wine's modules: no Wine source is patched
or rebuilt. It works with any x86_64 Wine 11.x on macOS 15 or later, such as
the WineHQ release or MetalSharp's Wine 11.17; it is tested on stock Wine 11.0,
WineHQ's macOS builds of Wine 11.0 and 11.18, and MetalSharp's Wine 11.17.

| File | Where | What |
|---|---|---|
| `winemetalgl.dll` | `lib/wine/x86_64-windows`, `lib/wine/i386-windows`, and the prefix's `system32` and `syswow64` | WGL and the GL entry points, for 64- and 32-bit programs |
| `winemetalgl.so` | `lib/wine/x86_64-unix` | its unix side, which runs both (32-bit calls through Wine's WoW64 thunks) |
| `libwinemetalgl.dylib` | `lib/wine/x86_64-unix` | the core |
| `opengl32.dll` | the prefix's `system32` and `syswow64` | forwards every export to `winemetalgl.dll` |

```sh
./scripts/build.sh                                      # the core
./scripts/build-wine-module.sh /path/to/wine            # build/wine-module
./scripts/install-wine-module.sh /path/to/wine /path/to/prefix
```

Run on their own (a script downloaded without the release, or WineMetalGL
Settings without one inside it), `install-wine-module.sh` and
`install-crossover.sh` download the latest release (`WINEMETALGL_VERSION=x.y.z`
picks another), check it against the release's `SHA256SUMS.txt`, and keep it
extracted in `~/Library/Caches/WineMetalGL/releases` (`WINEMETALGL_CACHE`).
macOS's own `curl`, `shasum` and `tar` do all of it.

`install-wine-module.sh` adds the module and the core to the Wine installation
(the directory with `bin/wine`, or a Wine app such as WineHQ's `Wine
Stable.app`) without replacing any of Wine's files. Given a prefix, it also puts the
forwarding `opengl32.dll` into it, with a copy of `winemetalgl.dll` for Wines
whose loader does not look in `lib/wine` for it, and sets the DLL override
`opengl32=native`; run it once per prefix (a prefix that does not exist yet
is created). If macOS refuses the copy (`Operation not permitted`), the
terminal needs App Management permission (System Settings, Privacy &
Security). Removing the override (or running
a program with `WINEDLLOVERRIDES=opengl32=b`) gives that prefix Wine's own
OpenGL back.

### CrossOver

CrossOver.app is signed and macOS does not let it be changed (`cp: ...:
Operation not permitted`), so `install-wine-module.sh` refuses it; use
`install-crossover.sh` instead, once per bottle:

```sh
./scripts/install-crossover.sh "My Bottle"                 # a bottle's name, or its directory
./scripts/install-crossover.sh "My Bottle" /path/to/CrossOver.app
```

Nothing goes into CrossOver.app, as a rule. The module and the core go into
`~/Library/Application Support/WineMetalGL/lib/wine` (`WINEMETALGL_HOME`
chooses another directory), and the bottle's `cxbottle.conf` adds that
directory to its `[Wine] DllPath`, so CrossOver's Wine finds them there. The
bottle gets the forwarding `opengl32.dll` and `winemetalgl.dll` (in
`system32`, and `syswow64` for a 64-bit bottle) and the `opengl32=native`
override. CrossOver's `wine` script ignores `WINEPREFIX` and
`WINEDLLOVERRIDES`; removing the override in the bottle's Wine configuration
(Libraries) gives the bottle CrossOver's OpenGL back, and
`wine --bottle "My Bottle" --dll opengl32=b` does so for one run. It is
tested on CrossOver 26.3, in 64- and 32-bit bottles. CrossOver 27 runs an
arm64 Wine, which the module is not built for yet (issue #13); a CrossOver
that carries the x86_64 Wine as well (a CrossOver 27 Preview) is installed
into; arm64 bottles may experience issues.

Some CrossOver builds (the CrossOver 27 Preview, issue #26) ignore the
bottle's `[Wine] DllPath`, and games then stop with `winemetalgl.so is not
loaded`. The installer asks the bottle's Wine which DLL directories it got,
and when WineMetalGL's is not among them, copies the module and the core into
CrossOver.app's own `lib/wine` instead (`WINEMETALGL_INTO_APP=1` does so
without asking). macOS may need Terminal, or WineMetalGL Settings, allowed
under Privacy & Security, App Management for that, and updating CrossOver
removes them again: run the installer once more after an update.

What matters:

- The Wine must be an x86_64 build (run under Rosetta) with i386 and x86_64
  guests (`--enable-archs=i386,x86_64`), so one WoW64 prefix runs 32- and
  64-bit programs.
- The module loads the core from its own directory; it reports an error on
  stderr and offers no pixel formats when the core cannot be loaded.
- Windows appear through Wine's macOS driver: the module puts a Metal layer
  over the window's client area, in Wine's Cocoa window.
- Wine's optional features need x86_64 builds of their libraries, such as
  FreeType for fonts. On Apple silicon, Homebrew installs arm64 libraries,
  which an x86_64 Wine cannot load;
  `WINEMETALGL_HOST_LIB_DIR=/path/to/x86_64/libs scripts/stage-host-libs.sh /path/to/wine`
  copies an x86_64 FreeType closure into a Wine installation.

## WineMetalGL Settings

`WineMetalGL Settings.app` (`src/settings-app`) is the easy way in. Each
release has it as a signed and notarized disk image,
`WineMetalGL-Settings-<version>.dmg`: open it and drag the app onto
Applications.

- **WineMetalGL**: the CrossOver bottles, MetalSharp's prefixes, the Steam
  games [NotProton](https://github.com/NotProtonNot/NotProton) runs, `~/.wine`
  and the prefixes installed into from the app, each with a switch that turns
  WineMetalGL on or off there (the `opengl32` override, `native` or
  `builtin`). A NotProton game is its prefix, `compatdata/<appid>/pfx` in any
  Steam library, run by the NotProton runner (a copy of CrossOver in
  `~/Library/Application Support/notproton/runners`) it last ran with; its
  **Install** puts WineMetalGL into that runner's `lib/wine` and into the
  prefix, set up with the Wine loader NotProton runs it with. NotProton
  replacing the runner, or rebuilding the prefix, removes it again: the game
  then shows **Install** once more. Three buttons always show:
  - **CrossOver** (Install into CrossOver): a bottle found anywhere on the Mac
    (CrossOver's Bottles folder, the bottles installed into from the app, any
    other `cxbottle.conf` Spotlight finds), grouped by copy of CrossOver when
    there are several; or **Choose Bottle…**, or **Choose CrossOver and
    Bottle…** (a CrossOver app anywhere, a copy or one outside Applications),
    in Finder. Without CrossOver.app, the bottle still gets WineMetalGL (the
    override is written into its `user.reg`).
  - **Install**: one of the Wines found on the Mac, in its default prefix,
    `~/.wine` (a menu when there are several): apps in `/Applications` and
    `~/Applications`, Homebrew's and MacPorts', Whisky's and MetalSharp's,
    and any other `bin/wine` Spotlight finds.
  - **Custom Install**: any Wine, then any prefix, chosen in Finder.

  They run `install-crossover.sh` and `install-wine-module.sh` with the
  release the app carries, or, when it carries none, the latest release they
  download.
- **UpscaleGL**: Off, Quality, Balanced or Performance (`WINEMETALGL_UPSCALE`).
- **x87 Acceleration**: on (`WINEMETALGL_X87=1`) or off (`=0`). Until it
  is switched, it is on only for the games known to need it (Quake 4, Doom 3
  and Doom 3: BFG Edition: `Quake4.exe`, `Doom3.exe`, `Doom3BFG.exe`), and
  off for the others.
- **Logging**: everything (`WINEMETALGL_LOG=2`) or nothing, and a button that
  opens `~/Library/Logs/WineMetalGL`. Until it is switched, WineMetalGL logs
  warnings, as without the app.
- **Game Fixes**: two sections, each with switches of its own.
  **Wine Fixes**, on by default: the app applies the fixes in
  `Game Specific Patched Files` wherever it finds their games in those bottles
  and prefixes. It looks in Steam's libraries (from `libraryfolders.vdf`,
  inside the prefix or not), GOG's folders, Program Files, and MetalSharp's
  records of where its games are.
  - Quake 4 (`Quake4.exe`): the display modes go into the prefix or bottle
    (`apply-display-modes.sh`, once that prefix's programs have quit), and
    `r_useSMP 1` into `q4base/autoexec.cfg`, added to one the game already
    has.
  - SpaceEngine (`winemac.so`) and Wolfenstein: The New Order
    (`xaudio2_7.dll`): installed into MetalSharp's Wine 11.17, the Wine they
    were built from, and nowhere else.

  **Arm64 Fixes**, off by default, a switch per game: native games on the
  arm64 core, outside Wine, through the glue the app carries prebuilt
  (`scripts/build-arm64-glue.sh`, in the staged arm64 core's `glue`).
  - Minecraft: the core and the glue go into
    `~/.local/share/winemetalgl/minecraft`, beside LWJGL's own SDL3 and GLFW
    (from its natives jars), and each Prism Launcher instance, Minecraft
    Launcher installation and Modrinth App instance gets the JVM arguments that
    load them (the SDL3 glue for LWJGL 3.4, Minecraft 26.x; the GLFW glue for
    3.3, 1.21.x), after its own. What they had is recorded in
    `~/Library/Application Support/WineMetalGL/NativeFixes.json`, and turning
    the fix off puts it back. A launcher that is running waits until it quits.
  - OpenMW: an `OpenMW.app` in `/Applications` or `~/Applications` gets what
    its folder's `install.sh` installs: the glue as its SDL2 (the real one
    renamed `libSDL2-real.dylib`), the GL stubs and the core, and `openmw`
    and OpenSceneGraph's libraries load the stubs (their load commands
    rewritten in place, without the Command Line Tools). It is signed ad hoc
    again. Turning the fix off loads Apple's OpenGL again.

  A fix that is already in place is left alone. Each file a fix replaces is
  first copied to `~/Library/Application Support/WineMetalGL/Backups`, under
  its own path; copying that back undoes it.

When it opens, the app asks GitHub for the latest release. When that release
is newer, the app asks whether to update. On Yes, it downloads the release's
`WineMetalGL-Settings-<version>.dmg` and checks it against the release's
`SHA256SUMS.txt`. It then checks the image's signature and the new app's: the
same Developer ID team as the app, and accepted by Gatekeeper. The new app
replaces this one (in `/Applications` when this one cannot be replaced) and
opens. It then installs its own release, with the install scripts and module
it carries, into every bottle and prefix that has WineMetalGL. To try updating
without a release, set `WINEMETALGL_UPDATE_FEED` to the URL (`file://` works)
of a release in the JSON of GitHub's releases API.
`WINEMETALGL_UPDATE_TESTING=1` accepts the ad hoc signatures of local builds.
`WINEMETALGL_SETTINGS_HOME` gives the app another home directory, with its own
defaults, so made-up prefixes can be used.

UpscaleGL, x87 acceleration and logging go to `~/Library/Application Support/WineMetalGL/settings.conf`,
`NAME=value` lines of the variables below, which WineMetalGL reads when it
loads; the environment wins over the file, and a change applies to games
started afterwards. `scripts/build-settings-app.sh` builds the app (Liquid
Glass needs Xcode 26 to build and macOS 26 to show; on macOS 14 and 15 the
app draws a simulation of it, which `WINEMETALGL_SIMULATED_GLASS=1` shows on
macOS 26 too; `PAYLOAD=<staged
release>` puts the x86_64 release in it whole, which it then installs without
a download, `ARM64_CORE=<staged arm64 core>` the arm64 core, and
`APP_VERSION` overrides its version). `scripts/sign-settings-app.sh` signs it,
ad hoc without `WINEMETALGL_SIGN_IDENTITY`. `scripts/build-settings-dmg.sh`
makes the disk image it is released in.

## Checking that a program uses the core

Look at a program's `GL_RENDERER` string: it ends in `(WineMetalGL)`, for
example `M4 (WineMetalGL)`. With `WINEMETALGL_LOG=2` the core prints
`winemetalgl:info: WineMetalGL core on <GPU>` to stderr when it loads. If the
renderer is anything else, the prefix has no `opengl32.dll` from the module or
no `opengl32=native` override, and the program got Wine's own OpenGL.

## Environment

| Variable | Effect |
|---|---|
| `WINEDLLOVERRIDES=opengl32=b` | Use Wine's own OpenGL instead of WineMetalGL (how reference frames are recorded) |
| `WINEMETALGL_LOG=1..3` | Log warnings, information or traces, everywhere or per category (`shader:3,gpu:2`); see [Logging](logging.md). Errors go to stderr and warnings to a log file in `~/Library/Logs/WineMetalGL` by default. Warnings include each GL error the first time a command raises it, with the call's arguments |
| `WINEMETALGL_LOG_FILE=<path>` | Write the log file there (a directory, or a file where `%p` is the process ID); `0` writes none |
| `WINEMETALGL_LOG_MAX_MB=<n>` | Stop the log file at `n` MiB (default 64) |
| `WINEMETALGL_EXCEPTIONS=1` | Also log Windows access violations, illegal instructions and division by zero with a backtrace (invalid handles, stack overflows and heap corruption always are); see [Logging](logging.md#windows-exceptions) |
| `WINEMETALGL_CRASH_HANDLER=0` | Do not write crash reports (they go to the log file, with the recent history, when WineMetalGL's code faults or the process aborts) |
| `WINEMETALGL_TRACE=1` | Log every GL call |
| `WINEMETALGL_STATS=1` | Log per-frame statistics: frame and GL time, draws, passes and why they ended, command buffers, pipeline compiles, CPU conversions, readbacks, indirect draw commands read by the GPU or (after a wait) the CPU, and why the CPU reads them (each reason described once per program), and the memory the Metal device has allocated (growing while a scene stands still means GPU resources are retained); every 60 frames, the costliest commands |
| `WINEMETALGL_PASS_PROFILE=1` | Time every Metal encoder on the GPU and log (category `perf`, info: `WINEMETALGL_LOG=perf:2`) every 3 seconds the GPU milliseconds per second each kind of encoder took, by its target and the programs it ran, named by their `KHR_debug` labels. Encoder times overlap on tile-based GPUs, so their sum can exceed the frame time |
| `WINEMETALGL_BATCH=0` | Make every GL call its own unix call (by default the module batches calls that return nothing and take values, and runs them before the next other call) |
| `WINEMETALGL_COMMIT_DRAWS=<n>` | Commit the recorded work to the GPU once it holds `n` draws (default 256), so the GPU runs a frame's work while the CPU records the rest: where a draw starts a render pass, or within one past `8 × n` draws (a commit ends the pass); 0 commits only when a GL call needs the GPU's results and at SwapBuffers |
| `WINEMETALGL_METAL_TESSELLATION=0` | Tessellate every patch on the CPU, after reading the control stage's levels back, instead of drawing rasterized triangle and quad patches with Metal's tessellator |
| `WINEMETALGL_FLAT_ANALYSIS=0` | Rotate the indices of every draw with flat fragment inputs and the last-vertex convention (reading indirect commands on the CPU), instead of only those whose flat values can differ between a primitive's vertices |
| `WINEMETALGL_SAMPLER_LOD_BIAS=0` | Filter lookups through samplers with a LOD bias in the shader, as on macOS 15 where Metal's samplers have no LOD bias (for testing that path) |
| `WINEMETALGL_VERTEX_OFFSETS16=1` | Draw with vertex starts and base vertices past 16 bits through CPU-written indices, as on macOS virtual machines' GPU (for testing that path) |
| `WINEMETALGL_PREWARM=0` | Compile a program's Metal functions at its first draw instead of in the background after it links |
| `WINEMETALGL_PIPELINE_CACHE=0`, `=<directory>` | Turn off, or move, the on-disk pipeline cache. Render pipelines are kept compiled in a Metal binary archive per program, WineMetalGL build, GPU and macOS build, in `~/Library/Caches/WineMetalGL/pipelines` by default. A pipeline compiles once, not once per run |
| `WINEMETALGL_SHADER_CACHE=0` | Turn off the on-disk shader cache. Programs' GLSL-to-MSL translations are kept per link inputs and WineMetalGL build, in `~/Library/Caches/WineMetalGL/shaders` by default. A program linked again in a later run skips compiling and translating its shaders. Past 512 MB the least recently used files go |
| `WINEMETALGL_GPU_TIMERS=0` | Time `GL_TIME_ELAPSED` queries by committing command buffers at their begin and end instead of by GPU timestamps of the encoders in between |
| `WINEMETALGL_PREDICT_PIPELINES=0` | Do not build pipelines ahead of their first draw. By default, the render state of each new pipeline is built in the background for the other programs with the same vertex inputs and fragment outputs, so an effect's first draw does not wait for the Metal compiler |
| `WINEMETALGL_THREAD_QOS=0` | Leave thread priorities alone. By default, a thread that makes a context current, and the thread that presents frames at swap interval 0, ask macOS for the user-interactive quality-of-service class, as a native game's render thread does, so they keep performance cores while a game's worker threads load every core |
| `WINEMETALGL_INVARIANCE=0` | Compile vertex positions for invariance only in shaders that declare `invariant`. By default every vertex shader computes positions the same way, so passes that test depth for equality against another pass's depth (pre-passes, decals) match exactly under relaxed math |
| `WINEMETALGL_SAFE_MATH=1` | Compile shaders with Metal's safe math. By default they compile with relaxed math without reassociation: arithmetic runs in the order the shader writes it, as GL drivers compile GLSL, with fused multiply-adds (infinities and NaN are kept). Safe math keeps the order too, at a large cost to arithmetic-heavy shaders (Minecraft's BSL shaders run about 45% slower) |
| `WINEMETALGL_SAFE_MATH=0` | Compile shaders with relaxed math and reassociation: faster still, but it can overflow products a shader keeps finite (SpaceEngine's gas giants, issue #35) |
| `WINEMETALGL_METALFX=0`, `=<factor>` | Turn off, or set the factor of, MetalFX spatial upscaling of window back buffers. Unset, WineMetalGL follows DXMT's setting: off when `DXMT_METALFX_SPATIAL_SWAPCHAIN=0`, else `d3d11.metalSpatialUpscaleFactor` from `DXMT_CONFIG`, `DXMT_CONFIG_FILE` or the Wine installation's `etc/dxmt.conf` (2 when only `DXMT_METALFX_SPATIAL_SWAPCHAIN=1` is set). Factors above 1 (at most 4) present a back buffer through the MetalFX spatial scaler into a drawable that much larger, on GPUs MetalFX supports |
| `WINEMETALGL_X87=1` | x87 acceleration for 32-bit programs: when a 32-bit program loads `opengl32.dll`, WineMetalGL starts `x87sidecar` (installed next to `winemetalgl.so`; another with `WINEMETALGL_X87_SIDECAR=<path>`), which makes Rosetta translate x87 floating-point code into fast native code instead of its slow soft-float, and hands it this process. Code Rosetta translated before that stays slow; most of a game runs after it (Quake 4's heaviest scenes, with its `r_useSMP 1`, went from 33 to 62 fps). A Wine that runs 32-bit programs through x87sidecar already (MetalSharp) is left alone; where Rosetta is a version x87sidecar does not know, the program runs as without it. Off by default |
| `WINEMETALGL_UPSCALE=<preset>` | UpscaleGL: fullscreen programs render at a fraction of the screen and MetalFX scales their frames up to it, as DLSS's modes do in a game that supports them. `quality` renders at 67% of the screen's width and height, `balanced` at 58%, `performance` at 50% (960×540 on a 1080p screen) and `ultra` at 33%; a percentage (`75%`) sets another; `off` (the default) leaves the screen alone. The program is shown one display, the main one, at that size: it is the only resolution listed (at the screen's refresh rates), the system metrics and monitor rectangles agree, and a mode change succeeds without changing the real mode (a mode of another shape is shown with black bars, not stretched). A top-level window that covers that display is made to cover the real screen, and the program sees its client area, window rectangle, size messages and mouse and cursor positions at the size it asked for. Spatial upscaling (MetalFX), not temporal: OpenGL programs give no motion vectors. Windowed programs are not scaled, and a program that reads the display before it loads `opengl32.dll` sees the real one |
| `WINEMETALGL_EMULATE_MODES=0` | Leave display mode changes to Wine. By default, with UpscaleGL off, a program's mode changes are emulated: it sees the real display and all of its modes, and a mode it changes to (1280×720, say) becomes the one it sees while the real mode stays, its fullscreen window covering the real screen and its frames scaled to it, as UpscaleGL does. Nothing is scaled until it changes the mode. A real mode change through Wine's macOS driver can deadlock the program (a window resize during the change takes the display lock the change holds) or not take, leaving the program's mode-sized picture in a corner of the screen ([#86](https://github.com/metalsharp/WineMetalGL/issues/86): EDuke32 fullscreen) |
| `WINEMETALGL_SETTINGS=<path>`, `=0` | Read the settings file from there, or none. By default `~/Library/Application Support/WineMetalGL/settings.conf` (written by WineMetalGL Settings) sets the variables of this table the environment does not set |
| `WINEMETALGL_WINE_DEBUG=1` | Log what the Wine module does (contexts, surfaces, windows): `WINEMETALGL_LOG=wine:3,pe:3` |
| `WINEMETALGL_EXTENSIONS_LENGTH=<n>` | Keep `glGetString(GL_EXTENSIONS)` to `n` bytes (whole names, oldest first; `0`: the whole string). By default, contexts a program created as OpenGL 1.x or 2.x (legacy `wglCreateContext` asks for 1.0) get 3900 bytes, as older programs copy or print the string into fixed buffers (Jedi Academy overran a 4096-byte one); `glGetStringi` always lists every extension |
| `WINEMETALGL_MAX_FRAME_RATE=<fps>` | Limit the frame rate: `SwapBuffers` returns at most `fps` times a second, with or without vsync (`30`, `40`, `59.94`...). For older games that tie their physics to the frame rate and have no limit of their own but vsync, which runs them at 120 fps on a 120 Hz display, or to save battery. Off by default; it can also go in the settings file |
| `WINEMETALGL_VENDOR`, `WINEMETALGL_RENDERER` | Replace the `GL_VENDOR` and `GL_RENDERER` strings, for games that refuse unknown vendors |
