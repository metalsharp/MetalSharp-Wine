# Conformance testing

Generated 2026-10-22 by `scripts/summarize-tests.py` from the Wine 11.20 test suite, run with `scripts/run-winetest.sh` against the packaged `dist/` bundle. `aarch64` = native ARM64 test binaries; `x86_64` = x86_64 test binaries running through FEX (ARM64EC).

A unit is *clean* when it exits 0. *Crash/timeout* means the unit did not print its summary (crash, debugger, or the 300 s limit).

## aarch64

| module | units | clean | crash/timeout | checks | failures | failing units (failure count) |
|---|---|---|---|---|---|---|
| advapi32 | 12 | 12 | 0 | 12,804 | 0 | - |
| bcrypt | 1 | 1 | 0 | 23,523 | 0 | - |
| comctl32 | 29 | 29 | 0 | 349,644 | 0 | - |
| crypt32 | 15 | 15 | 0 | 18,092 | 0 | - |
| d3d11 | 1 | 0 | 0 | 3,927 | 15 | d3d11 (15) |
| d3d9 | 4 | 1 | 1 | 229,061 | 28 | d3d9ex (11), device (crash/timeout), visual (17) |
| dinput | 6 | 1 | 2 | 30,011 | 34 | device8 (16), force_feedback (crash/timeout), hid (12), hotplug (6), joystick8 (crash/timeout) |
| dsound | 7 | 7 | 0 | 34,126 | 0 | - |
| dxgi | 1 | 0 | 1 | 0 | 0 | dxgi (crash/timeout) |
| gdi32 | 14 | 10 | 0 | 4,088,341 | 47 | clipping (2), dc (6), font (37), metafile (2) |
| kernel32 | 33 | 26 | 1 | 5,340,184 | 284 | change (82), debugger (1), loader (crash/timeout), module (3), toolhelp (4), version (36), virtual (158) |
| kernelbase | 6 | 6 | 0 | 4,371 | 0 | - |
| mf | 3 | 1 | 0 | 86,275 | 17 | mf (1), transform (16) |
| mfplat | 1 | 1 | 0 | 12 | 0 | - |
| mmdevapi | 6 | 4 | 0 | 15,809 | 23 | render (2), spatialaudio (21) |
| msvcrt | 13 | 13 | 0 | 416,238 | 0 | - |
| ntdll | 27 | 21 | 0 | 5,003,313 | 183 | change (29), file (5), info (14), path (4), threadpool (1), wow64 (130) |
| ole32 | 14 | 13 | 0 | 33,788 | 5 | compobj (5) |
| oleaut32 | 10 | 9 | 0 | 3,585,293 | 8 | vartype (8) |
| opengl32 | 1 | 0 | 1 | 0 | 0 | opengl (crash/timeout) |
| secur32 | 5 | 2 | 1 | 559 | 5 | negotiate (3), ntlm (crash/timeout), secur32 (2) |
| shell32 | 19 | 19 | 0 | 12,256 | 0 | - |
| shlwapi | 11 | 11 | 0 | 216,791 | 0 | - |
| ucrtbase | 8 | 8 | 0 | 989,546 | 0 | - |
| user32 | 24 | 18 | 0 | 1,124,914 | 454 | dde (5), input (133), monitor (282), msg (24), sysparams (1), win (9) |
| vulkan-1 | 1 | 0 | 1 | 0 | 0 | vulkan (crash/timeout) |
| winmm | 10 | 10 | 0 | 16,483 | 0 | - |
| winspool.drv | 1 | 1 | 0 | 238 | 0 | - |
| ws2_32 | 3 | 0 | 1 | 9,186 | 41 | afd (10), protocol (31), sock (crash/timeout) |
| **total** | **286** | **239** | **9** | **21,644,785** | **1,144** | 99.995% of checks pass |

## i386

| module | units | clean | crash/timeout | checks | failures | failing units (failure count) |
|---|---|---|---|---|---|---|
| advapi32 | 12 | 12 | 0 | 13,267 | 0 | - |
| bcrypt | 1 | 1 | 0 | 23,523 | 0 | - |
| crypt32 | 15 | 15 | 0 | 18,076 | 0 | - |
| gdi32 | 14 | 10 | 0 | 4,088,332 | 47 | clipping (2), dc (6), font (37), metafile (2) |
| kernel32 | 33 | 26 | 1 | 5,274,387 | 128 | change (82), heap (1), loader (crash/timeout), module (3), process (2), thread (4), version (36) |
| kernelbase | 6 | 6 | 0 | 4,371 | 0 | - |
| msvcp140 | 1 | 1 | 0 | 1,651 | 0 | - |
| msvcrt | 13 | 13 | 0 | 416,272 | 0 | - |
| ntdll | 27 | 21 | 1 | 4,967,119 | 167 | change (26), exception (crash/timeout), file (5), path (4), threadpool (1), wow64 (131) |
| ole32 | 14 | 13 | 0 | 33,791 | 5 | compobj (5) |
| oleaut32 | 10 | 10 | 0 | 3,585,318 | 0 | - |
| shell32 | 19 | 19 | 0 | 12,223 | 0 | - |
| shlwapi | 11 | 11 | 0 | 216,791 | 0 | - |
| ucrtbase | 8 | 8 | 0 | 989,528 | 0 | - |
| user32 | 24 | 18 | 0 | 1,123,881 | 368 | clipboard (1), input (133), monitor (210), msg (14), sysparams (1), win (9) |
| ws2_32 | 3 | 0 | 1 | 9,186 | 41 | afd (10), protocol (31), sock (crash/timeout) |
| **total** | **211** | **184** | **3** | **20,777,716** | **756** | 99.996% of checks pass |

## x86_64

| module | units | clean | crash/timeout | checks | failures | failing units (failure count) |
|---|---|---|---|---|---|---|
| advapi32 | 12 | 12 | 0 | 12,804 | 0 | - |
| bcrypt | 1 | 1 | 0 | 23,523 | 0 | - |
| crypt32 | 15 | 14 | 0 | 18,092 | 2 | sip (2) |
| gdi32 | 14 | 10 | 0 | 4,088,341 | 47 | clipping (2), dc (6), font (37), metafile (2) |
| kernel32 | 33 | 25 | 0 | 5,321,883 | 330 | change (82), heap (1), loader (45), process (7), thread (6), toolhelp (4), version (27), virtual (158) |
| kernelbase | 6 | 6 | 0 | 4,371 | 0 | - |
| msvcp140 | 1 | 1 | 0 | 1,671 | 0 | - |
| msvcrt | 13 | 13 | 0 | 416,238 | 0 | - |
| ntdll | 27 | 19 | 2 | 4,858,202 | 197 | change (29), exception (crash/timeout), file (5), info (30), path (4), rtl (crash/timeout), threadpool (1), wow64 (128) |
| ole32 | 14 | 13 | 0 | 33,788 | 3 | compobj (3) |
| oleaut32 | 10 | 9 | 0 | 3,585,295 | 8 | vartype (8) |
| shell32 | 19 | 19 | 0 | 12,256 | 0 | - |
| shlwapi | 11 | 11 | 0 | 216,791 | 0 | - |
| ucrtbase | 8 | 8 | 0 | 989,551 | 0 | - |
| user32 | 24 | 17 | 0 | 1,124,836 | 375 | class (3), dde (5), input (133), monitor (210), msg (14), sysparams (1), win (9) |
| ws2_32 | 3 | 0 | 1 | 9,186 | 42 | afd (11), protocol (31), sock (crash/timeout) |
| **total** | **211** | **178** | **3** | **20,716,828** | **1,004** | 99.995% of checks pass |

## Where the failures come from

Each remaining failure group was looked at and assigned to a cause.

**16K host pages.** These go away once the process runs with 4K pages (`WINE_4K_PAGES=1` with the entitlement, macOS 26+). Linux on 16K-page kernels sees the same failures.
- ntdll `virtual`: guard size 0x4000 instead of 0x1000, RegionSize rounding, `STATUS_MAPPED_ALIGNMENT` for 4K-aligned section views.
- kernel32 `virtual`.
- kernelbase `process`: splitting placeholders.

**No mappable low 4GB on macOS without the entitlement.**
- `zero_bits` and low-address allocations fail.
- ntdll `info` queries 0x7ffe0000, but KUSER_SHARED_DATA lives at 0x17ffe0000.
- ntdll `wow64` and every 32-bit (i386) child process.

**macOS platform gaps that exist in upstream Wine too.**
- `change` in ntdll and kernel32: there is no directory change notification backend.
- ntdll `info`: thread affinity (macOS doesn't support pinning) and working-set statistics.
- user32 `dde` and `msg`: timing and focus with the window server.
- ws2_32: environment-dependent network checks.
- OpenGL: the legacy macOS GL 2.1 context lacks entry points the test calls through a NULL pointer.
- d3d9ex: hangs in the fullscreen display-mode change test (WM_DISPLAYCHANGE). That test changes the physical display mode.

**Direct3D 10/11 through wined3d.** wined3d's OpenGL renderer on macOS only reaches feature level 9_3. With Vulkan available, the dxgi swapchain tests then fail to create a D3D10 device and dereference NULL. On macOS, D3D10/11 normally goes through DXMT (D3D11 to Metal) or DXVK on MoltenVK. Neither is part of this build yet.

## Bugs found by these runs and fixed

| Bug | Fix |
|---|---|
| winemac.drv: `OnMainThread()` called on the main thread from `-[WineContentView viewWillDraw]` deadlocked every OpenGL/wined3d test | Run such blocks inline (`patches/wine`) |
| dnsapi: a 5s multicast-DNS timeout at every process start made the suite (and every cold start) slow | Use the interface addresses for `.local` hostnames |
| FEX: `pop [mem]` advanced RSP before a faulting store, so handlers resumed with RSP off by 8 (x64 ntdll `exception`) | Commit RSP after the store (`patches/fex`) |
| Write protection of executable 4K pages was lost inside a 16K host page, so FEX missed self-modifying code (x64 ntdll `exception` ran a stale translation) | Keep such host pages read-only and emulate writes to their writable pages |
| Executable-memory write exceptions also applied to FEX's own JIT buffers, an endless exception loop (x64 ntdll `virtual` hang) | Exempt ARM64EC code allocations |
| The free-area search probed occupied address space 64K at a time; once FEX's per-thread reservations used up Wine's reserved area, thread creation stalled (x64 ntdll `threadpool`: 3/512 callbacks in 20s) | Skip whole regions with `mach_vm_region()`; now 512/512 in 66 ms |
| FEX: `ThreadTerm` dereferenced the TEB of another process's thread (x64 kernel32 `process` and `loader` crashes) | Ignore threads of other processes |
| FEX: DF lost on a fault in the same block as `std` (cached context store), and AF/DF missing from ARM64EC contexts | Write DF through; carry AF/DF in RES0 CPSR bits |
| Vulkan: the loader hid MoltenVK without portability enumeration (`VK_ERROR_INCOMPATIBLE_DRIVER`) | Enable `VK_KHR_portability_enumeration` |

## Expected x86_64-only differences (ARM64EC semantics, same as upstream Wine on any ARM64 host)

- An x64 process can't `LoadLibrary` Wine's ARM64 programs such as `winver.exe` (ERROR_BAD_EXE_FORMAT).
- The process machine reports `0xaa64`.
- Thread contexts and start addresses come back in their ARM64EC form, and ARM64X DLL headers look different from x64.
- `DbgBreakPoint` is a native `brk`, so x64 handlers that skip one byte resume mid-instruction (ntdll `rtl`).
- FEX doesn't raise unmasked SSE floating-point exceptions (ntdll `exception`).

## Not port bugs

- **kernel32 `loader` test 7.** ARM64 Windows refuses images without `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`. Wine implements that, and test 6 expects `ERROR_BAD_EXE_FORMAT` for ARM64. Test 7 loads the same image through `LdrLoadDll` without that ARM64 exception and then dereferences the NULL module. It is an upstream test bug on every ARM64 platform.

