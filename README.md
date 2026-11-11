# Linux module host

Loads Linux x86_64 shared libraries into a Wine process on macOS and runs them. For aarch64 Linux libraries, which Apple silicon runs without translation, see `native/` (Elden Ring's Easy Anti-Cheat ships as one). It exists for Proton's anti-cheat support, where the code that does the checking is the vendor's Linux library (`SPEC.md` §9).

## How it works

The host is ordinary x86_64 Windows code. Under MetalSharp Wine, FEX translates it, and FEX translates the Linux library the same way: to FEX it is more x86_64 code in the process. Nothing here runs as native macOS code, and the library is never modified.

| File | What it does |
|---|---|
| `elfload.c` | Maps an ELF64 x86_64 shared library, applies its relocations, resolves its imports, runs its constructors, and gives each thread its thread-local storage |
| `glibc.c` | The resolver, the dynamic linker's functions (`dlopen` of an in-memory file, `dlsym`) and the core of glibc, with the System V calling convention and glibc's types |
| `libc.c` | Strings, wide strings, `iconv`, time, the environment, `uname`, `sysconf`, mutexes, `dl_iterate_phdr` |
| `libc_io.c` | Files, directories, `glob`, stdio and formatted output, on the host's Linux file descriptors |
| `libc_thread.c` | Threads, condition variables, semaphores, `nanosleep`, on Windows threads |
| `libc_net.c` | Sockets, `getaddrinfo`, `poll`, `select` and `epoll`, on Winsock |
| `libc_misc.c` | Locales (libstdc++'s GNU model reads glibc's locale layout), UTF-8 wide characters, message catalogs, `mmap`, `getpwuid`, POSIX shared memory, `signal`, glibc's `rand` |
| `unwind.c` | The C++ unwinder (`_Unwind_*`): DWARF call frame information from each library's `.eh_frame_hdr`, two-phase raise, cleanups, landing pads |
| `fork.c` | `fork`, `exec`, `ptrace` and `waitpid` as Easy Anti-Cheat's module launches a game with them |
| `process.c` | The descriptor table (files, memory files with seals, sockets, epoll sets, `/dev/urandom`), the made-up `/proc` and `/sys` files and directories, the `%fs` thread block, and the made-up `libc.so.6` a bootstrapper looks for |
| `syscall.c` | Receives the library's direct Linux system calls and answers them |
| `dllmain.c` | `linuxhost.dll`: sets it all up when Wine's loader loads it into a process (`PROTON_LINUX_HOST`) |
| `tools/elfinfo.py` | Lists what a library needs from a host: imports, segments, thread-local storage, direct system calls |

Calls between the host and a library use the System V convention (`SYSV` in `elfload.h`), which the compiler handles for Windows code.

**Direct system calls.** A library can use the `syscall` instruction with a Linux call number. FEX hands every `syscall` to Wine, which would take it for a Windows call. The host registers each library's address range with Wine (`__wine_register_linux_syscalls`, in MetalSharp Wine's `proton-macos` branch), and Wine turns a `syscall` inside a registered range into a jump to the host's handler, with the return address in `rcx` and nothing written to the library's stack. The handler steps over the 128 bytes below the stack pointer, which belong to the library, aligns the stack, and returns the result in `rax`.

## Build and test

```bash
darwin/linuxhost/build.sh <llvm-mingw dir> <output dir>
```

That builds `hosttest.exe`, `linuxhost.dll`, and `hello.so` and `extras.so`, Linux libraries compiled without a C library.

```bash
wine hosttest.exe hello /path/to/hello.so
```

```bash
wine hosttest.exe extras /path/to/extras.so
```

```bash
wine hosttest.exe load /path/to/library.so
```

```bash
wine hosttest.exe inspect /path/to/library.so
```

`hello` loads the test library and checks calls in both directions, thread-local storage, a mutex and direct system calls. `extras` throws an exception through a cleanup frame to a handler, takes a backtrace, and checks sockets, `poll`, `epoll`, threads, `mmap`, `glob` on `/proc`, `/dev/urandom`, the C locale's tables, UTF-8 conversions and glibc's `rand` sequence. `load` loads libraries, reports their imports and runs their constructors. `inspect` does the same without running any of the library's code.

## State

Working, tested on an Apple M6 under FEX:

- The test libraries: everything `hello` and `extras` check.
- Direct system calls: `read`, `write`, `open`, `openat`, `close`, `fstat`, `lseek`, `unlink`, `memfd_create`, `getpid`, `gettid`, `sched_yield`. An unknown call returns `-ENOSYS` and is reported.
- Valve's bridges from the Proton runtimes: `easyanticheat_x64.so` and `beclient_x64.so` load with no unresolved import, their constructors run, and their `__wine_unix_call_funcs` tables are found (24 and 17 entry points).
- Easy Anti-Cheat's EOS bootstrapper, through Wine's loader: its Linux module and its in-game client resolve every import they make. The module runs its checks, forks, traces the child and starts the game.

Not done:

- **The trace of the game.** EAC's module waits for the game's first Linux system call, through `ptrace`, to put its in-game client into it. A Windows game under FEX makes none.
- **Initial-exec thread-local storage** (`R_X86_64_TPOFF64`): counted as unsupported. Neither EAC library uses it.
- **Signals** are never delivered: `signal` records nothing.
- **A file mapped `MAP_SHARED`** is a private copy.
- **One locale**, C with UTF-8.
- **i386 libraries.**
