/* Test of the Linux module host: an x86_64 Windows program that loads Linux x86_64 libraries.
 *
 *   hosttest.exe hello <path to hello.so>     load the test library and call into it
 *   hosttest.exe extras <path to extras.so>   the unwinder, and glibc's sockets, threads, locales and files
 *   hosttest.exe load <library.so>...         load libraries, report what they import, run their constructors
 *   hosttest.exe inspect <library.so>...      the same, without running any of their code
 *
 * Paths are Windows paths or Unix paths starting with '/'. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../elfload.h"
#include "../glibc.h"
#include "../syscall.h"

static SYSV long add_callback( long a, long b ) { return a * 100 + b; }

static int report( struct elf_module *module )
{
    const struct elf_stats *stats = elf_get_stats( module );
    void *base;
    size_t size;

    elf_get_range( module, &base, &size );
    printf( "%s: loaded at %p, %zu KB; %u relocations, %u imports resolved, %u unresolved, %u weak left empty, "
            "%u unsupported relocations\n", elf_get_name( module ), base, size / 1024, stats->relocations,
            stats->imports, stats->unresolved, stats->weak_missing, stats->unsupported );
    if (stats->unresolved) printf( "  unresolved: %s\n", stats->unresolved_names );
    return stats->unresolved || stats->unsupported;
}

static int test_hello( const char *path )
{
    typedef long (SYSV *host_test_fn)( const char * );
    typedef double (SYSV *host_args_fn)( long, long, long, long, long, long, double, double );
    typedef long (SYSV *host_callback_fn)( long (SYSV *)( long, long ), long, long );
    char err[256];
    struct elf_module *module = elf_load( path, linuxhost_resolve, NULL, err, sizeof(err) );
    host_test_fn host_test;
    host_args_fn host_args;
    host_callback_fn host_callback;
    int failures = 0;
    long a, b, c;
    double d;

    if (!module)
    {
        printf( "FAIL  load: %s\n", err );
        return 1;
    }
    failures += report( module );
    elf_run_constructors( module );
    host_test = elf_symbol( module, "host_test" );
    host_args = elf_symbol( module, "host_args" );
    host_callback = elf_symbol( module, "host_callback" );
    if (!host_test || !host_args || !host_callback)
    {
        printf( "FAIL  exports not found\n" );
        return 1;
    }
    a = host_test( "abc" );     /* 3 + 41 + 7 */
    b = host_test( "abcde" );   /* 5 + 41 + 8 */
    printf( "%s  host_test: %ld, %ld (expect 51, 54)\n", a == 51 && b == 54 ? "ok  " : "FAIL", a, b );
    failures += !(a == 51 && b == 54);
    d = host_args( 1, 2, 3, 4, 5, 6, 1.5, 4.0 );  /* 1+4+9+16+25+36 + 6 */
    printf( "%s  host_args: %.1f (expect 97.0)\n", d == 97.0 ? "ok  " : "FAIL", d );
    failures += d != 97.0;
    c = host_callback( add_callback, 7, 9 );      /* 709 + 1 */
    printf( "%s  host_callback: %ld (expect 710)\n", c == 710 ? "ok  " : "FAIL", c );
    failures += c != 710;
    /* a direct Linux system call (getpid) made by the library reaches the host, when Wine routes them */
    if (linuxhost_register_syscalls( module ))
    {
        typedef long (SYSV *raw_fn)( void );
        raw_fn raw_getpid = elf_symbol( module, "host_raw_getpid" ), raw_unknown = elf_symbol( module, "host_raw_unknown" );
        long pid = raw_getpid(), unknown = raw_unknown();

        printf( "%s  direct getpid: %ld (expect %lu)\n", pid == (long)GetCurrentProcessId() ? "ok  " : "FAIL", pid,
                GetCurrentProcessId() );
        failures += pid != (long)GetCurrentProcessId();
        printf( "%s  direct unknown call: %ld (expect -38, ENOSYS), %u reported\n",
                unknown == -38 && linuxhost_unimplemented_syscalls() == 1 ? "ok  " : "FAIL", unknown,
                linuxhost_unimplemented_syscalls() );
        failures += !(unknown == -38 && linuxhost_unimplemented_syscalls() == 1);
        /* the library's other registers survive the call */
        d = host_args( 1, 2, 3, 4, 5, 6, 1.5, 4.0 );
        failures += d != 97.0;
    }
    else printf( "skip  direct system calls: this Wine does not route them\n" );
    return failures;
}

static int test_extras( const char *path )
{
    static const char *const checks[] = { "rand (glibc's sequence)", "mbrtowc", "wcrtomb", "socket send and recv",
                                           "poll", "epoll", "glob /proc/self/fd", "/dev/urandom", "/proc/cpuinfo",
                                           "pthread_create and join", "mmap and munmap", "the C locale's tables" };
    typedef long (SYSV *test_fn)( void );
    char err[256];
    struct elf_module *module = elf_load( path, linuxhost_resolve, NULL, err, sizeof(err) );
    test_fn unwind_test, backtrace_test, libc_test;
    int failures = 0;
    unsigned int i;
    long r;

    if (!module)
    {
        printf( "FAIL  load: %s\n", err );
        return 1;
    }
    failures += report( module );
    elf_run_constructors( module );
    unwind_test = elf_symbol( module, "unwind_test" );
    backtrace_test = elf_symbol( module, "backtrace_test" );
    libc_test = elf_symbol( module, "libc_test" );
    if (!unwind_test || !backtrace_test || !libc_test)
    {
        printf( "FAIL  exports not found\n" );
        return 1;
    }
    r = unwind_test();
    printf( "%s  exception through a cleanup to a handler: %ld (expect 7045)\n", r == 7045 ? "ok  " : "FAIL", r );
    failures += r != 7045;
    r = backtrace_test();
    printf( "%s  backtrace: %ld (expect 502: the end of the stack after two frames)\n", r == 502 ? "ok  " : "FAIL", r );
    failures += r != 502;
    r = libc_test();
    for (i = 0; i < ARRAY_SIZE(checks); i++)
    {
        printf( "%s  %s\n", (r >> i) & 1 ? "ok  " : "FAIL", checks[i] );
        failures += !((r >> i) & 1);
    }
    return failures;
}

static int test_load( int count, char **paths, int run )
{
    char err[256];
    int i, failures = 0;

    for (i = 0; i < count; i++)
    {
        struct elf_module *module = elf_load( paths[i], linuxhost_resolve, NULL, err, sizeof(err) );
        void **funcs;

        if (!module)
        {
            printf( "FAIL  %s\n", err );
            failures++;
            continue;
        }
        failures += report( module );
        if (run && !elf_get_stats( module )->unresolved)
        {
            elf_run_constructors( module );
            printf( "  constructors ran\n" );
        }
        /* a Wine unix library exports its entry points as a table */
        if ((funcs = elf_symbol( module, "__wine_unix_call_funcs" )))
        {
            void *base;
            size_t size;
            int n = 0;

            elf_get_range( module, &base, &size );
            while (n < 64 && (char *)funcs[n] >= (char *)base && (char *)funcs[n] < (char *)base + size) n++;
            printf( "  __wine_unix_call_funcs: %d entry points inside the library\n", n );
        }
    }
    return failures;
}

int main( int argc, char **argv )
{
    int failures;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc >= 3 && !strcmp( argv[1], "hello" )) failures = test_hello( argv[2] );
    else if (argc >= 3 && !strcmp( argv[1], "extras" )) failures = test_extras( argv[2] );
    else if (argc >= 3 && !strcmp( argv[1], "load" )) failures = test_load( argc - 2, argv + 2, 1 );
    else if (argc >= 3 && !strcmp( argv[1], "inspect" )) failures = test_load( argc - 2, argv + 2, 0 );
    else
    {
        printf( "usage: hosttest hello <hello.so> | extras <extras.so> | load <library.so>... | inspect <library.so>...\n" );
        return 2;
    }
    printf( "%d failure(s); the host provides %u glibc functions\n", failures, linuxhost_function_count() );
    return failures ? 1 : 0;
}
