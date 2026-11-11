/* Linux module host, native: the Linux files a library reads about its process and machine.
 *
 * /proc/self (and /proc/<own pid>): exe, cmdline, status, maps; /proc/meminfo; /etc/machine-id. Opening one of
 * them gives a file with what Linux would put there, made from macOS's view of the process: the regions in maps
 * are the process's memory map, the loaded Linux libraries named by their paths. Everything else under /proc and
 * /sys is missing, as on a system without it. */
#include <fcntl.h>
#include <stdarg.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

#include "elfload.h"
#include "glibc_internal.h"

/* "/proc/self/<name>" or "/proc/<our pid>/<name>": returns <name>, or NULL */
static const char *proc_self_entry( const char *path )
{
    char prefix[64];
    size_t len;

    if (!strncmp( path, "/proc/self/", 11 )) return path + 11;
    if (!strncmp( path, "/proc/thread-self/", 18 )) return path + 18;
    len = snprintf( prefix, sizeof(prefix), "/proc/%d/", getpid() );
    if (!strncmp( path, prefix, len )) return path + len;
    return NULL;
}

static void executable_path( char *buf, size_t size )
{
    uint32_t len = (uint32_t)size;
    char tmp[4096];

    if (_NSGetExecutablePath( tmp, &len ) || !realpath( tmp, buf )) snprintf( buf, size, "/usr/bin/wine" );
}

/* a growing text buffer */
struct text { char *data; size_t len, size; };

static void append( struct text *t, const char *fmt, ... ) __attribute__((format(printf, 2, 3)));
static void append( struct text *t, const char *fmt, ... )
{
    va_list args;
    int n;

    for (;;)
    {
        va_start( args, fmt );
        n = vsnprintf( t->data + t->len, t->size - t->len, fmt, args );
        va_end( args );
        if (n >= 0 && t->len + n < t->size) break;
        t->size = t->size ? t->size * 2 + n : 4096 + n;
        if (!(t->data = realloc( t->data, t->size ))) abort();
    }
    t->len += n;
}

static void make_maps( struct text *t )
{
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    char exe[1024];

    executable_path( exe, sizeof(exe) );
    for (;;)
    {
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        struct elf_module *module;
        const char *name = "";

        if (mach_vm_region( mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                            &count, &object ) != KERN_SUCCESS)
            break;
        if ((module = elf_find_address( (void *)addr ))) name = elf_get_path( module );
        append( t, "%08llx-%08llx %c%c%c%c %08llx 00:00 0 %s%s\n", addr, addr + size,
                (info.protection & VM_PROT_READ) ? 'r' : '-', (info.protection & VM_PROT_WRITE) ? 'w' : '-',
                (info.protection & VM_PROT_EXECUTE) ? 'x' : '-', info.shared ? 's' : 'p',
                (unsigned long long)info.offset, *name ? "                          " : "", name );
        addr += size;
    }
    append( t, "fffffffff000-1000000000000 r-xp 00000000 00:00 0                          [vdso]\n" );
}

static void make_status( struct text *t )
{
    char exe[1024], *name;
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    unsigned long rss = 0, vsize = 0;
    int threads = 1;
    thread_act_array_t list;
    mach_msg_type_number_t nthreads;

    executable_path( exe, sizeof(exe) );
    name = strrchr( exe, '/' ) ? strrchr( exe, '/' ) + 1 : exe;
    if (task_info( mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count ) == KERN_SUCCESS)
    {
        rss = info.resident_size / 1024;
        vsize = info.virtual_size / 1024;
    }
    if (task_threads( mach_task_self(), &list, &nthreads ) == KERN_SUCCESS)
    {
        threads = nthreads;
        for (unsigned int i = 0; i < nthreads; i++) mach_port_deallocate( mach_task_self(), list[i] );
        vm_deallocate( mach_task_self(), (vm_address_t)list, nthreads * sizeof(*list) );
    }
    append( t, "Name:\t%.15s\nUmask:\t0022\nState:\tR (running)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n"
               "TracerPid:\t0\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t256\nGroups:\t\n"
               "VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmRSS:\t%8lu kB\nThreads:\t%d\n"
               "SigQ:\t0/63432\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
               "Seccomp:\t0\nCpus_allowed_list:\t0-%ld\nvoluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
            name, getpid(), getpid(), getppid(), getuid(), geteuid(), getuid(), getuid(), getgid(), getegid(),
            getgid(), getgid(), vsize, vsize, rss, threads, sysconf( _SC_NPROCESSORS_ONLN ) - 1 );
}

static void make_cmdline( struct text *t )
{
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();

    for (int i = 0; i < argc; i++)
    {
        append( t, "%s", argv[i] );
        append( t, "%c", 0 );
    }
    if (!t->len) append( t, "%c", 0 );
}

static void make_meminfo( struct text *t )
{
    uint64_t mem = 0;
    size_t size = sizeof(mem);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    unsigned long free_kb = 0, avail_kb = 0, page_kb = getpagesize() / 1024;

    sysctlbyname( "hw.memsize", &mem, &size, NULL, 0 );
    if (host_statistics64( mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count ) == KERN_SUCCESS)
    {
        free_kb = vm.free_count * page_kb;
        avail_kb = (vm.free_count + vm.inactive_count + vm.purgeable_count) * page_kb;
    }
    append( t, "MemTotal:       %8llu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:               0 kB\n"
               "Cached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
            mem / 1024, free_kb, avail_kb );
}

/* a stable id for the machine, derived from (not equal to) the Mac's hardware UUID, as systemd's is per install */
static void make_machine_id( struct text *t )
{
    char uuid[64] = "";
    size_t size = sizeof(uuid);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];

    sysctlbyname( "kern.uuid", uuid, &size, NULL, 0 );
    CC_SHA256( uuid, (CC_LONG)strlen( uuid ), digest );
    for (int i = 0; i < 16; i++) append( t, "%02x", digest[i] );
    append( t, "\n" );
}

/* the contents of a Linux file this layer provides, or 0 if it is not one */
static int linux_file( const char *path, struct text *t )
{
    const char *entry = proc_self_entry( path );

    memset( t, 0, sizeof(*t) );
    if (entry)
    {
        if (!strcmp( entry, "maps" )) make_maps( t );
        else if (!strcmp( entry, "status" )) make_status( t );
        else if (!strcmp( entry, "cmdline" )) make_cmdline( t );
        else return 0;
        return 1;
    }
    if (!strcmp( path, "/proc/meminfo" )) { make_meminfo( t ); return 1; }
    if (!strcmp( path, "/etc/machine-id" ) || !strcmp( path, "/var/lib/dbus/machine-id" ))
    {
        make_machine_id( t );
        return 1;
    }
    return 0;
}

/* a path under /proc, /sys or /dev that macOS would answer differently: the library should see it as missing */
static int linux_only_path( const char *path )
{
    return !strncmp( path, "/proc/", 6 ) || !strncmp( path, "/sys/", 5 ) || !strcmp( path, "/proc" ) ||
           !strcmp( path, "/etc/machine-id" ) || !strncmp( path, "/dev/input", 10 ) || !strcmp( path, "/dev/hwrng" );
}

/* An open descriptor with the file's contents, or -2 if the path is not one of these files (open it normally);
 * -1 with errno set if it is a Linux path that does not exist here. */
int glibc_proc_open( const char *path )
{
    struct text t;
    char name[] = "/tmp/linuxhost.XXXXXX";
    int fd;

    if (!path || !linux_only_path( path )) return -2;
    if (!linux_file( path, &t )) return linux_fail( 2 );  /* ENOENT */
    if ((fd = mkstemp( name )) == -1)
    {
        free( t.data );
        return linux_ret( -1 );
    }
    unlink( name );
    if (write( fd, t.data, t.len ) != (ssize_t)t.len) { free( t.data ); close( fd ); return linux_fail( 5 ); }
    lseek( fd, 0, SEEK_SET );
    free( t.data );
    return fd;
}

/* readlink: /proc/self/exe is the program; the rest of /proc is missing. Returns -2 for other paths. */
long glibc_proc_readlink( const char *path, char *buf, size_t size )
{
    const char *entry;
    char exe[1024];
    size_t len;

    if (!linux_only_path( path )) return -2;
    if (!(entry = proc_self_entry( path )) || strcmp( entry, "exe" )) return linux_fail( 2 );
    executable_path( exe, sizeof(exe) );
    len = strlen( exe );
    if (len > size) len = size;
    memcpy( buf, exe, len );
    return (long)len;
}

/* access and stat: whether a Linux path exists here. Returns -2 for other paths, else 0 or -1 (ENOENT). */
int glibc_proc_exists( const char *path )
{
    struct text t;
    const char *entry;

    if (!linux_only_path( path )) return -2;
    if (!strcmp( path, "/proc" ) || !strcmp( path, "/proc/self" )) return 0;
    if ((entry = proc_self_entry( path )) && !strcmp( entry, "exe" )) return 0;
    if (linux_file( path, &t )) { free( t.data ); return 0; }
    return linux_fail( 2 );
}
