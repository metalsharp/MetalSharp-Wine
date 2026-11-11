/* Linux module host, native: load an aarch64 Linux library on macOS and report what it needs.
 *
 *   eoshost inspect <library.so>   load it and list the imports nobody provides yet
 *   eoshost eos <library.so>       also run its constructors and start Epic Online Services in it */
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

#include "elfload.h"
#include "glibc.h"

extern char **environ;

/* EOS_Logging and EOS_Initialize, from Epic's eos_logging_types.h and eos_types.h */
struct eos_log_message { const char *category, *message; int32_t level; };
struct eos_initialize_options
{
    int32_t api_version;
    void *allocate, *reallocate, *release;
    const char *product_name, *product_version;
    void *reserved, *system_initialize_options, *override_thread_affinity;
};

static void log_callback( const struct eos_log_message *msg )
{
    printf( "  [eos %s] %s\n", msg->category, msg->message );
}

/* where a crash happened, by symbol in a Linux library or offset in it */
static void describe( const char *what, uint64_t addr )
{
    struct elf_module *module = elf_find_address( (void *)addr );
    const char *name;
    void *start, *base;
    size_t size;

    if (!module)
    {
        fprintf( stderr, "  %s %#llx\n", what, addr );
        return;
    }
    elf_get_range( module, &base, &size );
    if (elf_symbol_for_address( (void *)addr, &name, &start ))
        fprintf( stderr, "  %s %#llx: %s+%#llx (%s+%#llx)\n", what, addr, name, addr - (uint64_t)start,
                 elf_get_name( module ), addr - elf_get_base( module ) );
    else
        fprintf( stderr, "  %s %#llx: %s+%#llx\n", what, addr, elf_get_name( module ), addr - elf_get_base( module ) );
}

static void crash_handler( int sig, siginfo_t *info, void *context )
{
    ucontext_t *uc = context;
    uint64_t fp = uc->uc_mcontext->__ss.__fp;

    if (sig == SIGTRAP)
    {
        fprintf( stderr, "eoshost: breakpoint\n" );
        describe( "pc", uc->uc_mcontext->__ss.__pc );
        describe( "lr", uc->uc_mcontext->__ss.__lr );
        for (int i = 0; i < 31; i++)
            fprintf( stderr, "  x%d %#llx%s", i, uc->uc_mcontext->__ss.__x[i], i % 4 == 3 ? "\n" : "" );
        fprintf( stderr, "\n  sp %#llx\n", uc->uc_mcontext->__ss.__sp );
        _exit( 128 + sig );
    }

    fprintf( stderr, "eoshost: signal %d at address %p\n", sig, info->si_addr );
    describe( "pc", uc->uc_mcontext->__ss.__pc );
    describe( "lr", uc->uc_mcontext->__ss.__lr );
    for (int i = 0; i < 12 && fp && !(fp & 7); i++)
    {
        uint64_t *frame = (uint64_t *)fp;

        describe( "frame", frame[1] );
        fp = frame[0];
    }
    for (int i = 0; i < 29; i++)
    {
        uint64_t x = uc->uc_mcontext->__ss.__x[i];

        if (elf_find_address( (void *)x )) { char what[8]; snprintf( what, sizeof(what), "x%d", i ); describe( what, x ); }
    }
    {
        /* return addresses into the library on the stack */
        uint64_t *sp = (uint64_t *)uc->uc_mcontext->__ss.__sp;
        int found = 0;

        fprintf( stderr, "  sp %p, thread %p\n", (void *)sp, (void *)pthread_self() );
        for (int i = 0; i < 2048 && found < 16; i++)
        {
            struct elf_module *m = elf_find_address( (void *)sp[i] );
            void *b; size_t s;

            if (!m) continue;
            elf_get_range( m, &b, &s );
            /* code addresses only: the library's executable segment starts at 0x45d000 here; good enough to filter */
            describe( "stack", sp[i] );
            found++;
        }
    }
    _exit( 128 + sig );
}

static void report_unresolved( const char *name )
{
    fprintf( stderr, "eoshost: the library called %s, which is not provided yet\n", name );
}

int main( int argc, char **argv )
{
    const struct elf_stats *stats;
    struct elf_module *module;
    const char *name;
    char err[1024];
    unsigned int i;

    if (argc < 3 || (strcmp( argv[1], "inspect" ) && strcmp( argv[1], "eos" ) && strcmp( argv[1], "proc" )))
    {
        fprintf( stderr, "usage: %s inspect|eos <library.so>\n", argv[0] );
        return 2;
    }
    struct sigaction sa = { .sa_sigaction = crash_handler, .sa_flags = SA_SIGINFO };

    sigaction( SIGSEGV, &sa, NULL );
    sigaction( SIGBUS, &sa, NULL );
    sigaction( SIGILL, &sa, NULL );
    sigaction( SIGTRAP, &sa, NULL );
    setvbuf( stdout, NULL, _IONBF, 0 );
    elf_unresolved_handler = report_unresolved;
    if (!(module = elf_load( argv[2], glibc_resolve, NULL, err, sizeof(err) )))
    {
        fprintf( stderr, "eoshost: %s\n", err );
        return 1;
    }
    glibc_module_loaded( module );
    /* LINUXHOST_BREAK=<offset>[,<offset>...]: a brk at each, to see whether and how the library gets there */
    if (getenv( "LINUXHOST_BREAK" ))
    {
        char *list = strdup( getenv( "LINUXHOST_BREAK" ) ), *tok;
        size_t page = getpagesize();

        for (tok = strtok( list, "," ); tok; tok = strtok( NULL, "," ))
        {
            uint32_t *insn = (uint32_t *)(elf_get_base( module ) + strtoull( tok, NULL, 16 ));
            void *start = (void *)((uintptr_t)insn & ~(page - 1));

            mprotect( start, page, PROT_READ | PROT_WRITE );
            *insn = 0xd4200000;  /* brk #0 */
            mprotect( start, page, PROT_READ | PROT_EXEC );
            sys_icache_invalidate( insn, 4 );
        }
    }
    stats = elf_get_stats( module );
    printf( "%s: %u relocations, %u imports resolved, %u unresolved, %u weak left at 0, %u unsupported\n",
            elf_get_name( module ), stats->relocations, stats->imports, stats->unresolved, stats->weak_missing,
            stats->unsupported );
    for (i = 0; (name = elf_unresolved_name( module, i )); i++) printf( "%s%s", i ? " " : "unresolved: ", name );
    if (i) printf( "\n" );
    if (!strcmp( argv[1], "inspect" )) return 0;
    if (!strcmp( argv[1], "proc" ))
    {
        /* the Linux files the library would read, through the library's own open and read */
        static const char *const paths[] = { "/proc/self/status", "/proc/self/cmdline", "/proc/meminfo",
                                             "/etc/machine-id", "/proc/self/maps" };
        int (*lopen)( const char *, int, unsigned int ) = glibc_resolve( "open", NULL, NULL );
        long (*lread)( int, void *, size_t ) = glibc_resolve( "read", NULL, NULL );
        long (*lreadlink)( const char *, char *, size_t ) = glibc_resolve( "readlink", NULL, NULL );
        char buf[65536], link[1024];
        long n = lreadlink( "/proc/self/exe", link, sizeof(link) - 1 );

        printf( "/proc/self/exe -> %.*s\n", (int)(n > 0 ? n : 0), link );
        for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        {
            int fd = lopen( paths[i], 0, 0 );
            long len = fd >= 0 ? lread( fd, buf, sizeof(buf) - 1 ) : -1;

            if (len < 0) { printf( "%s: errno %d\n", paths[i], errno ); continue; }
            buf[len] = 0;
            for (long j = 0; j < len; j++) if (!buf[j]) buf[j] = ' ';
            printf( "== %s (%ld bytes)\n%.700s%s\n", paths[i], len, buf, len > 700 ? "..." : "" );
        }
        return 0;
    }

    {
        const char *(*get_version)( void ) = elf_symbol( module, "EOS_GetVersion" );
        const char *(*result_to_string)( int ) = elf_symbol( module, "EOS_EResult_ToString" );
        int (*set_log_callback)( void (*)(const struct eos_log_message *) ) = elf_symbol( module, "EOS_Logging_SetCallback" );
        int (*set_log_level)( int, int ) = elf_symbol( module, "EOS_Logging_SetLogLevel" );
        int (*initialize)( const struct eos_initialize_options * ) = elf_symbol( module, "EOS_Initialize" );
        int (*shutdown)( void ) = elf_symbol( module, "EOS_Shutdown" );
        struct eos_initialize_options options = { 4 };
        int ret;

        printf( "running the constructors\n" );
        elf_run_constructors( module, 1, argv + 2, environ );
        printf( "constructors done\n" );
        printf( "EOS_GetVersion: %s\n", get_version() );
        printf( "EOS_EResult_ToString(0): %s\n", result_to_string( 0 ) );
        printf( "EOS_Logging_SetCallback: %s\n", result_to_string( set_log_callback( log_callback ) ) );
        set_log_level( 0x7fffffff, 600 );  /* every category, verbose */
        options.product_name = "linuxhost";
        options.product_version = "1.0";
        ret = initialize( &options );
        printf( "EOS_Initialize: %s\n", result_to_string( ret ) );
        if (!ret) printf( "EOS_Shutdown: %s\n", result_to_string( shutdown() ) );
    }
    return 0;
}
