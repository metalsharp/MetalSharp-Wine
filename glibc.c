/* Linux module host: the glibc functions Linux libraries import, on top of the Windows C runtime.
 *
 * Every function here is called by Linux code, so it uses the System V calling convention and
 * glibc's types: long is 64 bits, a va_list is a pointer to the x86_64 register save area
 * description, and a pthread_mutex_t or pthread_cond_t is a zero-initialised block of memory in
 * which a Windows SRWLOCK or CONDITION_VARIABLE is kept.
 *
 * This is the set Valve's EasyAntiCheat and BattlEye bridges import, plus what the loader's own
 * test library uses. linuxhost_missing() lists what a library needs that is not here yet. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "glibc.h"
#include "libc.h"
#include "process.h"
#include "syscall.h"

typedef unsigned long long lx_size_t;      /* size_t and unsigned long of the Linux ABI */
typedef long long lx_long;

/* how x86_64 Linux passes a va_list */
struct lx_va_list
{
    unsigned int gp_offset, fp_offset;
    void *overflow_arg_area, *reg_save_area;
};

static unsigned long long va_next( struct lx_va_list *ap )
{
    unsigned long long value;

    if (ap->gp_offset < 48)
    {
        value = *(unsigned long long *)((char *)ap->reg_save_area + ap->gp_offset);
        ap->gp_offset += 8;
    }
    else
    {
        value = *(unsigned long long *)ap->overflow_arg_area;
        ap->overflow_arg_area = (char *)ap->overflow_arg_area + 8;
    }
    return value;
}

/* ---- memory and strings ---- */

static SYSV void *lx_malloc( lx_size_t size ) { return malloc( size ); }
static SYSV void *lx_calloc( lx_size_t count, lx_size_t size ) { return calloc( count, size ); }
static SYSV void *lx_realloc( void *ptr, lx_size_t size ) { return realloc( ptr, size ); }
static SYSV void lx_free( void *ptr ) { free( ptr ); }
static SYSV void *lx_memcpy( void *dst, const void *src, lx_size_t n ) { return memcpy( dst, src, n ); }
static SYSV void *lx_memmove( void *dst, const void *src, lx_size_t n ) { return memmove( dst, src, n ); }
static SYSV void *lx_memset( void *dst, int c, lx_size_t n ) { return memset( dst, c, n ); }
static SYSV int lx_memcmp( const void *a, const void *b, lx_size_t n )
{
    int ret = memcmp( a, b, n );

    linuxhost_trace( "memcmp \"%.*s\" \"%.*s\" %llu: %d\n", (int)min( n, 64 ), (const char *)a, (int)min( n, 64 ),
                     (const char *)b, n, ret );
    return ret;
}
static SYSV lx_size_t lx_strlen( const char *s ) { return strlen( s ); }
static SYSV char *lx_strcpy( char *dst, const char *src ) { return strcpy( dst, src ); }
static SYSV char *lx_strncpy( char *dst, const char *src, lx_size_t n ) { return strncpy( dst, src, n ); }
static SYSV char *lx_strcat( char *dst, const char *src ) { return strcat( dst, src ); }
static SYSV int lx_strcmp( const char *a, const char *b )
{
    int ret = strcmp( a, b );

    linuxhost_trace( "strcmp \"%.80s\" \"%.80s\": %d\n", a, b, ret );
    return ret;
}
static SYSV int lx_strncmp( const char *a, const char *b, lx_size_t n ) { return strncmp( a, b, n ); }
static SYSV char *lx_strchr( const char *s, int c ) { return strchr( s, c ); }
static SYSV char *lx_strrchr( const char *s, int c ) { return strrchr( s, c ); }
static SYSV char *lx_strstr( const char *s, const char *sub )
{
    char *ret = strstr( s, sub );

    linuxhost_trace( "strstr \"%.80s\" \"%.80s\": %s\n", s, sub, ret ? "found" : "no" );
    return ret;
}

static SYSV char *lx_strdup( const char *s )
{
    char *copy = malloc( strlen( s ) + 1 );

    if (copy) strcpy( copy, s );
    return copy;
}

/* ---- formatted output: the conversions the bridges' messages use ---- */

static void put_char( char **out, char *end, char c, int *total )
{
    if (*out < end) *(*out)++ = c;
    (*total)++;
}

static void put_string( char **out, char *end, const char *s, int *total )
{
    while (*s) put_char( out, end, *s++, total );
}

static SYSV int lx_vsnprintf( char *buffer, lx_size_t size, const char *format, struct lx_va_list *ap )
{
    char *out = buffer, *end = size ? buffer + size - 1 : buffer, number[32];
    int total = 0;

    for (; *format; format++)
    {
        int is_long = 0;

        if (*format != '%')
        {
            put_char( &out, end, *format, &total );
            continue;
        }
        format++;
        while (*format == '#' || *format == '0' || *format == '-' || (*format >= '1' && *format <= '9')) format++;
        while (*format == 'l' || *format == 'z')
        {
            is_long = 1;
            format++;
        }
        switch (*format)
        {
        case 's':
        {
            const char *s = (const char *)va_next( ap );
            put_string( &out, end, s ? s : "(null)", &total );
            break;
        }
        case 'd':
        case 'i':
            if (is_long) snprintf( number, sizeof(number), "%lld", (long long)va_next( ap ) );
            else snprintf( number, sizeof(number), "%d", (int)va_next( ap ) );
            put_string( &out, end, number, &total );
            break;
        case 'u':
            if (is_long) snprintf( number, sizeof(number), "%llu", va_next( ap ) );
            else snprintf( number, sizeof(number), "%u", (unsigned int)va_next( ap ) );
            put_string( &out, end, number, &total );
            break;
        case 'x':
        case 'X':
            if (is_long) snprintf( number, sizeof(number), "%llx", va_next( ap ) );
            else snprintf( number, sizeof(number), "%x", (unsigned int)va_next( ap ) );
            put_string( &out, end, number, &total );
            break;
        case 'p':
            snprintf( number, sizeof(number), "0x%llx", va_next( ap ) );
            put_string( &out, end, number, &total );
            break;
        case 'c':
            put_char( &out, end, (char)va_next( ap ), &total );
            break;
        case '%':
            put_char( &out, end, '%', &total );
            break;
        case 0:
            format--;
            break;
        default:
            put_char( &out, end, '?', &total );
            break;
        }
    }
    if (size) *out = 0;
    return total;
}

static SYSV int lx_puts( const char *s )
{
    DWORD written;

    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), s, strlen( s ), &written, NULL );
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), "\n", 1, &written, NULL );
    return 1;
}

static SYSV lx_long lx_write( int fd, const void *data, lx_size_t size )
{
    DWORD written;

    if (fd != 1 && fd != 2) return -1;
    if (!WriteFile( GetStdHandle( fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE ), data, size, &written, NULL ))
        return -1;
    return written;
}

static SYSV char *lx_getenv( const char *name ) { return getenv( name ); }

/* ---- threads: glibc's zero-initialised mutex and condition variable hold the Windows ones ---- */

static SYSV int lx_pthread_mutex_lock( void *mutex ) { AcquireSRWLockExclusive( mutex ); return 0; }
static SYSV int lx_pthread_mutex_unlock( void *mutex ) { ReleaseSRWLockExclusive( mutex ); return 0; }
static SYSV int lx_pthread_cond_signal( void *cond ) { WakeConditionVariable( cond ); return 0; }
static SYSV int lx_pthread_cond_broadcast( void *cond ) { WakeAllConditionVariable( cond ); return 0; }

static SYSV int lx_pthread_cond_wait( void *cond, void *mutex )
{
    SleepConditionVariableSRW( cond, mutex, INFINITE, 0 );
    return 0;
}

/* ---- the dynamic linker ---- */

struct lx_tls_index { unsigned long long module, offset; };

static SYSV void *lx_tls_get_addr( struct lx_tls_index *index )
{
    struct elf_module *module = elf_module_by_tls_id( index->module );

    return module ? (char *)elf_tls_block( module ) + index->offset : NULL;
}

static char dl_error[256];

static SYSV void *lx_dlopen( const char *path, int flags )
{
    struct elf_module *module;
    const void *data;
    size_t size;
    int fd;

    linuxhost_trace( "dlopen %s %#x\n", path ? path : "(null)", flags );
    linuxhost_thread_init();
    if (!path) return NULL;
    if ((module = elf_find( path ))) return module;
    /* an in-memory file, as EAC's bootstrapper passes the module */
    if (sscanf( path, "/proc/self/fd/%d", &fd ) == 1)
    {
        if (!linuxhost_fd_contents( fd, &data, &size ))
        {
            snprintf( dl_error, sizeof(dl_error), "%s: no such file", path );
            return NULL;
        }
        {
            /* LINUXHOST_DUMP=<path>: keep a copy of what was handed over, to look at */
            char dump[MAX_PATH];
            HANDLE file;
            DWORD written;

            if (GetEnvironmentVariableA( "LINUXHOST_DUMP", dump, sizeof(dump) ) &&
                (file = CreateFileA( dump, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL )) != INVALID_HANDLE_VALUE)
            {
                WriteFile( file, data, size, &written, NULL );
                CloseHandle( file );
                linuxhost_trace( "saved %s to %s\n", path, dump );
            }
        }
        module = elf_load_buffer( path, data, size, linuxhost_resolve, NULL, dl_error, sizeof(dl_error) );
    }
    else module = elf_load( path, linuxhost_resolve, NULL, dl_error, sizeof(dl_error) );
    if (!module)
    {
        linuxhost_trace( "dlopen %s failed: %s\n", path, dl_error );
        return NULL;
    }
    {
        const struct elf_stats *stats = elf_get_stats( module );

        linuxhost_trace( "loaded %s: %u relocations, %u imports, %u unresolved (%s), %u unsupported\n", path,
                         stats->relocations, stats->imports, stats->unresolved, stats->unresolved_names,
                         stats->unsupported );
    }
    {
        /* the name /proc/self/maps shows it under, as Linux would */
        char maps_name[256];
        void *base;
        size_t size;

        elf_get_range( module, &base, &size );
        if (sscanf( path, "/proc/self/fd/%d", &fd ) == 1 && linuxhost_fd_maps_name( fd, maps_name, sizeof(maps_name) ))
            linuxhost_note_module_path( base, size, maps_name );
        else
            linuxhost_note_module_path( base, size, path );
    }
    linuxhost_register_syscalls( module );
    elf_run_constructors( module );
    return module;
}

static SYSV void *lx_dlsym( void *handle, const char *name )
{
    void *addr = handle ? elf_symbol( handle, name ) : NULL;

    if (addr && elf_symbol_is_function( handle, name )) addr = linuxhost_entry_stub( addr, name );
    linuxhost_trace( "dlsym %p %s: %p\n", handle, name, addr );
    if (!addr) snprintf( dl_error, sizeof(dl_error), "undefined symbol: %s", name );
    return addr;
}

static SYSV int lx_dlclose( void *handle ) { return 0; }

static SYSV char *lx_dlerror(void)
{
    static char last[256];

    if (!dl_error[0]) return NULL;
    strcpy( last, dl_error );
    dl_error[0] = 0;
    return last;
}

static SYSV int lx_cxa_atexit( void (*fn)(void *), void *arg, void *dso ) { return 0; }
static SYSV void lx_cxa_finalize( void *dso ) {}

/* Wine's debug channels, which a unix library built for Wine imports from ntdll.so: off */
static SYSV int lx_wine_dbg_header( int cls, void *channel, const char *function ) { return -1; }
static SYSV int lx_wine_dbg_output( const char *str ) { return 0; }

static const struct { const char *name; void *function; } functions[] =
{
    { "malloc", lx_malloc }, { "calloc", lx_calloc }, { "realloc", lx_realloc }, { "free", lx_free },
    { "memcpy", lx_memcpy }, { "memmove", lx_memmove }, { "memset", lx_memset }, { "memcmp", lx_memcmp },
    { "strlen", lx_strlen }, { "strcpy", lx_strcpy }, { "strncpy", lx_strncpy }, { "strcat", lx_strcat },
    { "strcmp", lx_strcmp }, { "strncmp", lx_strncmp }, { "strchr", lx_strchr }, { "strrchr", lx_strrchr },
    { "strstr", lx_strstr }, { "strdup", lx_strdup },
    { "vsnprintf", lx_vsnprintf }, { "puts", lx_puts }, { "write", lx_write }, { "getenv", lx_getenv },
    { "pthread_mutex_lock", lx_pthread_mutex_lock }, { "pthread_mutex_unlock", lx_pthread_mutex_unlock },
    { "pthread_cond_wait", lx_pthread_cond_wait }, { "pthread_cond_signal", lx_pthread_cond_signal },
    { "pthread_cond_broadcast", lx_pthread_cond_broadcast },
    { "__tls_get_addr", lx_tls_get_addr },
    { "dlopen", lx_dlopen }, { "dlsym", lx_dlsym }, { "dlclose", lx_dlclose }, { "dlerror", lx_dlerror },
    { "__cxa_atexit", lx_cxa_atexit }, { "__cxa_finalize", lx_cxa_finalize },
    { "__wine_dbg_header", lx_wine_dbg_header }, { "__wine_dbg_output", lx_wine_dbg_output },
};

void *linuxhost_glibc_function( const char *name )
{
    unsigned int i;

    for (i = 0; i < sizeof(functions) / sizeof(functions[0]); i++)
        if (!strcmp( functions[i].name, name )) return functions[i].function;
    return NULL;
}

/* what the made-up libc.so.6 exports (process.c) */
const struct libc_export *linuxhost_libc_exports( unsigned int *count )
{
    static const struct libc_export exports[] =
    {
        { "dlopen", lx_dlopen }, { "__libc_dlopen_mode", lx_dlopen }, { "dlsym", lx_dlsym },
        { "__libc_dlsym", lx_dlsym }, { "dlclose", lx_dlclose }, { "dlerror", lx_dlerror },
    };

    *count = sizeof(exports) / sizeof(exports[0]);
    return exports;
}

void *linuxhost_resolve( const char *name, void *context )
{
    const struct libc_function *f;
    unsigned int i;

    for (f = linuxhost_libc_functions; f->name; f++) if (!strcmp( f->name, name )) return f->function;
    for (f = linuxhost_libc_io_functions; f->name; f++) if (!strcmp( f->name, name )) return f->function;
    for (f = linuxhost_fork_functions; f->name; f++) if (!strcmp( f->name, name )) return f->function;
    for (i = 0; i < sizeof(functions) / sizeof(functions[0]); i++)
        if (!strcmp( functions[i].name, name )) return functions[i].function;
    return NULL;
}

unsigned int linuxhost_function_count(void)
{
    return sizeof(functions) / sizeof(functions[0]);
}
