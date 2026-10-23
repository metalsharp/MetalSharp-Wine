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
