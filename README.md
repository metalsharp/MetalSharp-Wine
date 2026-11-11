# Linux module host

Loads Linux x86_64 shared libraries into a Wine process on macOS and runs them. It exists for Proton's anti-cheat support, where the code that does the checking is the vendor's Linux library (`SPEC.md` §9).

## How it works

The host is ordinary x86_64 Windows code. Under MetalSharp Wine, FEX translates it, and FEX translates the Linux library the same way: to FEX it is more x86_64 code in the process. Nothing here runs as native macOS code, and the library is never modified.

| File | What it does |
|---|---|
| `elfload.c` | Maps an ELF64 x86_64 shared library, applies its relocations, resolves its imports, runs its constructors, and gives each thread its thread-local storage |
| `glibc.c` | The glibc functions libraries import, with the System V calling convention and glibc's types, on top of the Windows C runtime |
| `syscall.c` | Receives the library's direct Linux system calls and answers them |
| `tools/elfinfo.py` | Lists what a library needs from a host: imports, segments, thread-local storage, direct system calls |

Calls between the host and a library use the System V convention (`SYSV` in `elfload.h`), which the compiler handles for Windows code.

**Direct system calls.** A library can use the `syscall` instruction with a Linux call number. FEX hands every `syscall` to Wine, which would take it for a Windows call. The host registers each library's address range with Wine (`__wine_register_linux_syscalls`, in MetalSharp Wine's `proton-macos` branch), and Wine turns a `syscall` inside a registered range into a jump to the host's handler, with the return address in `rcx` and nothing written to the library's stack. The handler steps over the 128 bytes below the stack pointer, which belong to the library, aligns the stack, and returns the result in `rax`.

## Build and test

```bash
darwin/linuxhost/build.sh <llvm-mingw dir> <output dir>
```

That builds `hosttest.exe` and `hello.so`, a Linux library compiled without a C library.

```bash
wine hosttest.exe hello /path/to/hello.so
```

```bash
wine hosttest.exe load /path/to/library.so
```

```bash
wine hosttest.exe inspect /path/to/library.so
```

`hello` loads the test library and checks calls in both directions, thread-local storage, a mutex and direct system calls. `load` loads libraries, reports their imports and runs their constructors. `inspect` does the same without running any of the library's code.

## State

Working, tested on an Apple M6 under FEX:

- The test library: relocations, a constructor, thread-local storage through `__tls_get_addr`, six integer and two floating-point arguments, a callback into the host, a mutex.
- Direct system calls: `getpid` returns the process id, an unknown call returns `-ENOSYS` and is reported.
- Valve's bridges from the Proton runtimes: `easyanticheat_x64.so` and `beclient_x64.so` load with no unresolved import, their constructors run, and their `__wine_unix_call_funcs` tables are found (24 and 17 entry points).

Not done:

- **The glibc layer has 36 functions.** BattlEye's client needs 134 more, among them file and socket I/O, `mmap`, `uname`, `getmntent`, the C++ runtime and the unwinder. `hosttest inspect` lists them.
- **Four system calls are implemented**: `write`, `getpid`, `gettid`, `sched_yield`.
- **No Linux view of the process**: `/proc`, `/etc/machine-id`, the auxiliary vector.
- **No `%fs` thread pointer.** Libraries built with a stack protector or the initial-exec TLS model read `%fs:0x28` and `%fs:0`. Valve's bridges do not; BattlEye's client appears to. It needs a per-thread block and a way to set the `fs` base under FEX.
- **Not connected to Wine's loader.** A bridge's Windows half reaches its unix half through `__wine_unix_call`. Wine has to hand those calls to a hosted library instead of looking for a native one.
- **i386 libraries.**
- **Nothing has been run against a real anti-cheat service.**
