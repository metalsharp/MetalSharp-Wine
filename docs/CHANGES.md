# Changes

The [release notes](https://github.com/metalsharp/MetalSharp-Wine/releases) have the full list for each release.

## Unreleased

- Steam's window no longer freezes under FEX after it loads. Its web helper runs SwiftShader for WebGL only (`--use-angle=swiftshader-webgl`), as on the x86_64 runtime, instead of compositing Steam's UI through ANGLE on SwiftShader (Wine patch 0164).

## 6.5

Both builds. Proton's Wine changes and the test-suite fixes are the arm64 build's; Mullion and VKD3D-Proton-MacOS 5.4 are both builds'.

- Proton's Wine. Programs Steam starts get Proton's per-game rules for 198 games (`share/proton/appcompat.env`), and Valve's EasyAntiCheat and BattlEye support is ported: the BattlEye launcher redirect and `belauncher.exe`, `PROTON_EAC_RUNTIME` and `PROTON_BATTLEYE_RUNTIME`, and the EasyAntiCheat bridge load order. Easy Anti-Cheat's EOS bootstrapper loads EAC's Linux module into itself, as under Proton, through the Linux module host (`lib/linuxhost`). With Elden Ring, EAC starts and keeps running and the game boots, but plays offline: EAC's in-game client never gets into the game. BattlEye has not been tested with a game. The port is [metalsharp/Proton](https://github.com/metalsharp/Proton), branch `proton_11.0-macos`.
- Fourteen fixes found by running Wine's test suite: network data received into write-watched memory (the .NET heap) was lost, four socket bugs could hang a program, socket options now have Windows' values and sizes, and copy-on-write protection of a read-only file view works. Native ARM64 failures in the six core modules went from 301 to 227, and every unit finishes ([`docs/TESTING.md`](TESTING.md)).
- Steam starts again under FEX after Steam's November 2026 update. Its web helper now gets `--disable-gpu-early-init`, which switched off all of Wine's Steam browser flags, so its sandboxed storage service crashed in a loop (Wine patch 0163).
- Mullion rebased onto upstream [dttdrv/mullion](https://github.com/dttdrv/mullion) main, on both builds, with its NGX core signed for DLSS.
- VKD3D-Proton-MacOS 5.3 → 5.4 on both builds.

## 6.0

- The x86_64 build, `MetalSharp-Wine-x86_64.app`: x86_64 Wine 11.17 under Rosetta 2 with every graphics lane, DLSS on MetalFX and Apple's D3DMetal (Game Porting Toolkit 4.0 beta 3 on macOS 27, 3.0 below).
- FEX runs x86 code more like an x86 CPU: x64 debug registers, code that protectors unlock on execute, undefined flags as Intel sets them, x87 state across exceptions, and the TSC's frequency in CPUID.
- Faster x86 callbacks and cold program starts.
- VKD3D-Proton-MacOS 5.3 with the Metal Shader Converter bridge, WineMetalGL 3.9.0 and a newer Mullion.
