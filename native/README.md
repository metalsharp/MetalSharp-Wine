# Native Linux module host (aarch64)

Runs aarch64 Linux shared libraries natively in an arm64 macOS process. It exists because the anti-cheat code some games ship for Proton is an aarch64 Linux library: Elden Ring's `eossdk-win64-shipping.so` is Epic's `libEOSSDK-LinuxArm64-Shipping.so` (EOS 1.19), which carries Easy Anti-Cheat's client. Valve's ARM Proton runs it on an arm64 Linux host; here Apple silicon runs it with no translation.

The x86_64 host in `..` (under FEX) remains for libraries that only exist for x86_64.

## How it works

| File | What it does |
|---|---|
| `elfload.c` | Maps an ELF64 aarch64 shared library, applies its relocations (including TLS descriptors and symbol versions), resolves its imports, gives each segment its protection and runs its constructors. An import nobody provides becomes a stub that names it when called |
| `tlsdesc.S` | The TLS descriptor resolver, and the entry the unresolved-import stubs jump to |
| `glibc*.c`, `glibc_asm.S` | glibc's ABI on macOS's libc, in areas: core (strings, math, files, memory, process), threads, signals and contexts, stdio and formatted output, sockets and epoll, locales and long double, dynamic loading and unwinding |
| `glibc_proc.c` | The Linux files a library reads about its process: `/proc/self/{exe,maps,status,cmdline}`, `/proc/meminfo`, `/etc/machine-id` |
| `eoshost.c` | Test program |

Most imports mean the same on both systems and are passed through. The rest is converted:

- **Structures** whose layout differs: `stat`, `dirent`, `utsname`, `sysinfo`, `passwd`, `sockaddr`, `addrinfo`, `msghdr`/`cmsghdr`, `ifaddrs`, `sigaction`, `flock`.
- **Constants**: open and fcntl flags, errno numbers above 34, signal numbers, clock ids, `sysconf` names, socket options and message flags, poll bits, getaddrinfo flags and errors.
- **Sizes**: glibc's pthread objects are smaller than macOS's; each holds a pointer to a macOS one, made on first use, so glibc's zero initializers work.
- **Missing on macOS**: epoll (on kqueue), unnamed semaphores, `recvmmsg`, quad-precision `long double` (`strtold` widens a double; `__lttf2` compares).
- **Variadic functions**: Linux passes variadic arguments in registers, Apple on the stack. The printf and scanf family enter through `glibc_asm.S`, which builds Linux's `va_list`; the arguments are then laid out in 8-byte slots, which is Apple's `va_list`, and macOS's own formatting functions do the work.
- **Exceptions**: libgcc's `_Unwind_*` and macOS's libunwind implement the same ABI. Each library's `.eh_frame` is registered with macOS's unwinder.
- **Signals** are virtual: the library's handlers and masks are recorded and reported back, never installed, so it cannot take signals from Wine.

Two machine-level differences matter:

- **x18.** Linux code uses it as a scratch register (EOS writes it about 3,500 times). macOS preserves it across context switches only for programs whose Mach-O SDK version is 12.0 or lower, so the host is linked with `-platform_version macos 15.0 12.0`, as Wine's loader is.
- **tpidr_el0.** On Linux it is the thread pointer. On macOS it holds the CPU number, writes are discarded, and it changes when a thread moves to another core. The library's TLS descriptors resolve against its value at the moment of the call, which is right unless the thread migrates between the descriptor call and the read that follows. The complete fix is to rewrite each `mrs xN, tpidr_el0` (395 in EOS) to read `tpidrro_el0`, which macOS keeps per thread; that modifies the vendor's code, which anti-cheat may check, so it is not done.

## Build and test

```bash
darwin/linuxhost/native/build.sh <output dir>
```

```bash
<output dir>/eoshost inspect <library.so>
```

```bash
<output dir>/eoshost eos <library.so>
```

```bash
<output dir>/eoshost proc <library.so>
```

`inspect` loads the library and lists imports nobody provides. `eos` also runs its constructors and starts and stops Epic Online Services in it. `proc` prints the Linux files as the library would read them. `LINUXHOST_TRACE=1` lists the constructors as they run; `LINUXHOST_BREAK=<offset>,...` puts a breakpoint at offsets in the library and reports the registers there.

## State

Tested 2026-11-11 on an Apple M6, macOS 27.2, with Elden Ring's EOS library:

- All 53,744 relocations apply; all 428 imports resolve.
- The 15 constructors run.
- `EOS_GetVersion` reports `1.19.0.7-52449437+LinuxAArch64_v19_clang-11.0.1-centos7`; `EOS_Initialize` and `EOS_Shutdown` succeed.

Not done:

- **Wine.** The host runs as its own program. Under Proton, the EOS bootstrapper (`start_protected_game.exe`) starts the game itself from the Linux side (Valve's kernelbase comment: "EOS EAC bootstrapper will start the game process directly without using WINAPI") and keeps a mapping in `/tmp/eac_wine_pid_%X`. How the Windows EOS SDK then reaches this library has to be traced with the host in Wine.
- **EOS platform and anti-cheat.** Nothing past `EOS_Initialize` has run: no `EOS_Platform_Create`, no network, no anti-cheat client. EAC downloads its module at run time; what that module is, and what it checks, is unknown.
- The TLS register rewrite described above.
