/* Linux module host: the Linux process a Windows program expects to be in under Proton.
 *
 * Easy Anti-Cheat's EOS bootstrapper (start_protected_game.exe), on Wine, loads EAC's Linux module into its own
 * process with Linux's own means: it copies a stub that executes the syscall instruction to memory it allocates
 * at 0x700000000000 or above, where Valve's Wine lets system calls through to the kernel, and with it creates an
 * in-memory file (memfd_create), writes the module to it, reads /proc/<pid>/maps to find glibc, finds dlopen and
 * dlsym in glibc's ELF image and calls dlopen("/proc/self/fd/<n>").
 *
 * Here there is no Linux kernel and no glibc in the process. The host registers the region at 0x700000000000 with
 * Wine, so those system calls come to linux_syscall (syscall.c), which calls the functions below; it keeps a table
 * of Linux file descriptors (in-memory files and the /proc files it makes up); and it maps a small ELF image named
 * libc.so.6 whose dlopen and dlsym are the host's (glibc.c), which load the module with the host's ELF loader. */
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "process.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define LX_EACCES 13
#define LX_ENOENT 2
#define LX_EBADF  9
#define LX_ENOMEM 12
#define LX_EINVAL 22
#define LX_EMFILE 24

/* ---------------------------------------------------------------------------------------------------------
 * File descriptors
 */

enum fd_kind { FD_FREE, FD_MEMORY, FD_HANDLE, FD_SOCKET, FD_EPOLL, FD_DEVICE };
enum device { DEV_RANDOM = 1, DEV_NULL, DEV_ZERO };

struct fd_entry
{
    enum fd_kind kind;
    HANDLE handle;         /* FD_HANDLE */
    int flags;             /* Linux open flags */
    unsigned int seals;    /* memfd seals: F_SEAL_SEAL 1, SHRINK 2, GROW 4, WRITE 8 */
    enum device device;    /* FD_DEVICE */
    char *data;
    size_t size, capacity, pos;
    char name[64];
};

#define FIRST_FD 3
#define MAX_FDS 256
static struct fd_entry fds[MAX_FDS];
/* initialised statically (unowned, its event made when first contended), so descriptors work before process init */
static CRITICAL_SECTION fd_lock = { NULL, -1, 0, 0, 0, 0 };

static int alloc_fd( const char *name )
{
    int fd;

    EnterCriticalSection( &fd_lock );
    for (fd = FIRST_FD; fd < MAX_FDS; fd++) if (fds[fd].kind == FD_FREE) break;
    if (fd == MAX_FDS)
    {
        LeaveCriticalSection( &fd_lock );
        return -LX_EMFILE;
    }
    memset( &fds[fd], 0, sizeof(fds[fd]) );
    fds[fd].kind = FD_MEMORY;
    snprintf( fds[fd].name, sizeof(fds[fd].name), "%s", name );
    LeaveCriticalSection( &fd_lock );
    return fd;
}

static struct fd_entry *get_fd( long long fd )
{
    if (fd < FIRST_FD || fd >= MAX_FDS || fds[fd].kind == FD_FREE) return NULL;
    return &fds[fd];
}

static int reserve( struct fd_entry *f, size_t size )
{
    char *data;
    size_t capacity;

    if (size <= f->capacity) return 1;
    capacity = f->capacity ? f->capacity : 4096;
    while (capacity < size) capacity *= 2;
    if (!(data = realloc( f->data, capacity ))) return 0;
    memset( data + f->capacity, 0, capacity - f->capacity );
    f->data = data;
    f->capacity = capacity;
    return 1;
}

/* a descriptor for a Windows object (a socket, an epoll set's private data): returns the descriptor or -errno */
int linuxhost_new_fd( int kind, HANDLE handle, const char *name )
{
    int fd = alloc_fd( name );

    if (fd < 0) return fd;
    fds[fd].kind = kind;
    fds[fd].handle = handle;
    return fd;
}

/* the kind and Windows object of a descriptor; FD_FREE (0) if it is not open */
int linuxhost_fd_kind( int fd, HANDLE *handle )
{
    struct fd_entry *f = get_fd( fd );

    if (!f) return FD_FREE;
    if (handle) *handle = f->handle;
    return f->kind;
}

/* where an epoll set keeps its registrations */
void **linuxhost_fd_private( int fd )
{
    struct fd_entry *f = get_fd( fd );

    return f ? (void **)&f->data : NULL;
}

int linuxhost_fd_contents( int fd, const void **data, size_t *size )
{
    struct fd_entry *f = get_fd( fd );

    if (!f) return 0;
    *data = f->data;
    *size = f->size;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------
 * The made-up libc
 */

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Elf64_Phdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; } Elf64_Shdr;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64_Sym;
typedef struct { int64_t tag; uint64_t val; } Elf64_Dyn;

static uint8_t *libc_image;
static size_t libc_size;
static const char libc_path[] = "/usr/lib/x86_64-linux-gnu/libc.so.6";

static uint32_t gnu_hash( const char *s )
{
    uint32_t h = 5381;

    while (*s) h = h * 33 + (uint8_t)*s++;
    return h;
}

/* An ELF image as glibc's would be in memory, with the functions given as its dynamic symbols: each symbol is an
 * indirect jump to the host's function. Its dynamic section has absolute addresses, as ld.so leaves glibc's. */
static int build_libc( const struct libc_export *exports, unsigned int count )
{
    enum { SEC_NULL, SEC_HASH, SEC_GNU_HASH, SEC_DYNSYM, SEC_DYNSTR, SEC_TEXT, SEC_DYNAMIC, SEC_SHSTRTAB, SEC_COUNT };
    static const char shstrtab[] = "\0.hash\0.gnu.hash\0.dynsym\0.dynstr\0.text\0.dynamic\0.shstrtab";
    static const uint32_t shname[SEC_COUNT] = { 0, 1, 7, 17, 25, 33, 39, 48 };
    unsigned int nsyms = count + 1, i;
    size_t off_hash, off_gnu, off_sym, off_str, off_text, off_dyn, off_shstr, off_sh, str_size = 1, end;
    uint8_t *p;
    Elf64_Ehdr *eh;
    Elf64_Phdr *ph;
    Elf64_Shdr *sh;
    Elf64_Sym *sym;
    Elf64_Dyn *dyn;
    uint32_t *hash, *gnu;
    char *str;

    for (i = 0; i < count; i++) str_size += strlen( exports[i].name ) + 1;
    off_hash = 0x200;
    off_gnu = off_hash + (2 + 1 + nsyms) * 4;
    off_gnu = (off_gnu + 7) & ~7;
    off_sym = off_gnu + 16 + 8 + 4 + count * 4;
    off_sym = (off_sym + 7) & ~7;
    off_str = off_sym + nsyms * sizeof(Elf64_Sym);
    off_text = (off_str + str_size + 15) & ~15;
    off_dyn = (off_text + count * 16 + 15) & ~15;
    off_shstr = off_dyn + 8 * sizeof(Elf64_Dyn);
    off_sh = (off_shstr + sizeof(shstrtab) + 7) & ~7;
    end = off_sh + SEC_COUNT * sizeof(Elf64_Shdr);
    libc_size = (end + 0xffff) & ~0xffff;

    if (!(p = VirtualAlloc( NULL, libc_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE ))) return 0;

    eh = (Elf64_Ehdr *)p;
    memcpy( eh->ident, "\177ELF\2\1\1", 7 );
    eh->type = 3;  /* ET_DYN */
    eh->machine = 62;
    eh->version = 1;
    eh->phoff = sizeof(*eh);
    eh->shoff = off_sh;
    eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(Elf64_Phdr);
    eh->phnum = 2;
    eh->shentsize = sizeof(Elf64_Shdr);
    eh->shnum = SEC_COUNT;
    eh->shstrndx = SEC_SHSTRTAB;

    ph = (Elf64_Phdr *)(p + eh->phoff);
    ph[0].type = 1;  /* PT_LOAD */
    ph[0].flags = 5;
    ph[0].filesz = ph[0].memsz = libc_size;
    ph[0].align = 0x1000;
    ph[1].type = 2;  /* PT_DYNAMIC */
    ph[1].flags = 4;
    ph[1].offset = ph[1].vaddr = ph[1].paddr = off_dyn;
    ph[1].filesz = ph[1].memsz = 8 * sizeof(Elf64_Dyn);
    ph[1].align = 8;

    sym = (Elf64_Sym *)(p + off_sym);
    str = (char *)(p + off_str);
    for (i = 0, str_size = 1; i < count; i++)
    {
        uint8_t *code = p + off_text + i * 16;
        uint64_t target = (uint64_t)exports[i].function;

        strcpy( str + str_size, exports[i].name );
        sym[i + 1].name = str_size;
        sym[i + 1].info = (1 << 4) | 2;  /* STB_GLOBAL, STT_FUNC */
        sym[i + 1].shndx = SEC_TEXT;
        sym[i + 1].value = off_text + i * 16;
        sym[i + 1].size = 16;
        str_size += strlen( exports[i].name ) + 1;
        code[0] = 0xff; code[1] = 0x25; memset( code + 2, 0, 4 );  /* jmp *0(%rip) */
        memcpy( code + 6, &target, 8 );
    }

    /* DT_HASH: one bucket holding the last symbol, each chained to the one before */
    hash = (uint32_t *)(p + off_hash);
    hash[0] = 1;
    hash[1] = nsyms;
    hash[2] = nsyms - 1;
    for (i = 0; i < nsyms; i++) hash[3 + i] = i ? i - 1 : 0;

    /* DT_GNU_HASH: one bucket, a bloom filter that lets everything through */
    gnu = (uint32_t *)(p + off_gnu);
    gnu[0] = 1;   /* nbuckets */
    gnu[1] = 1;   /* symoffset */
    gnu[2] = 1;   /* bloom words */
    gnu[3] = 6;   /* bloom shift */
    memset( gnu + 4, 0xff, 8 );
    gnu[6] = 1;   /* bucket 0: the first symbol */
    for (i = 0; i < count; i++) gnu[7 + i] = (gnu_hash( exports[i].name ) & ~1u) | (i == count - 1);

    dyn = (Elf64_Dyn *)(p + off_dyn);
    dyn[0].tag = 4;  dyn[0].val = (uint64_t)p + off_hash;      /* DT_HASH */
    dyn[1].tag = 0x6ffffef5; dyn[1].val = (uint64_t)p + off_gnu;  /* DT_GNU_HASH */
    dyn[2].tag = 5;  dyn[2].val = (uint64_t)p + off_str;       /* DT_STRTAB */
    dyn[3].tag = 6;  dyn[3].val = (uint64_t)p + off_sym;       /* DT_SYMTAB */
    dyn[4].tag = 10; dyn[4].val = str_size;                   /* DT_STRSZ */
    dyn[5].tag = 11; dyn[5].val = sizeof(Elf64_Sym);          /* DT_SYMENT */
    dyn[6].tag = 14; dyn[6].val = 0;                          /* DT_SONAME: "" */
    dyn[7].tag = 0;

    memcpy( p + off_shstr, shstrtab, sizeof(shstrtab) );
    sh = (Elf64_Shdr *)(p + off_sh);
    for (i = 0; i < SEC_COUNT; i++) sh[i].name = shname[i];
    sh[SEC_HASH].type = 5;  sh[SEC_HASH].offset = sh[SEC_HASH].addr = off_hash;
    sh[SEC_HASH].size = (3 + nsyms) * 4; sh[SEC_HASH].link = SEC_DYNSYM; sh[SEC_HASH].entsize = 4;
    sh[SEC_GNU_HASH].type = 0x6ffffff6; sh[SEC_GNU_HASH].offset = sh[SEC_GNU_HASH].addr = off_gnu;
    sh[SEC_GNU_HASH].size = 28 + count * 4; sh[SEC_GNU_HASH].link = SEC_DYNSYM;
    sh[SEC_DYNSYM].type = 11; sh[SEC_DYNSYM].offset = sh[SEC_DYNSYM].addr = off_sym;
    sh[SEC_DYNSYM].size = nsyms * sizeof(Elf64_Sym); sh[SEC_DYNSYM].link = SEC_DYNSTR; sh[SEC_DYNSYM].info = 1;
    sh[SEC_DYNSYM].entsize = sizeof(Elf64_Sym); sh[SEC_DYNSYM].addralign = 8; sh[SEC_DYNSYM].flags = 2;
    sh[SEC_DYNSTR].type = 3; sh[SEC_DYNSTR].offset = sh[SEC_DYNSTR].addr = off_str; sh[SEC_DYNSTR].size = str_size;
    sh[SEC_DYNSTR].flags = 2;
    sh[SEC_TEXT].type = 1; sh[SEC_TEXT].offset = sh[SEC_TEXT].addr = off_text; sh[SEC_TEXT].size = count * 16;
    sh[SEC_TEXT].flags = 6; sh[SEC_TEXT].addralign = 16;
    sh[SEC_DYNAMIC].type = 6; sh[SEC_DYNAMIC].offset = sh[SEC_DYNAMIC].addr = off_dyn;
    sh[SEC_DYNAMIC].size = 8 * sizeof(Elf64_Dyn); sh[SEC_DYNAMIC].link = SEC_DYNSTR; sh[SEC_DYNAMIC].entsize = 16;
    sh[SEC_DYNAMIC].flags = 3;
    sh[SEC_SHSTRTAB].type = 3; sh[SEC_SHSTRTAB].offset = off_shstr; sh[SEC_SHSTRTAB].size = sizeof(shstrtab);

    {
        DWORD old;
        VirtualProtect( p, libc_size, PAGE_EXECUTE_READ, &old );
    }
    FlushInstructionCache( GetCurrentProcess(), p, libc_size );
    libc_image = p;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------
 * /proc
 */

/* the names Linux code was loaded under, as /proc/self/maps shows them (a memfd: "/memfd:<name> (deleted)") */
static struct { void *base; size_t size; char path[256]; } module_names[16];
static unsigned int module_name_count;

void linuxhost_note_module_path( void *base, size_t size, const char *path )
{
    if (module_name_count == ARRAY_SIZE(module_names)) return;
    module_names[module_name_count].base = base;
    module_names[module_name_count].size = size;
    snprintf( module_names[module_name_count].path, sizeof(module_names[0].path), "%s", path );
    module_name_count++;
}

/* how an in-memory file shows in /proc/self/maps once mapped */
int linuxhost_fd_maps_name( int fd, char *buf, size_t size )
{
    struct fd_entry *f = get_fd( fd );

    if (!f || f->kind != FD_MEMORY) return 0;
    snprintf( buf, size, "/memfd:%s (deleted)", f->name );
    return 1;
}

/* what /proc/self/fd/<fd> links to */
int linuxhost_fd_link( int fd, char *buf, size_t size )
{
    struct fd_entry *f;

    if (fd >= 0 && fd < FIRST_FD)
    {
        snprintf( buf, size, "/dev/pts/0" );
        return 1;
    }
    if (!(f = get_fd( fd ))) return 0;
    switch (f->kind)
    {
    case FD_MEMORY:
        if (f->name[0] == '/') snprintf( buf, size, "%s", f->name );  /* a made-up /proc file */
        else snprintf( buf, size, "/memfd:%s (deleted)", f->name );
        break;
    case FD_SOCKET: snprintf( buf, size, "socket:[%llu]", (unsigned long long)(UINT_PTR)f->handle + 40000 ); break;
    case FD_EPOLL: snprintf( buf, size, "anon_inode:[eventpoll]" ); break;
    default: snprintf( buf, size, "%s", f->name ); break;
    }
    return 1;
}

/* the file a region is mapped from, as a Unix path: an image Wine mapped, or Linux code the host loaded */
static int region_path( const MEMORY_BASIC_INFORMATION *info, char *buf, size_t size )
{
    static char cached[1024];
    static void *cached_base;
    typedef char *(CDECL *get_unix_fn)( const WCHAR * );
    static get_unix_fn get_unix;
    WCHAR name[MAX_PATH];
    char *unix_name;
    unsigned int i;

    for (i = 0; i < module_name_count; i++)
        if ((char *)info->BaseAddress >= (char *)module_names[i].base &&
            (char *)info->BaseAddress < (char *)module_names[i].base + module_names[i].size)
        {
            snprintf( buf, size, "%s", module_names[i].path );
            return 1;
        }
    if (info->Type != MEM_IMAGE) return 0;
    if (info->AllocationBase == cached_base && cached[0])
    {
        snprintf( buf, size, "%s", cached );
        return 1;
    }
    if (!get_unix) get_unix = (get_unix_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_unix_file_name" );
    if (!get_unix || !GetModuleFileNameW( (HMODULE)info->AllocationBase, name, MAX_PATH )) return 0;
    if (!(unix_name = get_unix( name ))) return 0;
    snprintf( cached, sizeof(cached), "%s", unix_name );
    HeapFree( GetProcessHeap(), 0, unix_name );
    cached_base = info->AllocationBase;
    snprintf( buf, size, "%s", cached );
    return 1;
}

/* /proc/<pid>/maps: the process's memory as Windows sees it, with the files mapped in it, and the made-up libc */
static int make_maps( struct fd_entry *f )
{
    MEMORY_BASIC_INFORMATION info;
    char *addr = NULL;
    char line[1400], path[1024];

    while (VirtualQuery( addr, &info, sizeof(info) ) == sizeof(info))
    {
        char *end = (char *)info.BaseAddress + info.RegionSize;

        if (info.State == MEM_COMMIT && (info.BaseAddress != libc_image))
        {
            int has_path = region_path( &info, path, sizeof(path) );
            DWORD p = info.Protect & 0xff;
            char r = (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) ? 'r' : '-';
            char w = (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ? 'w' : '-';
            char x = (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ? 'x' : '-';
            int len;

            if (has_path)
                len = snprintf( line, sizeof(line), "%08llx-%08llx %c%c%c%c %08llx 08:01 %lu                    %s\n",
                                (unsigned long long)info.BaseAddress, (unsigned long long)end, r, w, x, 'p',
                                (unsigned long long)((char *)info.BaseAddress - (char *)info.AllocationBase),
                                (unsigned long)((uintptr_t)info.AllocationBase >> 12) & 0xfffff, path );
            else
                len = snprintf( line, sizeof(line), "%08llx-%08llx %c%c%c%c 00000000 00:00 0\n",
                                (unsigned long long)info.BaseAddress, (unsigned long long)end, r, w, x,
                                info.Type == MEM_PRIVATE ? 'p' : 's' );

            if (!reserve( f, f->size + len )) return 0;
            memcpy( f->data + f->size, line, len );
            f->size += len;
        }
        if (end <= addr) break;
        addr = end;
    }
    if (libc_image)
    {
        int len = snprintf( line, sizeof(line), "%08llx-%08llx r-xp 00000000 08:01 1048577                    %s\n",
                            (unsigned long long)libc_image, (unsigned long long)(libc_image + libc_size), libc_path );

        if (!reserve( f, f->size + len )) return 0;
        memcpy( f->data + f->size, line, len );
        f->size += len;
    }
    return 1;
}

/* The made-up file a /proc or /sys path names: an entry of this process ("maps"), of init ("init-status"), of a
 * child ("child-<pid>-status") or of the machine ("sys-mounts"); NULL if there is none. A thread's own entries
 * (/proc/self/task/<tid>/...) are the process's. */
static const char *proc_self( const char *path )
{
    static const struct { const char *path, *entry; } machine[] =
    {
        { "/proc/mounts", "sys-mounts" }, { "/proc/bus/pci/devices", "sys-pci" }, { "/proc/cpuinfo", "sys-cpuinfo" },
        { "/proc/meminfo", "sys-meminfo" }, { "/proc/version", "sys-version" }, { "/proc/uptime", "sys-uptime" },
        { "/proc/loadavg", "sys-loadavg" }, { "/proc/stat", "sys-stat" }, { "/proc/filesystems", "sys-filesystems" },
        { "/proc/modules", "sys-empty" }, { "/proc/cmdline", "sys-cmdline" },
        { "/proc/sys/kernel/osrelease", "sys-osrelease" }, { "/proc/sys/kernel/ostype", "sys-ostype" },
        { "/proc/sys/kernel/hostname", "sys-hostname" }, { "/proc/sys/kernel/pid_max", "sys-pid_max" },
        { "/proc/sys/kernel/yama/ptrace_scope", "sys-ptrace_scope" }, { "/proc/sys/kernel/random/boot_id", "sys-boot_id" },
        { "/proc/sys/kernel/random/uuid", "sys-uuid" }, { "/proc/sys/kernel/tainted", "sys-zero" },
        { "/proc/sys/kernel/unprivileged_userns_clone", "sys-one" }, { "/proc/sys/fs/file-max", "sys-file_max" },
    };
    static __thread char entry[96];
    const char *rest;
    char prefix[32];
    unsigned int i;
    int len, pid;

    for (i = 0; i < ARRAY_SIZE(machine); i++) if (!strcmp( path, machine[i].path )) return machine[i].entry;
    /* the firmware's identity: the machine as it is, a Mac */
    if (!strncmp( path, "/sys/devices/virtual/dmi/id/", 28 ) || !strncmp( path, "/sys/class/dmi/id/", 18 ))
    {
        snprintf( entry, sizeof(entry), "dmi-%s", strrchr( path, '/' ) + 1 );
        return entry;
    }
    /* this process is a child of init (pid 1), which has no parent and no tracer */
    if (!strncmp( path, "/proc/1/", 8 ))
    {
        snprintf( entry, sizeof(entry), "init-%s", path + 8 );
        return entry;
    }
    len = snprintf( prefix, sizeof(prefix), "/proc/%lu/", GetCurrentProcessId() );
    if (!strncmp( path, "/proc/self/", 11 )) rest = path + 11;
    else if (!strncmp( path, "/proc/thread-self/", 18 )) rest = path + 18;
    else if (!strncmp( path, prefix, len )) rest = path + len;
    else if (sscanf( path, "/proc/%d/%n", &pid, &len ) == 1 && len)
    {
        char state;
        int traced;
        unsigned long windows_pid;

        if (!linuxhost_child_info( pid, &state, &traced, &windows_pid )) return NULL;
        snprintf( entry, sizeof(entry), "child-%d-%s", pid, path + len );
        return entry;
    }
    else return NULL;
    if (!strncmp( rest, "task/", 5 ) && (rest = strchr( rest + 5, '/' ))) rest++;
    return rest;
}

static int append_text( struct fd_entry *f, const char *text, size_t len )
{
    if (!reserve( f, f->size + len )) return 0;
    memcpy( f->data + f->size, text, len );
    f->size += len;
    return 1;
}

/* The Unix path of this process's executable, as /proc/self/exe gives it: Wine's loader, which Wine names in
 * WINELOADER (an NT path, \??\Z:\...), as on Linux under Proton. */
int linuxhost_exe_path( char *buf, size_t size )
{
    WCHAR loader[MAX_PATH], *p = loader;
    char *unix_name;
    typedef char *(CDECL *get_unix_fn)( const WCHAR * );
    get_unix_fn get_unix = (get_unix_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_unix_file_name" );

    if (!GetEnvironmentVariableW( L"WINELOADER", loader, MAX_PATH )) return linuxhost_program_path( buf, size );
    if (!wcsncmp( p, L"\\??\\", 4 )) p += 4;
    if (!get_unix || !(unix_name = get_unix( p ))) return linuxhost_program_path( buf, size );
    snprintf( buf, size, "%s", unix_name );
    HeapFree( GetProcessHeap(), 0, unix_name );
    {
        /* On Linux, a Wine process's executable is Wine's preloader, which loads the wine binary beside it; EAC
         * starts the game the same way, deriving the wine binary from that name (its exec comes to the host, which
         * starts the program). MetalSharp Wine's loader needs no preloader, so the name is Proton's 64-bit one for the binary
         * that is there. LINUXHOST_EXE_NAME gives another name, to test. */
        char name[64] = "wine64-preloader", *slash = strrchr( buf, '/' );

        GetEnvironmentVariableA( "LINUXHOST_EXE_NAME", name, sizeof(name) );
        if (slash) snprintf( slash + 1, size - (slash + 1 - buf), "%s", name );
    }
    return 1;
}

/* the Unix path of the Windows program this process runs */
int linuxhost_program_path( char *buf, size_t size )
{
    WCHAR exe[MAX_PATH];
    char *unix_name;
    typedef char *(CDECL *get_unix_fn)( const WCHAR * );
    get_unix_fn get_unix = (get_unix_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_unix_file_name" );

    GetModuleFileNameW( NULL, exe, MAX_PATH );
    if (!get_unix || !(unix_name = get_unix( exe ))) return 0;
    snprintf( buf, size, "%s", unix_name );
    HeapFree( GetProcessHeap(), 0, unix_name );
    return 1;
}

/* /sys/class/dmi/id: the firmware as a Mac's would describe itself */
static int make_dmi_file( struct fd_entry *f, const char *name )
{
    static const struct { const char *name, *value; } dmi[] =
    {
        { "board_name", "Mac\n" }, { "product_name", "Mac\n" }, { "product_family", "Mac\n" },
        { "sys_vendor", "Apple Inc.\n" }, { "board_vendor", "Apple Inc.\n" }, { "chassis_vendor", "Apple Inc.\n" },
        { "bios_vendor", "Apple Inc.\n" }, { "bios_version", "1.0\n" }, { "bios_date", "01/01/2025\n" },
        { "product_version", "1.0\n" }, { "board_version", "1.0\n" }, { "chassis_type", "10\n" },
        { "chassis_version", "1.0\n" },
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(dmi); i++)
        if (!strcmp( name, dmi[i].name )) return append_text( f, dmi[i].value, strlen( dmi[i].value ) );
    return 0;
}

/* the processor's name, as cpuid gives it to x86 code (FEX's, here) */
static void cpu_brand( char *brand )
{
    int regs[12];

    __cpuid( regs, 0x80000002 );
    __cpuid( regs + 4, 0x80000003 );
    __cpuid( regs + 8, 0x80000004 );
    memcpy( brand, regs, 48 );
    brand[48] = 0;
    while (*brand == ' ') memmove( brand, brand + 1, strlen( brand ) );
}

/* files about the machine: what it is, a Mac with its own processors and memory, said the way Linux says it */
static int make_machine_file( struct fd_entry *f, const char *name )
{
    char text[4096];
    int len = 0;

    if (!strcmp( name, "mounts" ))
    {
        static const char mounts[] = "/dev/disk3s1 / apfs rw,relatime 0 0\nproc /proc proc rw,nosuid,nodev,noexec 0 0\n"
                                     "sysfs /sys sysfs rw,nosuid,nodev,noexec 0 0\ntmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n";
        return append_text( f, mounts, sizeof(mounts) - 1 );
    }
    if (!strcmp( name, "pci" ) || !strcmp( name, "empty" )) return 1;
    if (!strcmp( name, "zero" )) return append_text( f, "0\n", 2 );
    if (!strcmp( name, "one" )) return append_text( f, "1\n", 2 );
    if (!strcmp( name, "ptrace_scope" )) return append_text( f, "1\n", 2 );
    if (!strcmp( name, "pid_max" )) return append_text( f, "4194304\n", 8 );
    if (!strcmp( name, "file_max" )) return append_text( f, "9223372036854775807\n", 20 );
    if (!strcmp( name, "ostype" )) return append_text( f, "Linux\n", 6 );
    if (!strcmp( name, "osrelease" )) return append_text( f, "6.5.0\n", 6 );
    if (!strcmp( name, "version" ))
        len = snprintf( text, sizeof(text), "Linux version 6.5.0 (builder@localhost) (gcc (GCC) 13.2.0, GNU ld 2.41) "
                        "#1 SMP PREEMPT_DYNAMIC\n" );
    else if (!strcmp( name, "cmdline" )) len = snprintf( text, sizeof(text), "BOOT_IMAGE=/boot/vmlinuz-6.5.0 ro quiet splash\n" );
    else if (!strcmp( name, "filesystems" ))
        len = snprintf( text, sizeof(text), "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\nnodev\tdevtmpfs\n\text4\n\tapfs\n" );
    else if (!strcmp( name, "hostname" ))
    {
        DWORD size = sizeof(text) - 2;

        if (!GetComputerNameA( text, &size )) strcpy( text, "localhost" );
        strcat( text, "\n" );
        len = strlen( text );
    }
    else if (!strcmp( name, "boot_id" ) || !strcmp( name, "uuid" ))
    {
        static unsigned char boot[16];
        unsigned char id[16];
        unsigned int i;

        /* boot_id stays the same for the process; uuid is new each time */
        if (!boot[0]) for (i = 0; i < 16; i++) boot[i] = (unsigned char)(GetTickCount64() * 2654435761u >> (i % 4 * 8)) ^ (i * 37) ^ GetCurrentProcessId();
        for (i = 0; i < 16; i++) id[i] = name[0] == 'b' ? boot[i] : (unsigned char)(rand() ^ (GetTickCount64() >> i));
        id[6] = (id[6] & 0x0f) | 0x40;
        id[8] = (id[8] & 0x3f) | 0x80;
        len = snprintf( text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12],
                        id[13], id[14], id[15] );
    }
    else if (!strcmp( name, "uptime" ))
    {
        unsigned long long ms = GetTickCount64();

        len = snprintf( text, sizeof(text), "%llu.%02llu %llu.%02llu\n", ms / 1000, ms / 10 % 100, ms / 1000 * 4,
                        ms / 10 % 100 );
    }
    else if (!strcmp( name, "loadavg" ))
        len = snprintf( text, sizeof(text), "0.50 0.40 0.30 1/600 %lu\n", GetCurrentProcessId() );
    else if (!strcmp( name, "meminfo" ))
    {
        MEMORYSTATUSEX mem = { sizeof(mem) };

        GlobalMemoryStatusEx( &mem );
        len = snprintf( text, sizeof(text), "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n"
                        "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
                        "SwapTotal:             0 kB\nSwapFree:              0 kB\n",
                        mem.ullTotalPhys / 1024, mem.ullAvailPhys / 1024, mem.ullAvailPhys / 1024 );
    }
    else if (!strcmp( name, "stat" ) || !strcmp( name, "cpuinfo" ))
    {
        SYSTEM_INFO info;
        unsigned int i;
        char brand[49];

        GetSystemInfo( &info );
        if (!strcmp( name, "stat" ))
        {
            FILETIME now;
            unsigned long long t;

            GetSystemTimeAsFileTime( &now );
            t = (((unsigned long long)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ull) / 10000000;
            len = snprintf( text, sizeof(text), "cpu  1000 0 1000 100000 0 0 0 0 0 0\n" );
            for (i = 0; i < info.dwNumberOfProcessors && len < (int)sizeof(text) - 200; i++)
                len += snprintf( text + len, sizeof(text) - len, "cpu%u 100 0 100 10000 0 0 0 0 0 0\n", i );
            len += snprintf( text + len, sizeof(text) - len, "ctxt 100000\nbtime %llu\nprocesses 1000\n"
                             "procs_running 1\nprocs_blocked 0\n", t - GetTickCount64() / 1000 );
            return append_text( f, text, len );
        }
        cpu_brand( brand );
        for (i = 0; i < info.dwNumberOfProcessors; i++)
        {
            len = snprintf( text, sizeof(text), "processor\t: %u\nvendor_id\t: GenuineIntel\ncpu family\t: 6\n"
                            "model\t\t: 85\nmodel name\t: %s\nstepping\t: 4\ncpu MHz\t\t: 3000.000\n"
                            "cache size\t: 4096 KB\nphysical id\t: 0\nsiblings\t: %lu\ncore id\t\t: %u\n"
                            "cpu cores\t: %lu\napicid\t\t: %u\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 13\n"
                            "wp\t\t: yes\nflags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat "
                            "pse36 clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc rep_good nopl pni pclmulqdq "
                            "ssse3 cx16 sse4_1 sse4_2 popcnt aes xsave avx hypervisor lahf_lm\nbogomips\t: 6000.00\n"
                            "clflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 48 bits physical, 48 bits virtual\n"
                            "power management:\n\n", i, brand, info.dwNumberOfProcessors, i, info.dwNumberOfProcessors, i );
            if (!append_text( f, text, len )) return 0;
        }
        return 1;
    }
    else return 0;
    return append_text( f, text, len );
}

/* /proc/<pid> of a child of fork: a Wine process this process started and traces */
static int make_child_file( struct fd_entry *f, const char *entry )
{
    char text[1024], state;
    int pid, traced, len;
    unsigned long windows_pid;
    const char *name;

    if (sscanf( entry, "%d-%n", &pid, &len ) != 1 || !linuxhost_child_info( pid, &state, &traced, &windows_pid ))
        return 0;
    name = entry + len;
    if (!strcmp( name, "comm" )) return append_text( f, "wine64-preload\n", 15 );
    if (!strcmp( name, "status" ))
    {
        len = snprintf( text, sizeof(text), "Name:\twine64-preload\nUmask:\t0022\nState:\t%c (%s)\nTgid:\t%d\nNgid:\t0\n"
                        "Pid:\t%d\nPPid:\t%lu\nTracerPid:\t%lu\nUid:\t1000\t1000\t1000\t1000\n"
                        "Gid:\t1000\t1000\t1000\t1000\nFDSize:\t64\nGroups:\t\nThreads:\t1\nSeccomp:\t0\n", state,
                        state == 't' ? "tracing stop" : state == 'T' ? "stopped" : state == 'Z' ? "zombie" : "sleeping",
                        pid, pid, GetCurrentProcessId(), traced ? GetCurrentProcessId() : 0 );
        return append_text( f, text, len );
    }
    if (!strcmp( name, "stat" ))
    {
        len = snprintf( text, sizeof(text), "%d (wine64-preload) %c %lu %d %d 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 "
                        "200 0 0 18446744073709551615 0 0 0 0 0 0 0 4096 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                        pid, state, GetCurrentProcessId(), pid, pid );
        return append_text( f, text, len );
    }
    return 0;
}

/* the names in a made-up directory */
int linuxhost_proc_dir( const char *path, char *names, size_t size )
{
    char buf[4096];
    size_t len = 0, n;
    int count = 0, pid, i;
    const char *rest = NULL;
    char prefix[32];

#define ADD(...) do { n = snprintf( buf + len, sizeof(buf) - len, __VA_ARGS__ ); \
                      if (len + n + 1 < sizeof(buf)) { len += n + 1; count++; } } while (0)
    snprintf( prefix, sizeof(prefix), "/proc/%lu", GetCurrentProcessId() );
    if (!strcmp( path, "/proc" ) || !strcmp( path, "/proc/" ))
    {
        char state;
        int traced;
        unsigned long windows_pid;

        ADD( "1" );
        ADD( "%lu", GetCurrentProcessId() );
        for (pid = 0x7000; pid < 0x7010; pid++)
            if (linuxhost_child_info( pid, &state, &traced, &windows_pid )) ADD( "%d", pid );
        ADD( "self" );
        ADD( "thread-self" );
        ADD( "cpuinfo" ); ADD( "meminfo" ); ADD( "mounts" ); ADD( "stat" ); ADD( "uptime" ); ADD( "version" );
        ADD( "loadavg" ); ADD( "filesystems" ); ADD( "modules" ); ADD( "cmdline" ); ADD( "sys" ); ADD( "bus" );
    }
    else if (!strcmp( path, "/proc/self" ) || !strcmp( path, prefix )) rest = "";
    else if (!strncmp( path, "/proc/self/", 11 )) rest = path + 11;
    else if (!strncmp( path, prefix, strlen( prefix ) ) && path[strlen( prefix )] == '/') rest = path + strlen( prefix ) + 1;
    else if (!strcmp( path, "/sys/devices/virtual/dmi/id" ) || !strcmp( path, "/sys/class/dmi/id" ))
    {
        ADD( "board_name" ); ADD( "board_vendor" ); ADD( "board_version" ); ADD( "product_name" );
        ADD( "product_family" ); ADD( "product_version" ); ADD( "sys_vendor" ); ADD( "chassis_vendor" );
        ADD( "chassis_type" ); ADD( "chassis_version" ); ADD( "bios_vendor" ); ADD( "bios_version" ); ADD( "bios_date" );
    }
    else return -1;
    if (rest)
    {
        if (!*rest)
        {
            static const char *const entries[] = { "fd", "task", "maps", "status", "stat", "statm", "comm", "cmdline",
                                                   "environ", "exe", "cwd", "root", "mounts", "mountinfo", "cgroup",
                                                   "limits", "auxv", "loginuid", "sessionid", "wchan", "attr" };

            for (i = 0; i < (int)ARRAY_SIZE(entries); i++) ADD( "%s", entries[i] );
        }
        else if (!strcmp( rest, "fd" ))
        {
            ADD( "0" ); ADD( "1" ); ADD( "2" );
            for (i = FIRST_FD; i < MAX_FDS; i++) if (fds[i].kind != FD_FREE) ADD( "%d", i );
        }
        else if (!strcmp( rest, "task" )) ADD( "%lu", GetCurrentThreadId() );
        else return -1;
    }
#undef ADD
    memcpy( names, buf, min( len, size ) );
    return count;
}

/* the /proc files of this process the host makes up; 0 if it does not provide this one */
static int make_proc_file( struct fd_entry *f, const char *entry )
{
    char text[4096], exe[1024];
    int len;

    if (!strcmp( entry, "maps" )) return make_maps( f );
    if (!strcmp( entry, "init-comm" )) return append_text( f, "systemd\n", 8 );
    if (!strcmp( entry, "init-cmdline" )) return append_text( f, "/sbin/init\0splash\0", 18 );
    if (!strcmp( entry, "init-status" ))
    {
        len = snprintf( text, sizeof(text), "Name:\tsystemd\nUmask:\t0000\nState:\tS (sleeping)\nTgid:\t1\nNgid:\t0\nPid:\t1\n"
                        "PPid:\t0\nTracerPid:\t0\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t256\nGroups:\t\n"
                        "Threads:\t1\nSeccomp:\t0\n" );
        return append_text( f, text, len );
    }
    if (!strcmp( entry, "init-stat" ))
    {
        len = snprintf( text, sizeof(text), "1 (systemd) S 0 1 1 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 1 0 0 "
                        "18446744073709551615 0 0 0 0 0 0 671173123 4096 1260 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n" );
        return append_text( f, text, len );
    }
    if (!strncmp( entry, "child-", 6 )) return make_child_file( f, entry + 6 );
    if (!strncmp( entry, "dmi-", 4 )) return make_dmi_file( f, entry + 4 );
    if (!strncmp( entry, "sys-", 4 )) return make_machine_file( f, entry + 4 );
    if (!strcmp( entry, "environ" ))
    {
        /* the Windows environment, which under Wine holds the Unix one */
        WCHAR *env = GetEnvironmentStringsW(), *p;

        for (p = env; p && *p; p += wcslen( p ) + 1)
        {
            if (*p == '=') continue;  /* a drive's current directory */
            len = WideCharToMultiByte( CP_UTF8, 0, p, -1, text, sizeof(text), NULL, NULL );
            if (len > 0 && !append_text( f, text, len )) break;
        }
        FreeEnvironmentStringsW( env );
        return 1;
    }
    if (!strcmp( entry, "cgroup" )) return append_text( f, "0::/user.slice/user-1000.slice/session-2.scope\n", 48 );
    if (!strcmp( entry, "mounts" )) return make_machine_file( f, "mounts" );
    if (!strcmp( entry, "mountinfo" ))
    {
        static const char mountinfo[] = "22 1 1:4 / / rw,relatime shared:1 - apfs /dev/disk3s1 rw\n"
                                        "23 22 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12 - proc proc rw\n"
                                        "24 22 0:22 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
                                        "25 22 0:23 / /tmp rw,nosuid,nodev shared:14 - tmpfs tmpfs rw\n";
        return append_text( f, mountinfo, sizeof(mountinfo) - 1 );
    }
    if (!strcmp( entry, "limits" ))
    {
        static const char limits[] =
            "Limit                     Soft Limit           Hard Limit           Units     \n"
            "Max cpu time              unlimited            unlimited            seconds   \n"
            "Max file size             unlimited            unlimited            bytes     \n"
            "Max data size             unlimited            unlimited            bytes     \n"
            "Max stack size            8388608              unlimited            bytes     \n"
            "Max core file size        0                    unlimited            bytes     \n"
            "Max resident set          unlimited            unlimited            bytes     \n"
            "Max processes             63432                63432                processes \n"
            "Max open files            1024                 524288               files     \n"
            "Max locked memory         8388608              8388608              bytes     \n"
            "Max address space         unlimited            unlimited            bytes     \n"
            "Max file locks            unlimited            unlimited            locks     \n"
            "Max pending signals       63432                63432                signals   \n"
            "Max msgqueue size         819200               819200               bytes     \n"
            "Max nice priority         0                    0                    \n"
            "Max realtime priority     0                    0                    \n"
            "Max realtime timeout      unlimited            unlimited            us        \n";
        return append_text( f, limits, sizeof(limits) - 1 );
    }
    if (!strcmp( entry, "statm" )) return append_text( f, "65536 16384 4096 512 0 32768 0\n", 31 );
    if (!strcmp( entry, "loginuid" )) return append_text( f, "1000", 4 );
    if (!strcmp( entry, "sessionid" )) return append_text( f, "2", 1 );
    if (!strcmp( entry, "oom_score_adj" ) || !strcmp( entry, "oom_score" )) return append_text( f, "0\n", 2 );
    if (!strcmp( entry, "wchan" )) return append_text( f, "0", 1 );
    if (!strcmp( entry, "personality" )) return append_text( f, "00000000\n", 9 );
    if (!strcmp( entry, "attr/current" )) return append_text( f, "unconfined\n", 11 );
    if (!strcmp( entry, "auxv" )) return append_text( f, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16 );  /* AT_NULL */
    if (!strcmp( entry, "comm" )) return append_text( f, "start_protected\n", 16 );
    if (!strcmp( entry, "status" ))
    {
        const char *name;

        if (!linuxhost_program_path( exe, sizeof(exe) )) strcpy( exe, "start_protected_game.exe" );
        name = strrchr( exe, '/' ) ? strrchr( exe, '/' ) + 1 : exe;
        len = snprintf( text, sizeof(text), "Name:\t%.15s\nUmask:\t0022\nState:\tR (running)\nTgid:\t%lu\nNgid:\t0\n"
                        "Pid:\t%lu\nPPid:\t1\nTracerPid:\t0\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\n"
                        "FDSize:\t256\nGroups:\t\nThreads:\t1\nSigQ:\t0/63432\nSigPnd:\t0000000000000000\n"
                        "ShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\nSigIgn:\t0000000000001000\n"
                        "SigCgt:\t0000000000000000\nSeccomp:\t0\nvoluntary_ctxt_switches:\t0\n"
                        "nonvoluntary_ctxt_switches:\t0\n", name, GetCurrentProcessId(), GetCurrentProcessId() );
        return append_text( f, text, len );
    }
    if (!strcmp( entry, "cmdline" ))
    {
        /* Wine on Linux shows a process's Windows command line, one argument per string */
        int argc, i;
        WCHAR **argv = CommandLineToArgvW( GetCommandLineW(), &argc );

        if (!argv) return append_text( f, "start_protected_game.exe", 25 );
        for (i = 0; i < argc; i++)
        {
            len = WideCharToMultiByte( CP_UTF8, 0, argv[i], -1, text, sizeof(text), NULL, NULL );
            if (len > 0 && !append_text( f, text, len )) break;
        }
        LocalFree( argv );
        return 1;
    }
    if (!strcmp( entry, "stat" ))
    {
        len = snprintf( text, sizeof(text), "%lu (start_protected) R 1 %lu %lu 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 "
                        "100 0 0 18446744073709551615 0 0 0 0 0 0 0 4096 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                        GetCurrentProcessId(), GetCurrentProcessId(), GetCurrentProcessId() );
        return append_text( f, text, len );
    }
    return 0;
}

/* a Unix path as Wine takes it: \\?\unix\<path> */
WCHAR *linuxhost_unix_path( const char *path )
{
    static const WCHAR prefix[] = L"\\\\?\\unix";
    int len = MultiByteToWideChar( CP_UTF8, 0, path, -1, NULL, 0 );
    WCHAR *ret = malloc( (len + ARRAY_SIZE(prefix)) * sizeof(WCHAR) ), *p;

    if (!ret) return NULL;
    wcscpy( ret, prefix );
    MultiByteToWideChar( CP_UTF8, 0, path, -1, ret + wcslen( prefix ), len );
    for (p = ret + wcslen( prefix ); *p; p++) if (*p == '/') *p = '\\';
    return ret;
}

int linuxhost_is_proc_path( const char *path )
{
    return !strncmp( path, "/proc/", 6 ) || !strncmp( path, "/sys/", 5 );
}

/* ---------------------------------------------------------------------------------------------------------
 * System calls
 */

long long linuxhost_open( const char *path, int flags )
{
    const char *entry;
    DWORD access, disposition, attrs = FILE_ATTRIBUTE_NORMAL;
    HANDLE handle;
    WCHAR *wpath;
    int fd;

    if (!path) return -LX_EINVAL;
    if (!strcmp( path, "/dev/urandom" ) || !strcmp( path, "/dev/random" ) || !strcmp( path, "/dev/null" ) ||
        !strcmp( path, "/dev/zero" ))
    {
        if ((fd = alloc_fd( path )) < 0) return fd;
        fds[fd].kind = FD_DEVICE;
        fds[fd].device = path[5] == 'n' ? DEV_NULL : path[5] == 'z' ? DEV_ZERO : DEV_RANDOM;
        fds[fd].flags = flags;
        linuxhost_trace( "open %s: %d\n", path, fd );
        return fd;
    }
    if (linuxhost_is_proc_path( path ))
    {
        if (!(entry = proc_self( path )))
        {
            linuxhost_trace( "open %s: not provided\n", path );
            return -LX_ENOENT;
        }
        if ((fd = alloc_fd( path )) < 0) return fd;
        if (!make_proc_file( &fds[fd], entry ))
        {
            linuxhost_close( fd );
            linuxhost_trace( "open %s: not provided\n", path );
            return -LX_ENOENT;
        }
        linuxhost_trace( "open %s: %d (made up, %zu bytes)\n", path, fd, fds[fd].size );
        return fd;
    }

    /* a real file */
    switch (flags & 3)
    {
    case 0: access = GENERIC_READ; break;
    case 1: access = GENERIC_WRITE; break;
    default: access = GENERIC_READ | GENERIC_WRITE; break;
    }
    if ((flags & 0xc0) == 0xc0) disposition = CREATE_NEW;           /* O_CREAT | O_EXCL */
    else if ((flags & 0x240) == 0x240) disposition = CREATE_ALWAYS;  /* O_CREAT | O_TRUNC */
    else if (flags & 0x40) disposition = OPEN_ALWAYS;                /* O_CREAT */
    else if (flags & 0x200) disposition = TRUNCATE_EXISTING;         /* O_TRUNC */
    else disposition = OPEN_EXISTING;
    if (flags & 0x10000) attrs |= FILE_FLAG_BACKUP_SEMANTICS;        /* O_DIRECTORY */
    if (!(wpath = linuxhost_unix_path( path ))) return -LX_ENOMEM;
    handle = CreateFileW( wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disposition,
                          attrs | FILE_FLAG_BACKUP_SEMANTICS, NULL );
    free( wpath );
    if (handle == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();

        linuxhost_trace( "open %s %#x: error %lu\n", path, flags, err );
        return err == ERROR_ACCESS_DENIED ? -LX_EACCES : err == ERROR_FILE_EXISTS ? -17 : -LX_ENOENT;
    }
    if ((fd = alloc_fd( path )) < 0)
    {
        CloseHandle( handle );
        return fd;
    }
    fds[fd].kind = FD_HANDLE;
    fds[fd].handle = handle;
    fds[fd].flags = flags;
    if (flags & 0x400) SetFilePointer( handle, 0, NULL, FILE_END );  /* O_APPEND */
    linuxhost_trace( "open %s %#x: %d\n", path, flags, fd );
    return fd;
}

long long linuxhost_memfd_create( const char *name, unsigned int flags )
{
    return alloc_fd( name ? name : "memfd" );
}

long long linuxhost_read( int fd, void *buf, size_t size )
{
    struct fd_entry *f = get_fd( fd );
    size_t n;

    if (!f) return -LX_EBADF;
    if (f->kind == FD_SOCKET) return linuxhost_socket_io( (UINT_PTR)f->handle, buf, size, 0 );
    if (f->kind == FD_DEVICE)
    {
        if (f->device == DEV_NULL) return 0;
        if (f->device == DEV_ZERO) memset( buf, 0, size );
        else if (BCryptGenRandom( NULL, buf, (ULONG)min( size, 0x7fffffff ), BCRYPT_USE_SYSTEM_PREFERRED_RNG )) return -5;
        return min( size, 0x7fffffff );
    }
    if (f->kind == FD_HANDLE)
    {
        DWORD got;

        if (!ReadFile( f->handle, buf, size, &got, NULL )) return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -5;
        return got;
    }
    n = f->pos < f->size ? min( size, f->size - f->pos ) : 0;
    memcpy( buf, f->data + f->pos, n );
    f->pos += n;
    return n;
}

long long linuxhost_seals( int fd, int add, unsigned int seals )
{
    struct fd_entry *f = get_fd( fd );

    if (!f) return -LX_EBADF;
    if (f->kind != FD_MEMORY) return -LX_EINVAL;
    if (!add) return f->seals;
    if (f->seals & 1) return -1;  /* EPERM: sealed against more seals */
    f->seals |= seals;
    return 0;
}

long long linuxhost_write( int fd, const void *buf, size_t size )
{
    struct fd_entry *f = get_fd( fd );

    if (!f) return -LX_EBADF;
    if (f->kind == FD_SOCKET) return linuxhost_socket_io( (UINT_PTR)f->handle, (void *)buf, size, 1 );
    if (f->kind == FD_DEVICE) return size;
    if (f->kind == FD_MEMORY && (f->seals & 8)) return -1;  /* EPERM: F_SEAL_WRITE */
    if (f->kind == FD_MEMORY && (f->seals & 4) && f->pos + size > f->size) return -1;  /* F_SEAL_GROW */
    if (f->kind == FD_HANDLE)
    {
        DWORD written;

        if (f->flags & 0x400) SetFilePointer( f->handle, 0, NULL, FILE_END );
        if (!WriteFile( f->handle, buf, size, &written, NULL )) return -5;
        return written;
    }
    if (!reserve( f, f->pos + size )) return -LX_ENOMEM;
    memcpy( f->data + f->pos, buf, size );
    f->pos += size;
    if (f->pos > f->size) f->size = f->pos;
    return size;
}

long long linuxhost_lseek( int fd, long long offset, int whence )
{
    struct fd_entry *f = get_fd( fd );
    long long pos;

    if (!f) return -LX_EBADF;
    if (f->kind == FD_HANDLE)
    {
        LARGE_INTEGER dist, pos;

        dist.QuadPart = offset;
        if (whence > 2 || !SetFilePointerEx( f->handle, dist, &pos, whence )) return -LX_EINVAL;
        return pos.QuadPart;
    }
    switch (whence)
    {
    case 0: pos = offset; break;
    case 1: pos = f->pos + offset; break;
    case 2: pos = f->size + offset; break;
    default: return -LX_EINVAL;
    }
    if (pos < 0) return -LX_EINVAL;
    f->pos = pos;
    return pos;
}

long long linuxhost_close( int fd )
{
    struct fd_entry *f = get_fd( fd );

    if (!f) return -LX_EBADF;
    EnterCriticalSection( &fd_lock );
    if (f->kind == FD_HANDLE) CloseHandle( f->handle );
    if (f->kind == FD_SOCKET) linuxhost_socket_close( (UINT_PTR)f->handle );
    if (f->kind != FD_EPOLL) free( f->data );
    else linuxhost_epoll_free( f->data );
    memset( f, 0, sizeof(*f) );
    LeaveCriticalSection( &fd_lock );
    return 0;
}

/* Linux's struct stat on x86_64 */
struct lx_stat
{
    unsigned long long dev, ino, nlink;
    unsigned int mode, uid, gid, pad;
    unsigned long long rdev;
    long long size, blksize, blocks;
    unsigned long long atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    long long unused[3];
};

static unsigned long long unix_seconds( FILETIME ft, unsigned long long *nsec )
{
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;

    *nsec = (t % 10000000) * 100;
    return t / 10000000;
}

void linuxhost_stat_from_info( const BY_HANDLE_FILE_INFORMATION *info, void *buf )
{
    struct lx_stat *st = buf;

    memset( st, 0, sizeof(*st) );
    st->dev = info->dwVolumeSerialNumber;
    st->ino = (unsigned long long)info->nFileIndexHigh << 32 | info->nFileIndexLow;
    st->nlink = info->nNumberOfLinks;
    st->mode = (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040755 :
               (info->dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0100555 : 0100755;
    st->uid = st->gid = 1000;
    st->size = (long long)info->nFileSizeHigh << 32 | info->nFileSizeLow;
    st->blksize = 4096;
    st->blocks = (st->size + 511) / 512;
    st->atime = unix_seconds( info->ftLastAccessTime, &st->atime_ns );
    st->mtime = unix_seconds( info->ftLastWriteTime, &st->mtime_ns );
    st->ctime = unix_seconds( info->ftLastWriteTime, &st->ctime_ns );
}

long long linuxhost_fstat( int fd, void *buf )
{
    struct fd_entry *f = get_fd( fd );
    struct lx_stat *st = buf;

    if (!f) return -LX_EBADF;
    if (f->kind == FD_HANDLE)
    {
        BY_HANDLE_FILE_INFORMATION info;

        if (!GetFileInformationByHandle( f->handle, &info )) return -5;
        linuxhost_stat_from_info( &info, st );
        return 0;
    }
    memset( st, 0, sizeof(*st) );
    st->ino = fd;
    st->nlink = 1;
    st->mode = f->kind == FD_DEVICE ? 0020666 : f->kind == FD_SOCKET ? 0140777 : 0100600;
    if (f->kind == FD_DEVICE) st->rdev = f->device == DEV_NULL ? 0x103 : f->device == DEV_ZERO ? 0x105 : 0x109;
    st->size = f->size;
    st->blksize = 4096;
    st->blocks = (f->size + 511) / 512;
    return 0;
}

long long linuxhost_unlink( const char *path )
{
    linuxhost_trace( "unlink %s\n", path ? path : "(null)" );
    return -LX_ENOENT;
}

/* ---------------------------------------------------------------------------------------------------------
 * Threads: Linux code reaches its thread control block through %fs. Windows x64 code does not use the segment, so
 * every thread gets one, and FEX's wrfsbase points %fs at it. Only the fields compiled code reads are set: the
 * pointers to itself (fs:0 and fs:0x10), the stack protector's canary (fs:0x28) and the pointer guard (fs:0x30).
 */

static unsigned long long stack_guard, pointer_guard;

void linuxhost_thread_init(void)
{
    /* glibc's tcbhead_t, with room below it for initial-exec TLS */
    enum { STATIC_TLS = 0x1000, TCB_SIZE = 0x700 };
    unsigned long long *tcb, current;
    char *block;

    __asm__ volatile( "rdfsbase %0" : "=r"(current) );
    if (current) return;
    if (!(block = VirtualAlloc( NULL, STATIC_TLS + TCB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE ))) return;
    tcb = (unsigned long long *)(block + STATIC_TLS);
    tcb[0] = (unsigned long long)tcb;   /* tcb */
    tcb[2] = (unsigned long long)tcb;   /* self */
    tcb[5] = stack_guard;               /* stack_guard, fs:0x28 */
    tcb[6] = pointer_guard;             /* pointer_guard, fs:0x30 */
    __asm__ volatile( "wrfsbase %0" :: "r"(tcb) );
}

/* linux_entry_common: reached from an entry stub with the Linux function in r11; sets up %fs if the thread has no
 * thread control block yet, keeping every register a System V call takes arguments in, and jumps to the function. */
__asm__( ".text\n"
         ".globl linux_entry_common\n"
         "linux_entry_common:\n"
         "    pushq %rax\n"
         "    rdfsbase %rax\n"
         "    testq %rax, %rax\n"
         "    popq %rax\n"
         "    jnz 1f\n"
         "    pushq %rbp\n"
         "    movq %rsp, %rbp\n"
         "    andq $-16, %rsp\n"
         "    subq $0xe0, %rsp\n"
         "    movq %rdi, 0x20(%rsp)\n"
         "    movq %rsi, 0x28(%rsp)\n"
         "    movq %rdx, 0x30(%rsp)\n"
         "    movq %rcx, 0x38(%rsp)\n"
         "    movq %r8, 0x40(%rsp)\n"
         "    movq %r9, 0x48(%rsp)\n"
         "    movq %rax, 0x50(%rsp)\n"
         "    movq %r11, 0x58(%rsp)\n"
         "    movdqu %xmm0, 0x60(%rsp)\n"
         "    movdqu %xmm1, 0x70(%rsp)\n"
         "    movdqu %xmm2, 0x80(%rsp)\n"
         "    movdqu %xmm3, 0x90(%rsp)\n"
         "    movdqu %xmm4, 0xa0(%rsp)\n"
         "    movdqu %xmm5, 0xb0(%rsp)\n"
         "    movdqu %xmm6, 0xc0(%rsp)\n"
         "    movdqu %xmm7, 0xd0(%rsp)\n"
         "    call linuxhost_thread_init\n"
         "    movq 0x20(%rsp), %rdi\n"
         "    movq 0x28(%rsp), %rsi\n"
         "    movq 0x30(%rsp), %rdx\n"
         "    movq 0x38(%rsp), %rcx\n"
         "    movq 0x40(%rsp), %r8\n"
         "    movq 0x48(%rsp), %r9\n"
         "    movq 0x50(%rsp), %rax\n"
         "    movq 0x58(%rsp), %r11\n"
         "    movdqu 0x60(%rsp), %xmm0\n"
         "    movdqu 0x70(%rsp), %xmm1\n"
         "    movdqu 0x80(%rsp), %xmm2\n"
         "    movdqu 0x90(%rsp), %xmm3\n"
         "    movdqu 0xa0(%rsp), %xmm4\n"
         "    movdqu 0xb0(%rsp), %xmm5\n"
         "    movdqu 0xc0(%rsp), %xmm6\n"
         "    movdqu 0xd0(%rsp), %xmm7\n"
         "    movq %rbp, %rsp\n"
         "    popq %rbp\n"
         "1:  jmp *%r11\n" );

extern void linux_entry_common(void);

/* Tracing the calls a Windows program makes into Linux code: the export's name, its first four arguments (as text
 * when they point at some) and its result. A wrapper per export, made by linuxhost_entry_stub when tracing. */
struct traced_export { void *function; char name[32]; };
typedef SYSV long long (*linux_fn)( long long, long long, long long, long long, long long, long long );

static void describe_arg( char *out, size_t size, long long arg )
{
    MEMORY_BASIC_INFORMATION info;
    const unsigned char *p = (const unsigned char *)arg;
    int i;

    snprintf( out, size, "%#llx", arg );
    if (arg < 0x10000 || VirtualQuery( p, &info, sizeof(info) ) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return;
    for (i = 0; i < 4 && p[i] >= 0x20 && p[i] < 0x7f; i++) ;
    if (i == 4) snprintf( out, size, "%#llx \"%.60s\"", arg, (const char *)p );
}

/* the callback a request to the module carries (EAC's launch request: the result comes back through it) */
static linux_fn request_callback;

SYSV long long traced_callback( long long a1, long long a2, long long a3, long long a4, long long a5, long long a6 )
{
    char d1[96], d2[96], d3[96], d4[96];
    long long ret;

    describe_arg( d1, sizeof(d1), a1 ); describe_arg( d2, sizeof(d2), a2 );
    describe_arg( d3, sizeof(d3), a3 ); describe_arg( d4, sizeof(d4), a4 );
    linuxhost_trace( "callback( %s, %s, %s, %s )\n", d1, d2, d3, d4 );
    if (a1 > 0x10000)
    {
        const long long *p = (const long long *)a1;
        int i;

        for (i = 0; i < 8; i++)
        {
            describe_arg( d1, sizeof(d1), p[i] );
            linuxhost_trace( "  callback arg +%#x: %s\n", i * 8, d1 );
        }
    }
    ret = request_callback( a1, a2, a3, a4, a5, a6 );
    linuxhost_trace( "callback returned %#llx\n", ret );
    return ret;
}

static void dump_request( const char *name, long long arg )
{
    long long *p = (long long *)arg;
    char d[96];
    int i;

    if (arg < 0x10000) return;
    for (i = 0; i < 12; i++)
    {
        describe_arg( d, sizeof(d), p[i] );
        linuxhost_trace( "  %s request +%#x: %s\n", name, i * 8, d );
    }
    /* EAC's launch request (a): a version, then pointers, the callback at +0x30 */
    if (!strcmp( name, "a" ) && p[6] && (void *)p[6] != (void *)traced_callback)
    {
        request_callback = (linux_fn)p[6];
        p[6] = (long long)traced_callback;
    }
}

SYSV long long traced_call( struct traced_export *e, long long a1, long long a2, long long a3, long long a4,
                                   long long a5 )
{
    char d1[96], d2[96], d3[96], d4[96];
    long long ret;

    dump_request( e->name, a1 );
    describe_arg( d1, sizeof(d1), a1 ); describe_arg( d2, sizeof(d2), a2 );
    describe_arg( d3, sizeof(d3), a3 ); describe_arg( d4, sizeof(d4), a4 );
    linuxhost_trace( "-> %s( %s, %s, %s, %s )\n", e->name, d1, d2, d3, d4 );
    ret = ((linux_fn)e->function)( a1, a2, a3, a4, a5, 0 );
    linuxhost_trace( "<- %s = %#llx (%lld)\n", e->name, ret, ret );
    return ret;
}

/* rdi..r8 move up one place for the export's description, which goes first */
__asm__( ".text\n"
         "traced_entry:\n"
         "    movq %r8, %r9\n"
         "    movq %rcx, %r8\n"
         "    movq %rdx, %rcx\n"
         "    movq %rsi, %rdx\n"
         "    movq %rdi, %rsi\n"
         "    movq %r10, %rdi\n"
         "    jmp traced_call\n" );
extern void traced_entry(void);

/* An entry stub for a Linux function a Windows program calls directly (from dlsym): movabs $function, %r11;
 * movabs $linux_entry_common, %rax; jmp *%rax would lose rax (the vector count of a variadic call), so the
 * common code's address is read from beside the stub instead. */
void *linuxhost_entry_stub( void *function, const char *name )
{
    static uint8_t *page;
    static size_t used;
    static int tracing = -1;
    uint8_t *code;
    DWORD old;

    if (!function) return NULL;
    if (tracing == -1) tracing = GetEnvironmentVariableA( "LINUXHOST_TRACE", NULL, 0 ) > 0;
    if (tracing)
    {
        /* the stub sets up %fs and goes to traced_entry, with the export's description in r10 */
        struct traced_export *e = calloc( 1, sizeof(*e) );
        static uint8_t *tpage;
        static size_t tused;
        uint8_t *t;
        void *target = traced_entry;

        if (!e) return function;
        e->function = function;
        snprintf( e->name, sizeof(e->name), "%s", name );
        if (!tpage || tused + 48 > 0x1000)
        {
            if (!(tpage = VirtualAlloc( NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE ))) return function;
            tused = 0;
        }
        t = tpage + tused;
        tused += 48;
        t[0] = 0x49; t[1] = 0xba; memcpy( t + 2, &e, 8 );           /* movabs $e, %r10 */
        t[10] = 0xff; t[11] = 0x25; memset( t + 12, 0, 4 );         /* jmp *0(%rip) */
        memcpy( t + 16, &target, 8 );
        FlushInstructionCache( GetCurrentProcess(), t, 48 );
        function = t;
    }
    if (!page || used + 32 > 0x1000)
    {
        if (!(page = VirtualAlloc( NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE ))) return function;
        used = 0;
    }
    code = page + used;
    used += 32;
    VirtualProtect( page, 0x1000, PAGE_EXECUTE_READWRITE, &old );
    code[0] = 0x49; code[1] = 0xbb; memcpy( code + 2, &function, 8 );          /* movabs $function, %r11 */
    code[10] = 0xff; code[11] = 0x25; memset( code + 12, 0, 4 );              /* jmp *0(%rip) */
    { void *common = linux_entry_common; memcpy( code + 16, &common, 8 ); }
    FlushInstructionCache( GetCurrentProcess(), code, 32 );
    return code;
}

/* ---------------------------------------------------------------------------------------------------------
 * Set-up
 */

void linuxhost_trace( const char *format, ... )
{
    static int enabled = -1;
    char line[512];
    DWORD written;
    va_list args;
    int len;

    if (enabled == -1) enabled = GetEnvironmentVariableA( "LINUXHOST_TRACE", NULL, 0 ) > 0;
    if (!enabled) return;
    len = snprintf( line, sizeof(line), "linuxhost:%04lx:%04lx: ", GetCurrentProcessId(), GetCurrentThreadId() );
    va_start( args, format );
    len += vsnprintf( line + len, sizeof(line) - len, format, args );
    va_end( args );
    len = min( len, (int)sizeof(line) - 1 );
    {
        /* LINUXHOST_TRACE_FILE: appended to, for processes whose standard error goes nowhere (Steam's games) */
        static HANDLE file = INVALID_HANDLE_VALUE;
        static int checked;
        char path[MAX_PATH];

        if (!checked)
        {
            checked = 1;
            if (GetEnvironmentVariableA( "LINUXHOST_TRACE_FILE", path, sizeof(path) ))
                file = CreateFileA( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL );
        }
        if (file != INVALID_HANDLE_VALUE) WriteFile( file, line, len, &written, NULL );
    }
    WriteFile( GetStdHandle( STD_ERROR_HANDLE ), line, len, &written, NULL );
}

/* Linux keeps a process's descriptors across exec. The in-memory files open at exec are written to temporary
 * files and named in LINUXHOST_INHERIT ("fd:seals:path;..."), which the new process's host reads at start. */
void linuxhost_export_fds(void)
{
    char list[2048] = "", tmp_dir[MAX_PATH], path[MAX_PATH];
    int fd;

    GetTempPathA( sizeof(tmp_dir), tmp_dir );
    EnterCriticalSection( &fd_lock );
    for (fd = FIRST_FD; fd < MAX_FDS; fd++)
    {
        HANDLE file;
        DWORD written;
        size_t len = strlen( list );

        if (fds[fd].kind != FD_MEMORY) continue;
        if (!GetTempFileNameA( tmp_dir, "lfd", 0, path )) continue;
        file = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        WriteFile( file, fds[fd].data, fds[fd].size, &written, NULL );
        CloseHandle( file );
        snprintf( list + len, sizeof(list) - len, "%d:%u:%s;", fd, fds[fd].seals, path );
        linuxhost_trace( "exec: descriptor %d (%s, %zu bytes) passed on\n", fd, fds[fd].name, fds[fd].size );
    }
    LeaveCriticalSection( &fd_lock );
    SetEnvironmentVariableA( "LINUXHOST_INHERIT", list[0] ? list : NULL );
}

static void import_fds(void)
{
    char list[2048], *entry, *next;

    if (!GetEnvironmentVariableA( "LINUXHOST_INHERIT", list, sizeof(list) )) return;
    SetEnvironmentVariableA( "LINUXHOST_INHERIT", NULL );  /* this process's own children get their own list */
    for (entry = list; *entry; entry = next)
    {
        int fd;
        unsigned int seals;
        char path[MAX_PATH];
        HANDLE file;
        LARGE_INTEGER size;
        DWORD got;

        if (!(next = strchr( entry, ';' ))) break;
        *next++ = 0;
        if (sscanf( entry, "%d:%u:%259[^;]", &fd, &seals, path ) != 3 || fd < FIRST_FD || fd >= MAX_FDS) continue;
        file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        GetFileSizeEx( file, &size );
        memset( &fds[fd], 0, sizeof(fds[fd]) );
        fds[fd].kind = FD_MEMORY;
        snprintf( fds[fd].name, sizeof(fds[fd].name), "inherited" );
        if (size.QuadPart && reserve( &fds[fd], size.QuadPart ))
        {
            ReadFile( file, fds[fd].data, size.QuadPart, &got, NULL );
            fds[fd].size = got;
        }
        fds[fd].seals = seals;
        CloseHandle( file );
        linuxhost_trace( "descriptor %d inherited (%zu bytes)\n", fd, fds[fd].size );
    }
}

int linuxhost_process_init( const struct libc_export *exports, unsigned int count )
{
    import_fds();
    /* glibc's canary has a zero low byte, so string functions cannot copy past it */
    while (!(stack_guard >> 8)) stack_guard = ((unsigned long long)rand() << 40 ^ (unsigned long long)rand() << 20 ^
                                               rand() ^ GetTickCount64() << 8 ^ (unsigned long long)&count) & ~0xffull;
    pointer_guard = stack_guard ^ 0x5bd1e995a5a5a5a5ull;
    linuxhost_thread_init();
    if (!build_libc( exports, count )) return 0;
    linuxhost_trace( "libc.so.6 made up at %p\n", libc_image );
    return 1;
}
